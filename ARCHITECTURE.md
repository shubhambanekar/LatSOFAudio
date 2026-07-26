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
  SOF firmware boots fine without it.
- **Stream-descriptor partitioning.** The controller advertises 7 input
  DMA engines (GCAP `0x9701`); AppleHDA uses SD0. This driver uses input
  descriptor **SD1 with stream tag 2** and touches nothing else.
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
  speakers until reboot. This driver never unmasks `ADSPIC` bit 0, and it
  drains/acks TDR at capture start, after trigger, and at stop. All IPC
  waits are bounded polls (≤500 ms).

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
samples. Speech peaks around −35 dBFS; amplification is deliberately done
in userspace (below), not in the kext.

IPC error decode, for anyone extending the pipeline:
`0xffffffea` = `-EINVAL` (params/topology mismatch),
`0xfffffff4` = `-ENOMEM` (shrink period/buffer),
`0xfffffffa` = `-EBUSY` (stream-tag collision).

## Kext ↔ userspace interface

A `IOUserClient` exposes: method 4 = start capture, 5 = stop, 6 = read
position; `clientMemoryForType(1)` maps the capture ring (S32LE,
interleaved stereo, 48 kHz) into the calling process. Capture is
refcounted around the HAL's `StartIO`/`StopIO`. Nothing ever starts DMA
at boot: an early boot-time self-test started a DMA engine while AppleHDA
was still initialising the controller and hung the machine — capture is
strictly on-demand from a fully booted system.

## The HAL plugin

`LatSOFAudioPlugin.driver` is a userspace CoreAudio server plugin derived
from Apple's `NullAudio` sample, reduced to **input-only**: no output
stream or controls are published, and `WriteMix` is refused. Publishing a
dead output here would invite apps (and Siri's voice-processing path) to
route audio into a void — playback belongs to AppleHDA.

Per IO cycle, `ReadInput`:

1. maps the ring position from the cycle's sample time,
2. converts S32 → Float32,
3. applies a one-pole **DC-blocking high-pass**
   (`y[n] = x[n] − x[n−1] + R·y[n−1]`, `R = 0.98822` ≈ 90 Hz corner at
   48 kHz) — raw PDM output carries a DC offset, and filtering *before*
   gain keeps that offset from eating headroom,
4. applies gain: a fixed 32× (≈ +30 dB, landing speech near −5 dBFS)
   scaled by the input-volume slider,
5. clamps and writes interleaved stereo Float32.

Filter state is cleared at capture start; denormals are flushed (a
geometric decay into denormal range would cost real CPU inside a realtime
callback).

**Latency** is reported as 0, and honestly so: measuring it properly is harder
than it looks. The obvious approach — play a burst, record it, subtract the
output device's reported latency — fails, because the arrival timestamps come
from this plugin's own clock model. That measures the suspect clock with the
suspect clock, and duly returns physically impossible negative values. A valid
measurement needs a second input device you trust (any USB microphone):
capture the same burst on both in one run, and the difference in arrival times
is this device's excess latency, with our timestamps out of the loop. Until
then the architecturally defensible value is safety offset + buffer size.

**Clocking** is the other honest weak point: `GetZeroTimeStamp` is disciplined
against the hardware DMA position (counted ring wraps, back-dated to the wrap
instant, with a 1/8 correction filter), which is the right design — but note
that this was *not* the fix for the playback failure above, and chasing it
first cost real time. The DPIB hardware position is available and disciplining
the timestamp stream against it is the intended next step; until then,
very long recordings risk a periodic glitch when the drifted read point
crosses the DMA write point.

The plugin's Makefile encodes an install ritual (permission
normalisation, quarantine stripping, `ditto`, sign-installed-copy-last,
verify gate). `INSTALL.md` explains the failure it prevents.

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

## What was measured

- Capture DMA advances at exactly real-time rate (241k frames in 5.02 s).
- 100 % nonzero samples, both channels live, on the first successful run.
- Speaker playback verified working *during* capture (shared-function
  coexistence) and after stop.
- Sleep/wake: the kext rebuilds the DSP on wake; recording works after a
  lid-close cycle.
- Plugin runs under stock AMFI with no library-validation exceptions.
