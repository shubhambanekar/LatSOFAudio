# Architecture

This document explains how LatSOFAudio captures from the DMIC array while
AppleHDA keeps full ownership of playback on the **same PCI function**.
It assumes familiarity with HDA controllers and basic IOKit.

## The hardware problem

On Comet Lake, the HDA controller and the Smart Sound DSP (cAVS) are one
PCI function (`00:1f.3`, `8086:02c8`). The internal mics are PDM DMICs
wired to the DSP, not to the codec — and on boards like the Latitude 3410
the ALC236 codec has no analog mic pin at all. macOS drives the codec via
AppleHDA and leaves the DSP in reset (`ADSPCS = 0x00000f0f`: all cores
powered down and stalled). No DSP, no mics.

Linux solves this with the SOF driver stack: boot open firmware on the
DSP, build audio pipelines over an IPC mailbox, capture via the
controller's DMA engines. This project ports the minimum viable subset of
that idea to macOS — capture only — while refusing to fight AppleHDA.

## Coexistence design

The central constraint: **AppleHDA must keep working.** Everything follows
from it.

- **No PCI device claim.** The kext matches `IOResources` and finds the
  HDA controller by walking the IORegistry. AppleHDA remains the device's
  driver; we map BAR4 (the DSP registers) read/write from the sidelines.
  Note for anyone modifying this: the driver class overrides `probe()`
  and the reference implementation cast its provider to `IOPCIDevice` —
  with an `IOResources` provider that cast fails silently and `start()`
  never runs. Both `probe()` and `start()` are patched accordingly.
- **No global controller reset.** The reference driver (written for a
  Chromebook where it owned the device) performed a GCTL reset during
  bring-up. Here that would destroy AppleHDA's state. It turns out the
  SOF firmware boots fine without it. Note that removing it from bring-up
  was not sufficient: a second GCTL reset survived in `shutdownDSPGated()`,
  the playback error-recovery path, together with blanket writes of `INTCTL`
  and `PPCTL` — reachable from any `StartPlayback` call, which on this board
  is guaranteed to fail (no I2S codec, so it selects a pipeline this fork
  deletes from the topology). Removed, and playback selectors now refuse.
  If you derive from the same reference, grep for *every* `GCTL` write, not
  just the one in the init path.
- **Stream-descriptor partitioning.** GCAP `0x9701` advertises 7 input and 9
  output DMA engines, so SD0–SD6 are input descriptors and SD7–SD15 output.
  AppleHDA captures on SD0. This driver captures on input descriptor **SD1
  with stream tag 2**, which is exclusively ours for the driver's lifetime.
  It also **borrows SD7 — AppleHDA's first output engine — for the duration
  of every firmware load**, because the DSP code loader has to run over an
  output descriptor. That borrow is the most dangerous thing this driver
  does; see "The borrowed-stream contract" below.
