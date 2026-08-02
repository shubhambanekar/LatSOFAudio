//
// LatSOFKernelAudio.hpp — kernel-published audio device for LatSOFAudio.
//
// WHY THIS EXISTS: Siri refuses input devices that don't look like real
// microphones. Disassembly of AVFAudio/CoreSpeech (Sequoia 15.7.7, 28 Jul
// 2026) shows the actual gate: a device with transport 'bltn' must expose
// an input-scope HAL DataSource reading 'imic' (internal mic) — USB
// transport skips the check, which is why a USB dongle always passed.
// The original hypothesis (kernel vs userspace device class) was wrong:
// the old HAL-plugin mic failed because it published no data source, not
// because of what it was. Both layers of the gate are property-based:
//   AVVCAudioDeviceManager (IsDeviceBuiltIn / GetAudioDeviceBuiltInMicrophone)
//   CSSiriRecordingInfo ('bltn' route requires 'imic'/'emic', else nil)
//
// The kernel port still earns its keep: AMFI blocks our unsigned plugin
// on this system, IOAudioFamily hands us the 'imic' selector, transport,
// and terminal-type plumbing for free, and the same DMA ring serves both
// paths. Same gated start/stop, same demand-latch and recovery semantics
// — the only new thing is who tells CoreAudio about it.
//
// Part of LatSOFAudio: https://github.com/shubhambanekar/LatSOFAudio
// Copyright (c) 2026 Shubham Banekar — BSD-3-Clause.
//

#ifndef LatSOFKernelAudio_hpp
#define LatSOFKernelAudio_hpp

#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/audio/IOAudioEngine.h>
#include <IOKit/audio/IOAudioStream.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <IOKit/audio/IOAudioSelectorControl.h>

class LatSOFAudioDevice;
class LatSOFKernelAudioEngine;

// Thin IOAudioDevice: exists so IOAudioFamily has a device node to hang the
// engine on. All hardware ownership stays with LatSOFAudioDevice (the
// owner); this class holds a non-retained back-pointer set before start.
class LatSOFKernelAudioDevice : public IOAudioDevice {
    OSDeclareDefaultStructors(LatSOFKernelAudioDevice)
    friend class LatSOFKernelAudioEngine;

public:
    virtual bool initHardware(IOService *provider) override;
    virtual IOWorkLoop *getWorkLoop() const override;
    // review 1 Aug round 3: the engine member is unretained after
    // activateAudioEngine and the family frees it inside stop()'s
    // deactivateAllAudioEngines — while the owner's gated readers (wake
    // re-latch, jackPoll executor) may still call getEngine(). This
    // override tells the owner, under its gate, to drop the resume latch
    // and null the pointer BEFORE the free. Covers IOKit-initiated
    // termination too, which never goes through the owner's stop().
    virtual void stop(IOService *provider) override;

    void setOwner(LatSOFAudioDevice *o) { owner = o; }
    LatSOFKernelAudioEngine *getEngine() const { return engine; }
    void clearEngine() { engine = nullptr; }   // gated callers only

private:
    LatSOFAudioDevice *owner;   // not retained; owner outlives us by design
    LatSOFKernelAudioEngine *engine;
};

class LatSOFKernelAudioEngine : public IOAudioEngine {
    OSDeclareDefaultStructors(LatSOFKernelAudioEngine)

public:
    bool initWithOwner(LatSOFAudioDevice *o);

    virtual bool initHardware(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOWorkLoop *getWorkLoop() const override;

    virtual IOReturn performAudioEngineStart() override;
    virtual IOReturn performAudioEngineStop() override;
    virtual UInt32 getCurrentSampleFrame() override;

    // patch-30: the family's pause/resume is asymmetric — pauseAudioEngine
    // calls performAudioEngineStop, but resumeAudioEngine only flips state
    // to Resumed and never calls performAudioEngineStart (verified against
    // IOAudioFamily-740.1 source). Nothing restarts our capture after the
    // PM resume, so a session held across sleep stays silent until reboot
    // (HANDOFF §5e). The override latches a restart request with the owner;
    // jackPoll executes it via completeDeferredResume once the DSP rebuild
    // has verifiably succeeded — one synchronous shot at resume time is the
    // design that failed all week (the DSP is still down when resume fires).
    virtual IOReturn resumeAudioEngine() override;
    // outcome (optional) receives a short literal for the Engine-Resume
    // property: "restarted", "already running", "no clients", or "idle".
    IOReturn completeDeferredResume(const char **outcome);
    // True when a client still holds a session but the engine is not
    // Running — the state an exhausted executor leaves behind. The owner's
    // wake path re-arms the latch on it: the family only pauses Running
    // engines at sleep and only resumes Paused ones at wake, so once parked
    // in Resumed no resumeAudioEngine call will ever come again on its own.
    bool needsDeferredResume() {
        return numActiveUserClients > 0 && getState() != kIOAudioEngineRunning;
    }

    virtual IOReturn convertInputSamples(const void *sampleBuf, void *destBuf,
                                         UInt32 firstSampleFrame, UInt32 numSampleFrames,
                                         const IOAudioStreamFormat *streamFormat,
                                         IOAudioStream *audioStream) override;

private:
    LatSOFAudioDevice *owner;           // not retained
    IOTimerEventSource *wrapTimer;      // 100 ms wrap detector while running
    UInt32 lastWrapFrame;               // last frame seen by the wrap detector
    bool engineRunning;

    // Input conditioning ported from the HAL plugin (which it replaces):
    // software gain (level control) and a one-pole DC-blocking high-pass —
    // raw PDM data carries a DC offset, and filtering before gain keeps the
    // offset from eating headroom. State is per-channel.
    SInt32 gainMilliDecibels;           // set by the level control
    float dcX1[2], dcY1[2];
    UInt32 dcNextFrame;                 // multi-client guard; kDCSeedPending until first IO

    // patch-29 clock accuracy. DPIB is not a continuous counter: it advances
    // in a staircase locked to the DSP's 1 ms pipeline period, so a read at a
    // random instant under-reports the true write head. Back-dating from it
    // places every timestamp late by a random fraction of a millisecond —
    // measured at 17.84 frames RMS against AppleHDA's 0.37 on the same
    // harness. Catching the staircase edge makes the (position, time) pair
    // exact. See the block comment above readPositionAtEdge().
    UInt32 edgeMisses;                  // DPIB never moved before the deadline
    UInt32 notReadyPolls;               // wrap poll skipped: position invalid
    UInt32 preemptRejects;              // sample discarded: read was preempted
    AbsoluteTime lastStampTime;         // host time of the last stamp we issued
    bool   edgeLockEnabled;             // boot-arg kill switch: latsof_edgelock=0

    bool readPosition(UInt32 *posOut);
    bool readPositionAtEdge(UInt32 *posOut, AbsoluteTime *tOut);
    void stampAt(bool incrementLoop, AbsoluteTime at, UInt32 framesAgo);
    void stampBackdated(bool incrementLoop, UInt32 framesAgo);
    static void wrapTimerFired(OSObject *target, IOTimerEventSource *sender);
    static IOReturn gainChangeHandler(OSObject *target, IOAudioControl *control,
                                      SInt32 oldValue, SInt32 newValue);

    bool createControls();
};

#endif
