//
// LatSOFKernelAudio.cpp — see LatSOFKernelAudio.hpp for why this exists.
//
// Design constraints inherited from three days of hard lessons:
//   - Everything runs on the OWNER's workloop (getWorkLoop overrides), so
//     engine callbacks serialize with jackPoll, PM, and the gated capture
//     paths. No new concurrency domain.
//   - The engine's sample buffer IS the kext's capture ring (capDmaBuf).
//     The DSP writes where CoreAudio reads; nothing is copied.
//   - performAudioEngineStart/Stop call the same gated capture functions
//     as the old HAL plugin's UserClient did, so the demand latch, the
//     wake re-arm, and patch-27's recovery apply to this engine unchanged.
//   - Timestamps: a 100 ms timer detects ring wrap (ring period ~341 ms)
//     and calls takeTimeStamp — the third home of patch-13's clock.
//
// Copyright (c) 2026 Shubham Banekar — BSD-3-Clause.
//

#include "LatSOFKernelAudio.hpp"
#include "LatSOFAudioDevice.hpp"
#include <IOKit/IOLib.h>

#define kEngineSampleRate   48000
#define kEngineChannels     2
#define kEngineBitDepth     32
#define kEngineFrames       kLatSOF_BufferFrames        // 16384, shared ring
#define kWrapPollMS         100
#define kDCSeedPending      0xFFFFFFFFu   // dcNextFrame: seed from first IO request

// ==================== device ====================

OSDefineMetaClassAndStructors(LatSOFKernelAudioDevice, IOAudioDevice)

IOWorkLoop *LatSOFKernelAudioDevice::getWorkLoop() const {
    return owner ? owner->getWorkLoop() : IOAudioDevice::getWorkLoop();
}

bool LatSOFKernelAudioDevice::initHardware(IOService *provider) {
    if (!IOAudioDevice::initHardware(provider)) return false;
    if (!owner) return false;

    setDeviceName("LatSOF Internal Microphone");
    setDeviceShortName("LatSOF Mic");
    setManufacturerName("Dell Latitude 3410");
    setDeviceModelName("LatSOF Internal Microphone");
    // Built-in transport, and see createControls(): CoreSpeech routes a
    // 'bltn' device only if its input-scope data source reads 'imic'. The
    // earlier belief that Siri filtered on device class (kernel vs
    // userspace) was wrong — disassembly of AVFAudio/CoreSpeech shows the
    // test is purely these HAL properties. USB devices skip the check,
    // which is the only reason the dongle ever worked.
    setDeviceTransportType(kIOAudioDeviceTransportTypeBuiltIn);

    engine = new LatSOFKernelAudioEngine;
    if (!engine) return false;
    if (!engine->initWithOwner(owner)) { engine->release(); engine = nullptr; return false; }
    if (activateAudioEngine(engine) != kIOReturnSuccess) {
        engine->release(); engine = nullptr;
        return false;
    }
    engine->release();   // activateAudioEngine retains
    return true;
}

// ==================== engine ====================

OSDefineMetaClassAndStructors(LatSOFKernelAudioEngine, IOAudioEngine)

bool LatSOFKernelAudioEngine::initWithOwner(LatSOFAudioDevice *o) {
    if (!IOAudioEngine::init(nullptr)) return false;
    owner = o;
    wrapTimer = nullptr;
    lastWrapFrame = 0;
    engineRunning = false;
    gainMilliDecibels = 30000;   // +30 dB ≈ the plugin's 32x software gain
    dcX1[0] = dcX1[1] = dcY1[0] = dcY1[1] = 0.0f;
    dcNextFrame = kDCSeedPending;
    return true;
}

IOWorkLoop *LatSOFKernelAudioEngine::getWorkLoop() const {
    return owner ? owner->getWorkLoop() : IOAudioEngine::getWorkLoop();
}

