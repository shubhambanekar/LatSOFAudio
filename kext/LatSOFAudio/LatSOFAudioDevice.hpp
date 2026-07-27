//
// LatSOFAudioDevice.hpp — part of LatSOFAudio, the internal-microphone driver for the
// Dell Latitude 3410 hackintosh: https://github.com/shubhambanekar/LatSOFAudio
//
// Forked from CmlSOFAudio by DexterSLamb (HP Chromebook C1030):
//   https://github.com/DexterSLamb/CmlSOFAudio
// Copyright (c) 2026 DexterSLamb
// Copyright (c) 2026 Shubham Banekar
// SPDX-License-Identifier: BSD-3-Clause — see LICENSE and NOTICE.
//

#ifndef LatSOFAudioDevice_hpp
#define LatSOFAudioDevice_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pwr_mgt/RootDomain.h>

// UserClient method selectors
enum {
    kLatSOF_StartPlayback  = 0,
    kLatSOF_StopPlayback   = 1,
    kLatSOF_GetPosition    = 2,
    kLatSOF_UpdateSPIB     = 3,
    kLatSOF_StartCapture   = 4,
    kLatSOF_StopCapture    = 5,
    kLatSOF_GetCapPosition = 6,
    kLatSOF_MethodCount    = 7
};

// Shared buffer memory types for clientMemoryForType
enum {
    kLatSOF_MemPlayback = 0,
    kLatSOF_MemCapture  = 1,
    // A single 4 KiB page of coordination flags shared with the plugin.
    // Byte 0: clamshell-muted. When non-zero, the plugin bzero's its own
    // just-written output region in DoIOOperation(WriteMix) so the audio
    // stream is digitally silenced at the source — without touching HDA
    // DMA or the DSP (avoids the PCM_PARAMS/FAILED-core-power breakage
    // that a hardware-level pause would cause).
    kLatSOF_MemFlags    = 2
};

// Byte offsets within the kLatSOF_MemFlags page.
#define kLatSOF_FlagOff_ClamshellMuted  0

#define kLatSOF_BufferFrames    16384
#define kLatSOF_BytesPerFrame   4       // S16 stereo (playback)
#define kLatSOF_BufferSize      (kLatSOF_BufferFrames * kLatSOF_BytesPerFrame)
// DMIC capture: 2ch S32 @ 48 kHz — this driver's DMA format (SDxFMT 0x0041,
// 32-bit container). Linux hw_params for the same PCM negotiates S16/2ch;
// the earlier "4ch" claim here was from the reference driver's board.
#define kLatSOF_CapChannels     2
#define kLatSOF_CapBytesPerFrame (kLatSOF_CapChannels * 4)  // 8 (2ch × S32)
#define kLatSOF_CapBufferSize   (kLatSOF_BufferFrames * kLatSOF_CapBytesPerFrame)

class LatSOFAudioUserClient;

class LatSOFAudioDevice : public IOService {
    OSDeclareDefaultStructors(LatSOFAudioDevice)
    friend class LatSOFAudioUserClient;

public:
    bool init(OSDictionary *dict = nullptr) override;
    void free() override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) override;

    // Playback (called by UserClient — serialized via commandGate)
    IOReturn startPlayback();
    IOReturn stopPlayback();
    UInt32   getSamplePosition();
    IOReturn updateSPIB(UInt32 byteOffset);
    IOBufferMemoryDescriptor *getSharedBuffer() { return sharedDmaBuf; }

    // Capture (called by UserClient — serialized via commandGate)
    IOReturn startCapture();
    IOReturn stopCapture();
    UInt32   getCapturePosition();
    IOBufferMemoryDescriptor *getCaptureBuffer() { return capDmaBuf; }