- **Per-stream decoupling only.** `PPCTL` is written to decouple SD1 into
  DSP mode exactly once at capture start, and it is re-coupled at stop.
  Bit 0 (AppleHDA's SD0) is never touched. An early version of this code
  applied the decouple with an uninitialised index — decoupling SD0 on
  every boot, harmlessly for playback but fatally for our capture, which
  waited forever on the HDA link for tag-2 data no codec sends. If your
  capture position sits at 0 while start/stop IPCs return success, check
  the decouple actually hit your descriptor.
- **Poll-only means poll-only, at every level.** This is the single most
  important rule in the driver, and getting it wrong costs playback rather
  than capture, which makes it very hard to diagnose. The reference driver
  owned its device and could handle interrupts; this one cannot. Concretely:
  never set `IOCE`, `FEIE` or `DEIE` in the capture stream's `SDCTL`, never set
  the stream's `SIE` bit in the controller's `INTCTL`, and clear `SDSTS`
  (write-1-to-clear) at both start *and* stop. See "The interrupt-starvation
  bug" below for what happens otherwise.
- **Poll-only IPC; the DSP interrupt stays masked.** `00:1f.3` has one
  interrupt line shared with AppleHDA. If firmware→host notifications
  (HIPCTDR) are left unacknowledged, the line stays asserted until macOS
  throttles it — which kills AppleHDA's playback interrupts and mutes the
  speakers until reboot. In steady state this driver keeps `ADSPIC` bit 0
  masked, with exactly one exception: the DSP ROM requires the doorbell
  interrupt enabled during the firmware-load handshake, so `initDSP()`
  unmasks it for that window and masks it again before exiting — the mask
  lives at the `cleanup:` label, and every path that can have unmasked it
  passes through there (the handful of exits that bypass `cleanup:` all
  occur before the unmask; do not arm the interrupt any earlier in the
  function without re-checking that). Separately, the driver drains/acks
  TDR at capture start, after trigger, and at stop. All IPC waits are
  bounded polls (≤500 ms).

## DSP bring-up and pipeline

Firmware is `sof-cml.ri` (IPC3 generation), loaded via the code-loader DMA
path. Boot is confirmed by `FW_ENTERED`/`FW_READY`; the uplink mailbox
lives at `0x81000` and the downlink at `0x82000` (verified by reading the
firmware tag and by a deliberately invalid IPC returning `-EINVAL`).

The capture topology is minimal: host component → DMIC DAI, configured by
eight IPCs. The DAI configuration is derived from the board's **NHLT**
table: `num_pdm_active = 1`, both microphones on PDM0, 48 kHz, 32-bit
container, stereo (`SDxFMT 0x0041` — the reference's 4-channel `0x0043`
does not apply to this board). `no_stream_position = 1` is required for a
poll-only driver; the position source is **DPIB**, which upstream Linux
also uses on this platform. (Empirically LPIB tracks DPIB for decoupled
input on this silicon, but DPIB is the documented choice.)

There is **no gain stage** in the pipeline — the DSP ships raw DMIC
samples. Speech peaks around −35 dBFS; amplification and DC removal are
deliberately done in the audio engine's conditioning stage (below), where
they are a few lines of float arithmetic that can be retuned without
reconfiguring a running DSP.

IPC error decode, for anyone extending the pipeline:
`0xffffffea` = `-EINVAL` (params/topology mismatch),
`0xfffffff4` = `-ENOMEM` (shrink period/buffer),
`0xfffffffa` = `-EBUSY` (stream-tag collision).

## Kext ↔ userspace interface

An `IOUserClient` exposes: method 4 = start capture, 5 = stop, 6 = read
position; `clientMemoryForType(1)` maps the capture ring (S32LE,
interleaved stereo, 48 kHz) into the calling process. This was the audio
path back when a userspace HAL plugin published the device. It is no
longer — the kernel engine described next is — but it is kept, because a
command-line tool that maps the ring and dumps it is the fastest way to
prove the DMA is alive without CoreAudio in the picture at all, and
because it costs nothing to leave in place. Both entry points funnel into
the same gated `startCaptureGated`/`stopCaptureGated`, and a start against
an already-running capture returns success without touching hardware.

Nothing ever starts DMA at boot: an early boot-time self-test started a
DMA engine while AppleHDA was still initialising the controller and hung
the machine — capture is strictly on-demand from a fully booted system.

## The kernel audio engine

`LatSOFKernelAudio.cpp` is how CoreAudio learns the microphone exists.
`LatSOFKernelAudioDevice` is a thin `IOAudioDevice` — it owns no hardware and
exists only so IOAudioFamily has a device node to hang an engine on — and it
activates one `LatSOFKernelAudioEngine`, an `IOAudioEngine` over the capture
ring. `LatSOFAudioDevice::start()` creates, attaches and starts the device after
`initDSP()` has succeeded, handing it a **non-retained** back-pointer to itself
(`setOwner`) which it passes on to the engine (`initWithOwner`); the owner
outlives both by construction, which is what makes not retaining safe. Publication
failure is deliberately non-fatal — capture still works through the UserClient —
but it logs loudly, because being visible to the system's voice stack is the
entire reason the class exists. Teardown runs *first* in the owner's `stop()`,
while the workloop and gated capture paths are still alive, because terminating
the engine can call back into `engineStopCapture()` and that must find working
machinery.

This replaces `LatSOFAudioPlugin.driver`; see "The HAL plugin, historically"
below for what survived the move and what did not.

**Zero copy.** `initHardware` calls `setSampleBuffer` on
`owner->getCaptureBuffer()->getBytesNoCopy()` for `kLatSOF_CapBufferSize` bytes.
The engine's sample buffer *is* the DMA ring the DSP writes into. There is no
copy layer and no second ring: the DSP writes where CoreAudio reads. The
ring is allocated by `initDSP()` before the engine is ever constructed, and
`initHardware` refuses to proceed if it is absent.

**Format and geometry.** One input stream, 48 kHz, 2 channels, signed 32-bit
little-endian, high-byte aligned, `kLatSOF_BufferFrames` = 16384 frames of ring
(≈341 ms). Two of the numbers configured here are easy to conflate, and
conflating them is an error the ear can hear:

- `setSampleOffset(4800)` — 100 ms at 48 kHz — is a **scheduling constraint**.
  DPIB trails the true write head, and the wrap poller below only samples it
  every 100 ms; the offset is what holds CoreAudio's read point far enough
  behind the record head that everything it reads is settled data.
- `setInputSampleLatency(240)` — 5 ms, provisional — is a **report-only**
  number. Nothing schedules against it. AV-sync and echo cancellation subtract
  it to decide when the sound in a buffer actually reached the microphone.

They answer different questions and must not be given the same value. An
earlier revision reported the DPIB lag as the latency *as well* as absorbing it
in the offset and the back-dated timeline, which double-counts it: the
consumers were told the audio was a further 100 ms older than the timeline
already said it was.

**Measuring the real latency.** The 240 frames above is a placeholder for the
pipeline delay and should be replaced by a measurement. The obvious approach —
play a burst, record it, subtract the output device's reported latency — fails,
because the arrival timestamps come from *this driver's own clock model*. That
measures the suspect clock with the suspect clock, and duly returns physically
impossible negative values. A valid measurement needs a second input device you
trust (any USB microphone): capture the same burst on both in one run, and the
difference in arrival times is this device's excess latency, with our
timestamps out of the loop. Until someone runs that, treat the number as
provisional and do not build anything on it.

**Timestamping, and why it is back-dated.** `takeTimeStamp` assumes the event
being stamped is happening *now*. Neither of this engine's two stamp sites is.
`stampBackdated(incrementLoop, framesAgo)` therefore computes
`now − framesAgo/48000` seconds and stamps that instead:

- `wrapTimerFired` is a 100 ms `IOTimerEventSource` that watches
  `getCurrentSampleFrame()` (DPIB/8) for the position moving backwards, which on
  a ~341 ms ring happens exactly once per loop. It can notice the wrap up to a
  full poll period after it happened, and the position it reads *is* the number
  of frames that have elapsed since — so it back-dates by exactly that.
- `performAudioEngineStart` may find DMA already running (the gated start
  returns success immediately when `isCapturing`, e.g. a client retrying after
  the wake window). DPIB is then mid-ring, and the engine's t0 has to be
  back-dated by the current position or the HAL's frame arithmetic starts up to
  one full ring skewed.

Stamping "poll time" in either case injects 0–100 ms of uniform jitter into
`fLastLoopTime`, which IOAudioFamily's own header calls the basis of the entire
audio timer mechanism. This is the third home of the clock discipline first
written as patch 13 in `history/README.md`: the plugin did the same back-dating
in `GetZeroTimeStamp`, with a 1/8 correction filter it applied itself. Here the
back-dating is ours and any smoothing is the HAL's.

**Input conditioning.** `convertInputSamples` receives the ring and a
destination buffer in the client's float format, and does the S32 → Float32
conversion in the same pass as a per-channel one-pole **DC-blocking high-pass**
(`y[n] = x[n] − x[n−1] + R·y[n−1]`, `R = 0.98822` ≈ 90 Hz corner at 48 kHz)
followed by gain and a clamp to ±1.0. This chain is the HAL plugin's, ported
intact, including its ordering rationale: raw PDM output carries a DC offset,
and filtering *before* gain keeps that offset from eating headroom. Denormals
are flushed, because a geometric decay into denormal range costs real CPU in a
realtime path. Gain comes from an `IOAudioLevelControl` (0…+40 dB, default
+30 dB — the plugin's fixed 32×, landing speech near −5 dBFS) and is converted
from dB to linear per call with a small polynomial `exp2` rather than `powf`.

**Filter state has to survive across IO blocks, and that is fiddlier than it
looks.** The filter is only meaningful if `x[n−1]`/`y[n−1]` carry from one
`convertInputSamples` call to the next, but the family may call it more than
once per cycle and, with two capture clients, once per client over overlapping
frame ranges. Letting every call advance the shared state double-steps the
filter and distorts. So `dcNextFrame` records the frame the filter state stands
at; a request starting there persists its state, and any other request runs on a
scratch copy of it.

The subtlety is the initial value. A naive design requires `dcNextFrame == 0` to
start — but the family's first read after a start lands at an arbitrary ring
position, essentially never 0, so under that rule the state would *never*
persist. Every IO block would restart the filter from zero, and every block
would therefore begin with the filter re-converging out of a gain-amplified DC
step: a periodic thump at IO-block rate that is indistinguishable from crackle
in a listen test. The fix is the sentinel `kDCSeedPending` (`0xFFFFFFFF`),
meaning "not yet seeded": `performAudioEngineStart` sets it along with zeroing
the filter, and the first `convertInputSamples` call adopts *its own*
`firstSampleFrame` as the expected frame.

The wrap-split case then falls out for free, and it is worth checking that it
does. When a read crosses the end of the ring the family splits it into
`[N, 16384)` followed by `[0, L)`. The first sub-call's advance is
`(firstSampleFrame + numSampleFrames) % kEngineFrames`, which is 0 — exactly the
second sub-call's `firstSampleFrame`. The split stays contiguous and the filter
never notices the seam.

**Concurrency: one workloop, no new domain.** Both classes override
`getWorkLoop()` to return the owner's, so family callbacks serialize against
`jackPoll`, the PM paths and the DSP command gate. Nothing here introduces a
second concurrency domain — which is the only reason the ported conditioning
state and the shared capture flags can be plain members.

The override is necessary but not sufficient, and the engine's start/stop
entry points still go through `commandGate->runAction` rather than calling the
gated forms directly. IOAudioFamily caches its own `workLoop` member and runs
device power management on a private loop, so "the family always calls us on
our workloop" is precisely the sort of claim that ought not to be trusted when
`runAction` is correct from any thread and re-entrant-safe from the gated one.

**Engine start must be atomic with the demand latch.** This is the part most
likely to be got wrong by someone adding a second entry point, because it is
correct-looking code that fails only under a timing window.

`performAudioEngineStart` calls `LatSOFAudioDevice::engineStartCapture()`, which
is a **single** `commandGate->runAction(&s_engineStartCapture)`. Inside that one
gate closure, `startCaptureGated()` runs *and*, if it fails, `gWasCapturing` is
cleared:

```c
IOReturn LatSOFAudioDevice::s_engineStartCapture(OSObject *o, void *, void *, void *, void *) {
    auto *self = static_cast<LatSOFAudioDevice *>(o);
    IOReturn r = self->startCaptureGated();
    if (r != kIOReturnSuccess)
        gWasCapturing = false;   // same gate closure: nothing can interleave
    return r;
}
```

The invariant being defended: **an engine-originated failure must not leave the
demand latch armed.** `gWasCapturing` is a HAL-plugin-era mechanism — that
client could not re-ask, so a start refused during the wake window had to be
remembered and re-armed later by the wake retry path. The family world is the
opposite: `coreaudiod` re-issues `performAudioEngineStart` on client activity by
design. A latch left armed here would have the wake path start capture DMA with
no *Running* engine attached — and IOAudioFamily only delivers
`performAudioEngineStop` to a Running engine, so nothing in the system could
ever stop it. Headless DMA, self-re-arming across every subsequent sleep.

Splitting that into two `runAction`s — refuse, then clear — reintroduces exactly
that. `jackPoll` ticks every 500 ms on the same workloop and can win the gate in
the gap between the two, consume the still-armed latch, and start the headless
DMA the clear was about to prevent. One closure, or the bug is back.

`performAudioEngineStop` clears `engineRunning`, cancels the wrap timer and
calls `engineStopCapture()` (a plain gated `s_stopCapture`); the engine's
`stop()` cancels, removes and releases the wrap timer before chaining up.

## Siri's microphone gate

This deserves its own section, because the answer is not documented anywhere
and this project believed the wrong thing about it for a long time.

The facts below come from disassembling `AVFAudio` and `CoreSpeech` out of this
machine's dyld shared cache (Sequoia 15.7.7). They are property tests, every one
of them, and they are the *same* tests for a kext, for a userspace
`AudioServerPlugIn` and for a DriverKit driver:

- `+[AVVCAudioDeviceManager IsDeviceBuiltIn:]` returns true only when the
  transport type is `'bltn'` **and** the first entry of the input-scope
  `kAudioDevicePropertyDataSources` (`'ssc#'`) is `'imic'`.
- `+[AVVCAudioDeviceManager GetAudioDeviceBuiltInMicrophone]` — used by the
  `'voic'` (Hey Siri) activation path — additionally requires the *current*
  input-scope `kAudioDevicePropertyDataSource` (`'ssrc'`) to read `'imic'`.
- CoreSpeech's `CSSiriRecordingInfo` classifies the route by transport. A
  `'bltn'` device must present a data source of `'imic'` or `'emic'`, or the
  route comes back nil — which is what surfaces in the UI as "Connect a
  microphone" over a perfectly working record path. `'usb '`, `'blue'` and
  `'line'` pass with **no data-source check at all**. That last clause is the
  entire reason a USB microphone has always worked on this machine while the
  internal one did not.

And one anti-fact, because it costs everyone who searches for this problem a
day. The log line

```
AVVCAudioDeviceManager.mm:723 device ID : LatSOFKernelAudioEngine:0, supported : 0
```

is a red herring. That function is `IsSiriSupportedExternalDevice:`, and all it
does is test the device's ModelUID for the string `05AC:1114` — the USB VID:PID
of the Apple Studio Display. Every other audio device on earth returns 0 there,
including the built-in microphone of a real MacBook. It is not a failure report;
it is a check for one specific monitor.

**What the engine does to pass.** `initHardware` sets
`setDeviceTransportType(kIOAudioDeviceTransportTypeBuiltIn)` for the `'bltn'`
half. `createControls()` supplies the other half: an `IOAudioSelectorControl`
from `createInputSelector`, whose one available selection is
`kIOAudioSelectorControlSelectionValueInternalMicrophone` (`'imic'`), registered
with `addDefaultAudioControl`. IOAudioFamily bridges that control to the HAL's
input-scope DataSource property, which is what satisfies both the `'ssc#'` and
the `'ssrc'` test above. AppleHDA publishes its speakers by exactly this
mechanism, with an `'ispk'` selector — if you want to see the shape of the
answer before writing it, look there. `setDeviceModelName` and the stream's
`setTerminalType(INPUT_MICROPHONE)` (0x0201) finish the picture; the terminal
type defaults to 0, which reads as "not a microphone" to anything that asks.

**A correction worth recording.** This project previously held that Siri
filtered on device *class* — kernel IOAudioFamily devices admitted, userspace
`AudioServerPlugIn` devices excluded — and the kernel port was undertaken on
that basis. That belief was wrong. There is no class test anywhere in the path.
The gate is HAL properties and nothing else, and the old HAL plugin almost
certainly failed for want of an `'imic'` data source rather than for being
userspace; it could in principle have been fixed where it stood. The port still
earns its keep — AMFI blocks the unsigned plugin on this system, and
IOAudioFamily supplies the selector, transport and terminal-type plumbing for
free — but the reasoning that motivated it was not sound. Anyone repeating this
work on another machine should start from the property list, not from the
driver model.

**One consequence that looks like a fault.** With the device classified as a
genuine built-in microphone, macOS suppresses Siri's listening tone: built-in
mics report `supportsEchoCancellation == 0`, and a chime played through speakers
that the built-in mic can hear would be captured along with the user's voice.
The missing tone is correct behaviour, not a regression.

## How the kext is loaded

Linking `com.apple.iokit.IOAudioFamily` changed how this kext gets into the
kernel, and the failure mode is *silent*, so it is worth being explicit.

**OpenCore can no longer inject it.** Injected kexts are linked into the boot
kernel collection and may resolve symbols only against that collection and
against other injected kexts. `IOAudioFamily` is not in the boot collection — it
lives in the system collection, `SystemKernelExtensions.kc`. OpenCore therefore
drops the kext, and does so without an error anywhere: nothing in the OpenCore
log, nothing in `dmesg`, nothing in `kmutil showloaded`. The kext is simply not
there. If you add an IOAudioFamily dependency to an injected kext and it
vanishes without complaint, this is why.

The kext now installs to `/Library/Extensions` and is linked into the
**auxiliary** kernel collection by macOS itself:

```sh
sudo kmutil load -p /Library/Extensions/LatSOFAudio.kext
```

`kmutil` answering that the change "requires a reboot" is the *success*
message, not an error — the auxiliary collection is rebuilt and takes effect at
the next boot. Note that `kmutil install --update-all`, the more commonly cited
incantation, **fails on a hackintosh**: it demands a Kernel Debug Kit matching
the exact running build, which for a spoofed or mismatched build is not
obtainable. Use `load -p`.

**The dependency floor must be 200.5.** `Info.plist` declares
`com.apple.iokit.IOAudioFamily` = `200.5`. The `1.0.0b1` floor that older
IOAudioFamily kexts carry will not link: the family's compatible version is
`1.0`, and `1.0.0b1` sorts *below* `1.0`, so the requirement can never be
satisfied.

**Consequence for the borrowed-stream contract.** Auxiliary-collection kexts
load late — well after AppleHDA is fully up — where an injected kext started at
boot-collection time. The borrow of SD7 described below therefore now happens
against an AppleHDA that has finished initialising. That ordering is benign, and
arguably an improvement. But it is a real change in timing and it moves which
case you are testing: do not assume, as was safe before, that the first borrow
after a boot finds SD7 unprogrammed. The contract holds either way — that is
the whole point of snapshot-once/restore-once — but the forgiving cold-boot
state described under "Why this is easy to get wrong" is no longer guaranteed to
be what you are exercising.

## The HAL plugin, historically

`plugin/LatSOFAudioPlugin.driver` remains in the tree and is **no longer
installed**. It was a userspace CoreAudio server plugin derived from Apple's
`NullAudio` sample, reduced to input-only, and for most of this project's life
it was how the microphone reached CoreAudio. Its design is kept here because
parts of it are still load-bearing and the reasoning behind them has not
expired:

- **Input-only was, and remains, correct.** The plugin published no output
  stream or controls and refused `WriteMix`. Publishing a dead output invites
  apps — and Siri's voice-processing path in particular — to route audio into a
  void; playback belongs to AppleHDA. The kernel engine publishes exactly one
  stream, direction input, for the same reason.
- **The conditioning chain moved wholesale.** `ReadInput`'s S32 → Float32
  conversion, the one-pole DC blocker at `R = 0.98822`, the filter-before-gain
  ordering, the 32× gain scaled by the input slider, the clamp and the denormal
  flush are all now in `convertInputSamples`, described above. The plugin
  cleared filter state at capture start; the engine does too, and adds the
  multi-client and seeding machinery the kernel context needs.
- **The clock model moved in spirit.** `GetZeroTimeStamp` was disciplined
  against the hardware DMA position — the DPIB value read through the kext, with
  counted ring wraps, back-dated to the wrap instant, smoothed by a 1/8
  correction filter. The back-dating is what the engine's wrap timer now does.
  Worth remembering, because it is the kind of thing that gets miscredited: that
  design was *not* the fix for the playback failure described under "The
  interrupt-starvation bug" below. It was written chasing a clock-drift theory
  that a profiler disproved in one command, and kept because it is correct
  regardless (patch 13 in `history/README.md`).
- **Latency was reported as 0** — a known-wrong placeholder. The engine now
  reports 240 frames, also provisional; the measurement method that would settle
  it is described in the engine section above.

The plugin's Makefile encodes an install ritual (permission normalisation,
quarantine stripping, `ditto`, sign-installed-copy-last, verify gate);
`DEBUGGING-LOG.md` Phase 8 explains the `-67030` failure it prevents. That ritual no longer applies to
shipping this driver, but it is the correct procedure for anyone installing a
CoreAudio server plugin on a modern system, and the plugin is still the
reference for how the ring is consumed from userspace.

## The interrupt-starvation bug

Worth documenting in full, because anyone deriving from the same reference
driver will reproduce it, and the symptom points away from the cause.

The reference enables interrupts at both levels when starting a stream:
`INTCTL |= GIE | CIE | (1 << streamIndex)` at the controller, and
`SDCTL |= IOCE | FEIE | DEIE` at the stream. That is correct for a driver that
owns the device and services them. This driver owns nothing and services
nothing — and the BDL is one entry per 4 KB page, so `BCIS` latches roughly
**94 times per second** on an interrupt line shared with AppleHDA.

Nobody acknowledges any of it. AppleHDA's handler finds none of its own status
bits set and returns not-handled. A few hundred unhandled interrupts later the
kernel throttles the line — and that line is how AppleHDA receives *its*
interrupts. The result:

- speakers cut out about 5-6 seconds into any sustained capture
- the volume keys stop responding
- `coreaudiod` logs `HALS_IOA1Engine::EndWriting` failing with `0xE00002EE`
  (`kIOReturnIsoTooOld`) against the **output** engine, and then burns 20-27%
  CPU formatting overload reports about it
- everything recovers the instant capture stops, because clearing `RUN` ends
  the completions

Note the shape of that: capture works perfectly throughout. The thing that
breaks is playback, in a driver you did not write. A profile of `coreaudiod`
(`sudo sample coreaudiod 10`) shows none of this driver's frames at all — the
CPU is spent describing the problem, not causing it.

The fix is to enable nothing and clear the latched status at both ends.
Position has always come from polling DPIB, so nothing depended on those
interrupts existing.

## The borrowed-stream contract

The other rule that costs playback rather than capture, and the one that took
longest to find. Read this before changing anything in `initDSP()`.

The DSP's ROM code loader transfers firmware over an HDA output stream. The
driver picks `SD(numISS)` — SD7 here — which is **AppleHDA's first output
engine**. There is no arbitration protocol between two independent macOS
drivers, so this is a genuine loan of live hardware: we take a descriptor
AppleHDA owns, reprogram it, run DMA on it, and must hand it back exactly.

Moving the loader elsewhere looks tempting and was tried. It failed at cold
boot, and the reason is worth recording because it was initially misread as
"the hardware demands SD7": the attempt changed the descriptor index but left
the **stream tag** at 1 — and AppleHDA drives SD7 with tag 1 as well. The ROM
binds its code-load gateway by *tag*, not by descriptor index, so that put two
output descriptors on one tag. Relocating the loader may well be viable, but
only if the tag moves with it.

Given the loan is unavoidable, the contract is:

> **Snapshot once, restore once, never write the descriptor again.**

Concretely, in `initDSP()`: `SdSnapshot` captures `CTL`/`CBL`/`LVI`/`FMT`/`BDL`
plus the shared `PPCTL` and SPIB state before the loader touches anything, and
`sdRestore()` — idempotent, guarded by a `restored` flag — puts it back. It is
called on the normal path *and* from `cleanup:`, so the ROM-IPC and INIT_DONE
timeout paths, which jump straight over the normal hand-back, cannot leave
AppleHDA's descriptor pointing at our firmware buffer.

**Why this is easy to get wrong.** Cold boot forgives every violation. At boot
SD7 is unprogrammed (`ctl=0x040000 fmt=0x0000 bdl=0 cbl=0 lvi=0`) — there is
nothing there to damage, and AppleHDA configures it *after* us. On a wake it is
fully live (`ctl=0x140000 fmt=0x4031 bdl=0x1f1d9000 cbl=393216 lvi=95`), and
anything written after the hand-back destroys a running playback engine. So the
bug is invisible until the machine sleeps, and it manifests in a driver you did
not write.

**Verify with `SD-Final`, never with `SD-Return`.** `SD-Return` is published
immediately after the hand-back, while the function still has work to do — for
a week it faithfully reported a perfect restore on wakes where the speakers
were already dead, because the code that killed them ran later. `SD-Final` is
read after the last write the function makes to the descriptor, and must match
`SD-Borrow` field for field:

```sh
ioreg -rc LatSOFAudioDevice -d 1 -w0 | grep -E "SD-Borrow|SD-Final"
```

If those two ever disagree, something wrote SD7 after the hand-back, and the
differing field names it. No inference required.

## What was measured

- Capture DMA advances at exactly real-time rate (241k frames in 5.02 s).
- 100 % nonzero samples, both channels live, on the first successful run.
- Speaker playback verified working *during* capture (shared-function
  coexistence) and after stop.
- Sleep/wake: the kext rebuilds the DSP on wake, and the borrowed output
  descriptor comes back byte-identical. Verified over a cold boot plus four
  sleep/wake cycles — `SD-Borrow == SD-Final` on every one, all four with SD7
  fully configured by AppleHDA, covering both AC software sleep and battery
  clamshell sleep. Mic and speakers working after each; the mic confirmed from
  non-zero `Capture-Stop` ring data rather than by ear.
- The kernel engine carries dictation end to end, and Siri selects it as the
  input route (`RecordRoute: LatSOFKernelAudioEngine:0`), records, answers on
  screen and speaks — the first time on this machine. macOS Sequoia 15.7.7.
- The HAL plugin ran under stock AMFI with no library-validation exceptions
  when that measurement was taken. On the current system AMFI refuses to load
  it unsigned, which is one of the two standing justifications for the kernel
  port (the other being the plumbing IOAudioFamily provides for free).
