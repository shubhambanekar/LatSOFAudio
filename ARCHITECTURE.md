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
- Plugin runs under stock AMFI with no library-validation exceptions.