private:
    IOPCIDevice *pciDevice;
    IOMemoryMap *hdaBarMap;
    IOMemoryMap *dspBarMap;

    // Saved from start() for UserClient use
    volatile UInt8 *hdaBase;
    volatile UInt8 *dspBase;
    UInt32 ppCap, spibCap, mlCap;
    int sIdx;                  // playback stream index
    UInt32 sTag, sd;           // playback stream tag & SD offset
    int capIdx;                // capture stream index
    UInt32 capTag, capSd;      // capture stream tag & SD offset
    UInt32 outboxOff;
    bool hwReady;
    bool isPlaying;
    bool isCapturing;
    UInt32 activePlaybackHost;  // comp_id of active playback pipeline HOST
    bool lastJackState;         // true = headphone inserted
    IOTimerEventSource *jackTimer;
    void jackPoll(IOTimerEventSource *sender);

    // Serialization: all hw-touching paths (setPowerState, UserClient methods,
    // WillSleep message, jackPoll) run on the same workloop via commandGate.
    // Eliminates races between PM callbacks and user path — the root cause
    // of clamshell-sleep deadlocks.
    IOCommandGate     *commandGate;
    IOPMrootDomain    *rootDomain;
    // Notifier from registerInterest(gIOGeneralInterest, ...). THIS is how
    // we receive system-level messages like kIOMessageSystemWillSleep and
    // kIOPMMessageClamshellStateChange. registerInterestedDriver (PM-state
    // interest) does NOT receive them.
    IONotifier        *powerNotifier;

    static IOReturn    sPowerInterestHandler(void *target, void *refCon,
                                             UInt32 messageType, IOService *provider,
                                             void *messageArg, vm_size_t argSize);

    // Gated implementations. Callers must already hold workloop (either via
    // commandGate->runAction, or by running inside a workloop event source
    // callback such as jackPoll).
    IOReturn setPowerStateGated(unsigned long powerStateOrdinal);
    IOReturn startPlaybackGated();
    IOReturn stopPlaybackGated();
    IOReturn startCaptureGated();
    IOReturn stopCaptureGated();
    void     handleWillSleepGated();
    // Clamshell state change — fires on short clamshell close where the
    // system never reaches kIOMessageSystemWillSleep. Pauses HDA DMA so the
    // ring buffer can't loop audibly, and resumes it on open.
    void     handleClamshellChangeGated(bool closed);

    // Fast mute: codec mute (RT5682) + DMA buffer bzero. Called on the
    // kIOMessageSystemWillSleep early notification and at shutdownDSP entry,
    // so the PM sleep window cannot produce "last-frame looping" audio.
    void     fastMute();

    // IOCommandGate::Action static wrappers.
    static IOReturn s_setPowerState(OSObject *o, void *a0, void *, void *, void *);
    static IOReturn s_startPlayback(OSObject *o, void *, void *, void *, void *);
    static IOReturn s_stopPlayback (OSObject *o, void *, void *, void *, void *);
    static IOReturn s_startCapture (OSObject *o, void *, void *, void *, void *);
    static IOReturn s_stopCapture  (OSObject *o, void *, void *, void *, void *);
    static IOReturn s_handleWillSleep(OSObject *o, void *, void *, void *, void *);
    static IOReturn s_handleClamshellChange(OSObject *o, void *closed, void *, void *, void *);

    // Hardware init (called from start() and setPowerState on wake)
    bool initDSP();
    // Full DSP/HDA teardown. Polls DSP_ADSPCS (50 ms) and HDA GCTL
    // reset (100 ms) — safe when caller knows hardware is live. Used by
    // startPlayback/startCapture error-recovery paths.
    void shutdownDSPGated();
    // Sleep-path teardown. NO MMIO polls, NO HDA reset, NO IPC. Only codec
    // mute + state reset. Avoids the kernel deadlock observed on long
    // clamshell close: macOS PM can start gating PCI power before
    // setPowerState(0) returns, turning any rd32(hdaBase,...) into an
    // infinite hang on the PCI bus with no panic log. Wake's initDSP()
    // rebuilds everything anyway, so the sleep-side teardown is pointless
    // work — delegate it to the PCI D3 auto-power transition.
    void shutdownForSleepGated();

    // I2C + RT5682 codec
    IOPCIDevice *i2cPciDevice;
    IOMemoryMap *i2cBarMap;
    volatile UInt8 *i2cBase;
    bool initI2C();
    bool i2cWrite16(UInt16 reg, UInt16 val);
    UInt16 i2cRead16(UInt16 reg);
    bool initRT5682();

    // Playback shared DMA buffer
    IOBufferMemoryDescriptor *sharedDmaBuf;
    IOBufferMemoryDescriptor *sharedBdlBuf;

    // Capture shared DMA buffer
    IOBufferMemoryDescriptor *capDmaBuf;
    IOBufferMemoryDescriptor *capBdlBuf;

    // Coordination flag page shared with the plugin (see kLatSOF_MemFlags).
    // The plugin maps this read-only-ish page via IOConnectMapMemory64 and
    // checks flagsBuf[kLatSOF_FlagOff_ClamshellMuted] on every IO cycle.
    IOBufferMemoryDescriptor *flagsBuf;
public:
    IOBufferMemoryDescriptor *getFlagsBuffer() { return flagsBuf; }
};

class LatSOFAudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(LatSOFAudioUserClient)
public:
    bool initWithTask(task_t owningTask, void *securityToken, UInt32 type) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                 IOMemoryDescriptor **memory) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                           IOExternalMethodDispatch *dispatch, OSObject *target,
                           void *reference) override;
private:
    LatSOFAudioDevice *device;
    task_t clientTask;
    static IOReturn sStart(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a);
    static IOReturn sStop(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a);
    static IOReturn sGetPos(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a);
    static IOReturn sUpdateSPIB(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a);
    static IOReturn sStartCap(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a);
    static IOReturn sStopCap(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a);
    static IOReturn sGetCapPos(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a);
    static const IOExternalMethodDispatch sMethods[kLatSOF_MethodCount];
};

#endif