bool LatSOFKernelAudioEngine::initHardware(IOService *provider) {
    if (!IOAudioEngine::initHardware(provider)) return false;
    if (!owner) return false;

    IOBufferMemoryDescriptor *ring = owner->getCaptureBuffer();
    if (!ring) return false;    // allocated by initDSP before we are created

    setDescription("LatSOF DMIC capture");
    const IOAudioSampleRate engineRate = { kEngineSampleRate, 0 };
    setSampleRate(&engineRate);
    setNumSampleFramesPerBuffer(kEngineFrames);
    // The DSP position (DPIB) trails the true write head slightly; the
    // conservative offset below is one poll period of frames so clients
    // never read ahead of valid data. Refine after first hardware listen.
    setSampleOffset(4800);      // 100 ms at 48 kHz

    IOAudioStream *stream = new IOAudioStream;
    if (!stream) return false;
    if (!stream->initWithAudioEngine(this, kIOAudioStreamDirectionInput, 1)) {
        stream->release(); return false;
    }

    IOAudioStreamFormat fmt = {};
    fmt.fNumChannels        = kEngineChannels;
    fmt.fSampleFormat       = kIOAudioStreamSampleFormatLinearPCM;
    fmt.fNumericRepresentation = kIOAudioStreamNumericRepresentationSignedInt;
    fmt.fBitDepth           = kEngineBitDepth;
    fmt.fBitWidth           = kEngineBitDepth;
    fmt.fAlignment          = kIOAudioStreamAlignmentHighByte;
    fmt.fByteOrder          = kIOAudioStreamByteOrderLittleEndian;
    fmt.fIsMixable          = true;
    fmt.fDriverTag          = 0;

    const IOAudioSampleRate rate = { kEngineSampleRate, 0 };
    stream->addAvailableFormat(&fmt, &rate, &rate);
    stream->setFormat(&fmt);
    stream->setTerminalType(INPUT_MICROPHONE);   // 0x0201; default 0x0 reads as "not a mic"
    stream->setSampleBuffer(ring->getBytesNoCopy(), kLatSOF_CapBufferSize);
    IOReturn ar = addAudioStream(stream);
    stream->release();          // addAudioStream retains on success
    if (ar != kIOReturnSuccess) return false;

    // Round-2 correction: the DPIB lag is already absorbed by the
    // back-dated timeline plus the sample offset — reporting it AGAIN as
    // latency told AEC/AV-sync that audio is 100 ms older than the
    // timeline says. Declare only the true pipeline delay (provisional
    // 5 ms; refine after hardware measurement).
    setInputSampleLatency(240);

    if (!createControls()) return false;

    IOWorkLoop *wl = getWorkLoop();
    if (!wl) return false;      // check BEFORE allocating — no timer leak
    wrapTimer = IOTimerEventSource::timerEventSource(this,
        (IOTimerEventSource::Action)&LatSOFKernelAudioEngine::wrapTimerFired);
    if (!wrapTimer) return false;
    if (wl->addEventSource(wrapTimer) != kIOReturnSuccess) {
        wrapTimer->release(); wrapTimer = nullptr;
        return false;
    }

    return true;
}

void LatSOFKernelAudioEngine::stop(IOService *provider) {
    if (wrapTimer) {
        wrapTimer->cancelTimeout();
        if (getWorkLoop()) getWorkLoop()->removeEventSource(wrapTimer);
        wrapTimer->release();
        wrapTimer = nullptr;
    }
    IOAudioEngine::stop(provider);
}

