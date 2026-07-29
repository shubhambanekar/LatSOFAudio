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

    void setOwner(LatSOFAudioDevice *o) { owner = o; }

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

    void stampBackdated(bool incrementLoop, UInt32 framesAgo);
    static void wrapTimerFired(OSObject *target, IOTimerEventSource *sender);
    static IOReturn gainChangeHandler(OSObject *target, IOAudioControl *control,
                                      SInt32 oldValue, SInt32 newValue);

    bool createControls();
};

#endif
