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

// patch-29 clock accuracy (see readPositionAtEdge). All bounds are in TIME,
// never in loop iterations: IODelay plus an uncached MMIO read costs more
// than the delay alone, so counting iterations understates the real bound.
#define kEdgeSpinUS         5             // pause between DPIB polls
#define kEdgeDeadlineNS     2000000ULL    // 2 ms: ~2x the worst observed dwell
#define kEdgeReadWindowNS   20000ULL      // >20 us around a read = preempted
#define kRingPeriodNS       (((UInt64)kEngineFrames * 1000000000ULL) / kEngineSampleRate)

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

// See the header comment: the owner must stop reading `engine` before the
// family frees it below in deactivateAllAudioEngines. The notification runs
// under the owner's commandGate, so it strictly precedes any later gated
// reader — which then sees nullptr instead of a dangling pointer.
void LatSOFKernelAudioDevice::stop(IOService *provider) {
    if (owner) owner->kernelAudioTearingDown(this);
    IOAudioDevice::stop(provider);
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
    edgeMisses = notReadyPolls = preemptRejects = 0;
    AbsoluteTime zero = 0;
    lastStampTime = zero;
    // Kill switch, kept because patch-28 shipped a design the hardware
    // rejected: latsof_edgelock=0 restores the previous timing behaviour
    // without a rebuild, while keeping the validity guard.
    UInt32 el = 1;
    if (PE_parse_boot_argn("latsof_edgelock", &el, sizeof(el)))
        edgeLockEnabled = (el != 0);
    else
        edgeLockEnabled = true;
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

// Stamp an event that happened `framesAgo` frames before an EXPLICIT host
// time, rather than before "now". Every caller that spins before stamping
// must use this: back-dating from a clock read taken after the spin, using a
// position read taken before it, lands the anchor late by the whole spin.
void LatSOFKernelAudioEngine::stampAt(bool incrementLoop, AbsoluteTime at,
                                      UInt32 framesAgo) {
    AbsoluteTime ts;
    UInt64 atNs = 0;
    absolutetime_to_nanoseconds(at, &atNs);
    UInt64 agoNs = ((UInt64)framesAgo * 1000000000ULL) / kEngineSampleRate;
    nanoseconds_to_absolutetime(atNs > agoNs ? atNs - agoNs : 0, &ts);
    lastStampTime = at;
    takeTimeStamp(incrementLoop, &ts);
}

// Position read that distinguishes "unknown" from "frame 0".
bool LatSOFKernelAudioEngine::readPosition(UInt32 *posOut) {
    if (!owner || !owner->capturePositionValid()) return false;
    *posOut = getCurrentSampleFrame();
    return true;
}

// Read the capture position AND the host time at which it changed.
//
// WHY: DPIB is a completed-transfer counter that only advances when the DSP
// pipeline ticks (this driver declares period = 1000 us in its PIPELINE
// COMP_NEW IPC), so between ticks it is frozen and a read UNDER-reports the
// true write head. stampBackdated()'s model — "the wrap happened
// framesAgo/48000 seconds ago" — is exact only at the instant DPIB updates.
// Reading at a random instant therefore places the anchor late by a random
// fraction of a tick, and IOAudioEngine.h is explicit that fLastLoopTime
// accuracy "is the basis for the entire timer and synchronization mechanism
// used by the audio system".
//
// So: spin until the register moves, and pair that new value with a clock
// read taken immediately around it.
//
// Two properties this must have, both learned from review rather than
// discovered afterwards:
//   1. The deadline is measured in TIME, not iterations. IODelay(5) plus an
//      uncached MMIO read costs more than 5 us, so counting iterations
//      understates the real bound by 2-3x.
//   2. IODelay spins with preemption ENABLED. If this thread is descheduled
//      between the position read and the clock read, the pair is a lie and
//      stamping it would be worse than today's bounded error. Bracket the
//      read and DISCARD any sample whose read window is implausibly long.
//      Rejecting a poisoned sample is what makes the scheme defensible.
//
// Returns false if the position never moved before the deadline, or validity
// was lost mid-spin; the caller must then fall back using a time captured
// BEFORE the spin.
bool LatSOFKernelAudioEngine::readPositionAtEdge(UInt32 *posOut, AbsoluteTime *tOut) {
    UInt32 p0;
    if (!readPosition(&p0)) return false;

    AbsoluteTime start;
    clock_get_uptime(&start);
    UInt64 startNs = 0;
    absolutetime_to_nanoseconds(start, &startNs);

    for (;;) {
        AbsoluteTime tA, tB;
        UInt32 p;

        clock_get_uptime(&tA);
        if (!readPosition(&p)) return false;
        clock_get_uptime(&tB);

        UInt64 aNs = 0, bNs = 0;
        absolutetime_to_nanoseconds(tA, &aNs);
        absolutetime_to_nanoseconds(tB, &bNs);

        if (bNs - aNs <= kEdgeReadWindowNS) {       // sample is trustworthy
            if (p != p0) { *posOut = p; *tOut = tB; return true; }
        } else {
            preemptRejects++;                        // descheduled mid-read
        }

        if (bNs - startNs >= kEdgeDeadlineNS) break; // time-bounded, not count
        IODelay(kEdgeSpinUS);
    }

    edgeMisses++;
    return false;
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
    AbsoluteTime t0;
    clock_get_uptime(&t0);            // before any spin, as in wrapTimerFired
    engineRunning = true;

    UInt32 pos = 0;
    AbsoluteTime edgeTime = 0;
    bool edged = edgeLockEnabled && readPositionAtEdge(&pos, &edgeTime);
    if (edged) {
        lastWrapFrame = pos;
        stampAt(false, edgeTime, pos);
    } else {
        pos = getCurrentSampleFrame();
        lastWrapFrame = pos;
        stampAt(false, t0, pos);
    }
    if (wrapTimer) wrapTimer->setTimeoutMS(kWrapPollMS);
    // One line per session start: session-start timeline marker for tests.
    // "edge=1" means the anchor came from a caught DPIB tick (patch-29).
    IOLog("LatSOF: engine start pos=%u edge=%d clients=%u\n",
          pos, edged ? 1 : 0, (unsigned)numActiveUserClients);
    return kIOReturnSuccess;
}

IOReturn LatSOFKernelAudioEngine::performAudioEngineStop() {
    engineRunning = false;
    if (wrapTimer) wrapTimer->cancelTimeout();
    if (owner) owner->engineStopCapture();
    // Publish the session's clock anomalies so they survive into ioreg after
    // the call ends. All three should be 0 or near it; a rising edgeMisses
    // means the DSP is stalling or the deadline is too tight, and a non-zero
    // notReadyPolls proves a recovery/wake window was crossed mid-session.
    setProperty("Clock-Edge-Misses",   edgeMisses,     32);
    setProperty("Clock-NotReady-Polls", notReadyPolls, 32);
    setProperty("Clock-Preempt-Rejects", preemptRejects, 32);
    // Counters are cumulative per boot; the log line gives each session's
    // snapshot without needing ioreg between test steps.
    IOLog("LatSOF: engine stop edgeMisses=%u notReadyPolls=%u preemptRejects=%u\n",
          edgeMisses, notReadyPolls, preemptRejects);
    return kIOReturnSuccess;
}

// patch-30: see the header comment. Latch only — the actual restart must
// wait for the owner's retry engine, because resume fires on the family PM
// path seconds before the DSP rebuild completes, and a synchronous start
// here would burn its one shot on kIOReturnNotReady.
IOReturn LatSOFKernelAudioEngine::resumeAudioEngine() {
    IOReturn r = IOAudioEngine::resumeAudioEngine();
    // state/clients at resume time are the whole story of test §6.1 vs §6.2:
    // clients>0 here is a session that MUST come back; clients==0 must not.
    IOLog("LatSOF: resumeAudioEngine state=%d clients=%u — latching deferred restart\n",
          (int)getState(), (unsigned)numActiveUserClients);
    if (owner) owner->engineRequestResume();
    return r;
}

// Executed by the owner's jackPoll once hwReady is back. Goes through the
// FAMILY's startAudioEngine rather than any side channel so DMA, engine
// state, the patch-29 edge-anchored timestamp and the Started notification
// to the HAL all come back through the one audited path. On a Resumed
// engine startAudioEngine falls through to performAudioEngineStart
// (IOAudioFamily-740.1:1806); on a Paused one it merely resumes, so a
// nested pauseCount is drained first and the executor's bounded retries
// cover the remainder.
IOReturn LatSOFKernelAudioEngine::completeDeferredResume(const char **outcome) {
    if (outcome) *outcome = "idle";
    IOAudioEngineState s = getState();
    if (s == kIOAudioEngineRunning) {
        if (outcome) *outcome = "already running";
        IOLog("LatSOF: %s\n", "deferred resume: already running — no-op");
        return kIOReturnSuccess;                               // client won the race
    }
    if (numActiveUserClients == 0) {
        if (outcome) *outcome = "no clients";
        IOLog("LatSOF: %s\n", "deferred resume: no clients — nothing to restart");
        return kIOReturnSuccess;                               // nobody held a session
    }
    IOLog("LatSOF: deferred resume: starting engine (state=%d clients=%u)\n",
          (int)s, (unsigned)numActiveUserClients);
    if (s == kIOAudioEnginePaused) IOAudioEngine::resumeAudioEngine();
    IOReturn r = startAudioEngine();
    if (r != kIOReturnSuccess) return r;
    // Never claim a restart we cannot see. startAudioEngine returns SUCCESS
    // on a Paused engine while only draining one pause level (740.1:1801) —
    // so with a nested pauseCount the engine ends up Resumed, not Running,
    // and clearing the latch here would strand the session silently. Report
    // busy instead: the executor's bounded retry drains the next level on
    // the following tick, and its telemetry stays honest either way.
    if (getState() != kIOAudioEngineRunning) {
        IOLog("LatSOF: %s\n",
              "deferred resume: nested pause drained, not running yet — will retry");
        return kIOReturnBusy;
    }
    if (outcome) *outcome = "restarted";
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

    // Capture the entry time BEFORE anything that can spin. Every fallback
    // path below back-dates from this, never from a clock read taken after
    // the edge spin — a stamp built from a pre-spin position and a post-spin
    // clock lands late by the whole spin, which is a far worse outlier than
    // the sub-millisecond error the spin exists to remove.
    AbsoluteTime t0;
    clock_get_uptime(&t0);

    UInt32 now;
    if (!self->readPosition(&now)) {
        // DSP is down (recovery, wake retry, or sleep teardown): DPIB reads
        // as 0, which is NOT a wrap. Stamping it would inject a phantom
        // +341 ms loop increment with zero elapsed host time.
        //
        // But do not simply go silent either. These windows are not short —
        // a rebuild takes >=1.5 s and defers while AppleHDA output is busy —
        // and a frozen fLastLoopTime stalls a client's read head outright,
        // which is worse for a live recording than a discontinuity. So
        // free-run the timeline at the nominal rate: keep issuing one loop
        // increment per ring period of REAL elapsed time, so sample time and
        // host time stay locked together across the outage. When capture
        // returns, the next genuine edge re-anchors it.
        self->notReadyPolls++;
        UInt64 lastNs = 0, nowNs = 0;
        absolutetime_to_nanoseconds(self->lastStampTime, &lastNs);
        absolutetime_to_nanoseconds(t0, &nowNs);
        if (lastNs != 0 && nowNs - lastNs >= kRingPeriodNS) {
            AbsoluteTime due;
            nanoseconds_to_absolutetime(lastNs + kRingPeriodNS, &due);
            self->stampAt(true, due, 0);
        }
        if (sender) sender->setTimeoutMS(kWrapPollMS);
        return;                      // lastWrapFrame deliberately untouched
    }

    // Ring period is ~341 ms; polling at 100 ms means a wrap is seen as
    // position moving backwards exactly once per cycle.
    if (now < self->lastWrapFrame) {
        UInt32 edgePos;
        AbsoluteTime edgeTime;
        if (self->edgeLockEnabled &&
            self->readPositionAtEdge(&edgePos, &edgeTime)) {
            // Exact pair: position and the host time it became true.
            self->stampAt(true, edgeTime, edgePos);
            now = edgePos;           // keep lastWrapFrame consistent with the
                                     // value the stamp was actually built from
        } else {
            // Fall back to t0 — the clock read from BEFORE any spin.
            self->stampAt(true, t0, now);
        }
    }
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