bool LatSOFKernelAudioEngine::createControls() {
    // Input gain: 0..+40 dB in millidecibels, defaulting to the +30 dB the
    // HAL plugin shipped with (its fixed 32x). Linear control values are
    // the same range so the slider position is meaningful.
    IOAudioLevelControl *gain = IOAudioLevelControl::createVolumeControl(
        gainMilliDecibels / 1000,           // initial (control units = dB)
        0, 40,                              // min, max (dB)
        0, (40 << 16),                      // min/max dB as IOFixed 16.16
        kIOAudioControlChannelIDAll,
        nullptr,                            // channel name
        0, kIOAudioControlUsageInput);
    if (!gain) return false;
    gain->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)&LatSOFKernelAudioEngine::gainChangeHandler, this);
    addDefaultAudioControl(gain);
    gain->release();

    // Input data source 'imic'. This is Siri's actual gate: CoreSpeech
    // accepts a built-in-transport route only when the input-scope HAL
    // DataSource ('ssrc') reads 'imic'/'emic', and AVFAudio's built-in-mic
    // finder demands 'imic' exactly. The HAL derives that property from an
    // input selector control; without one the query fails ('who?') and the
    // Siri UI declares "Connect a microphone" over a working record path.
    IOAudioSelectorControl *src = IOAudioSelectorControl::createInputSelector(
        kIOAudioSelectorControlSelectionValueInternalMicrophone,
        kIOAudioControlChannelIDAll);
    if (!src) return false;
    src->addAvailableSelection(kIOAudioSelectorControlSelectionValueInternalMicrophone,
                               "Internal Microphone");
    addDefaultAudioControl(src);
    src->release();
    return true;
}

IOReturn LatSOFKernelAudioEngine::gainChangeHandler(OSObject *target, IOAudioControl *control,
                                                    SInt32 oldValue, SInt32 newValue) {
    auto *self = OSDynamicCast(LatSOFKernelAudioEngine, target);
    if (!self) return kIOReturnBadArgument;
    self->gainMilliDecibels = newValue * 1000;    // control units are dB
    return kIOReturnSuccess;
}

// Back-date an event that happened `framesAgo` frames before "now" and
// stamp it. The wrap poller sees a wrap up to 100 ms after the fact, and a
// start against already-running DMA finds DPIB mid-ring; in both cases the
// true event time is now - frames/48kHz. Review finding: an un-backdated
// stamp puts 0-100 ms of uniform jitter on fLastLoopTime, which the header
// itself calls the basis of the entire audio timer mechanism.
void LatSOFKernelAudioEngine::stampBackdated(bool incrementLoop, UInt32 framesAgo) {
    AbsoluteTime now, ts;
    UInt64 nowNs;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now, &nowNs);
    UInt64 agoNs = ((UInt64)framesAgo * 1000000000ULL) / kEngineSampleRate;
    nanoseconds_to_absolutetime(nowNs > agoNs ? nowNs - agoNs : 0, &ts);
    takeTimeStamp(incrementLoop, &ts);
}

IOReturn LatSOFKernelAudioEngine::performAudioEngineStart() {
    if (!owner) return kIOReturnNotReady;
    // Gated entry; routed through the owner's commandGate (the family does
    // NOT reliably deliver this on the owner's workloop — its device PM
    // runs a private loop — so runAction is load-bearing, not paranoia).
    IOReturn r = owner->engineStartCapture();
    if (r != kIOReturnSuccess) return r;

    dcX1[0] = dcX1[1] = dcY1[0] = dcY1[1] = 0.0f;
    dcNextFrame = kDCSeedPending;
    // If capture was already running (isCapturing early-success — e.g. a
    // client retry after the wake window), DPIB is mid-ring: declare t0
    // back-dated by the current position so the HAL's frame math starts
    // aligned instead of up to one ring (~341 ms) skewed.
    UInt32 pos = getCurrentSampleFrame();
    lastWrapFrame = pos;
    engineRunning = true;
    stampBackdated(false, pos);
    if (wrapTimer) wrapTimer->setTimeoutMS(kWrapPollMS);
    return kIOReturnSuccess;
}

IOReturn LatSOFKernelAudioEngine::performAudioEngineStop() {
    engineRunning = false;
    if (wrapTimer) wrapTimer->cancelTimeout();
    if (owner) owner->engineStopCapture();
    return kIOReturnSuccess;
}

UInt32 LatSOFKernelAudioEngine::getCurrentSampleFrame() {
    if (!owner) return 0;
    UInt32 f = owner->getCapturePosition();       // DPIB/8, wraps at ring size
    return (f < kEngineFrames) ? f : 0;
}

void LatSOFKernelAudioEngine::wrapTimerFired(OSObject *target, IOTimerEventSource *sender) {
    auto *self = OSDynamicCast(LatSOFKernelAudioEngine, target);
    if (!self || !self->engineRunning) return;
    UInt32 now = self->getCurrentSampleFrame();
    // Ring period is ~341 ms; polling at 100 ms means a wrap is seen as
    // position moving backwards exactly once per cycle. The wrap happened
    // `now` frames ago — back-date the stamp; stamping "poll time" would
    // inject up to 100 ms of jitter per loop into the HAL's clock.
    if (now < self->lastWrapFrame)
        self->stampBackdated(true, now);
    self->lastWrapFrame = now;
    if (sender) sender->setTimeoutMS(kWrapPollMS);
}

IOReturn LatSOFKernelAudioEngine::convertInputSamples(const void *sampleBuf, void *destBuf,
                                                      UInt32 firstSampleFrame, UInt32 numSampleFrames,
                                                      const IOAudioStreamFormat *streamFormat,
                                                      IOAudioStream *audioStream) {
    const SInt32 *src = (const SInt32 *)sampleBuf + firstSampleFrame * kEngineChannels;
    float *dst = (float *)destBuf;

    // gain: dB -> linear. Cheap pow-free approx is not worth it in v1;
    // compute once per conversion call from the control value.
    float db = (float)gainMilliDecibels / 1000.0f;
    float gainLin = 1.0f;
    { // 10^(db/20) via exp2: 10^x = 2^(x*log2(10))
        float x = (db / 20.0f) * 3.3219281f;
        // exp2f approximation adequate for gain staging
        int   xi = (int)x;
        float xf = x - (float)xi;
        float p = 1.0f + xf * (0.6931472f + xf * (0.2401397f + xf * 0.0558282f));
        gainLin = p * (float)(1 << (xi > 0 ? xi : 0));
        if (xi < 0) gainLin = p / (float)(1 << (-xi));
    }

    // Multi-client guard (review minor): with two capture clients the
    // family calls this once per client over overlapping frames; letting
    // both advance the filter state double-steps it and distorts. Persist
    // state only for the client advancing contiguously from where the
    // filter last stood; any other request runs on a scratch copy.
    // Round-2 fix: the FIRST call after engine start seeds the expected
    // frame — the HAL starts reading at an arbitrary ring position, so
    // requiring contiguity-from-zero meant the state never persisted for
    // a normal single client (filter re-converging from zero on every IO
    // block: a repeated gain-amplified DC step for up to one ring period,
    // indistinguishable from crackle in a listen test).
    if (dcNextFrame == kDCSeedPending)
        dcNextFrame = firstSampleFrame;
    bool contiguous = (firstSampleFrame == dcNextFrame);
    float x1[2] = { dcX1[0], dcX1[1] }, y1[2] = { dcY1[0], dcY1[1] };

    const float R = 0.98822f;   // ~90 Hz corner at 48 kHz, same as plugin
    for (UInt32 i = 0; i < numSampleFrames; i++) {
        for (UInt32 ch = 0; ch < kEngineChannels; ch++) {
            float x = (float)src[i * kEngineChannels + ch] / 2147483648.0f;
            float y = x - x1[ch] + R * y1[ch];
            x1[ch] = x;
            // flush denormals: a geometric decay into denormal range costs
            // real CPU in a realtime path
            if (y < 1e-15f && y > -1e-15f) y = 0.0f;
            y1[ch] = y;
            float v = y * gainLin;
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            dst[i * kEngineChannels + ch] = v;
        }
    }
    if (contiguous) {
        dcX1[0] = x1[0]; dcX1[1] = x1[1];
        dcY1[0] = y1[0]; dcY1[1] = y1[1];
        dcNextFrame = (firstSampleFrame + numSampleFrames) % kEngineFrames;
    }
    return kIOReturnSuccess;
}
