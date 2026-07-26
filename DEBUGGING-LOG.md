# Debugging log

How this driver actually got written, in order, including the parts that were
wrong. The patch series in `history/` corresponds to the numbered steps.

This is here because the finished code hides the expensive part. Every fix
below looks obvious in hindsight and none of them were, and three of them
presented as symptoms in a completely different subsystem. If you are porting
this to another Comet Lake machine, the failures will be more useful to you
than the architecture.

Two days, roughly twenty hours.

---

## Phase 0 — Establish the hardware can do it at all

Before writing any macOS code: boot Linux and record from the microphones.
This sounds like a formality. It is the single most valuable step, because it
converts "can this work?" into "why doesn't my code work?" — and those are very
different questions to debug.

Linux recorded real audio via SOF with `sof-cml.ri` (IPC3), DMIC on capture
device 6 at 48 kHz. Five diagnostic tarballs were archived at this point: the
firmware and its checksum, DSP memory layout, DMIC topology and NHLT
configuration, DMA parameters, working mixer state, a proof-of-capture WAV, and
the ALC236 pin map.

Those dumps were consulted repeatedly over the following two days, including
for a problem that had nothing to do with the microphone. Take them.

**Also learned here:** the ALC236's analog microphone pins (nodes `0x19` and
`0x1a`) both read `Pin Default 0x411111f0` — not connected. The codec has mic
pin *widgets* but they are unwired on this board, so the headset jack cannot
carry a microphone either. The DSP genuinely was the only route. This is worth
checking before committing to a project like this, because it determines
whether a $15 USB microphone would have solved the problem instead.

## Phase 1 — Feasibility, read-only

Rather than start from the reference driver, a throwaway kext was written first
whose only job was to answer architectural questions without changing
anything. It matched `IOResources` and walked the IORegistry rather than
claiming the PCI device.

What it established:

- The kext loads and coexists with AppleHDA without displacing it
- BAR4 maps and reads correctly from outside the owning driver
- `ADSPCS` reads `0x00000f0f` — all four DSP cores in reset and stalled,
  unpowered. AppleHDA never touches the DSP. That was the critical unknown:
  the DSP was genuinely free real estate.
- `GCAP` reads `0x9701` — seven input DMA streams available, where AppleHDA
  needs about one

An earlier reading of all-ones caused an afternoon of doubt before turning out
to be a timing artifact: memory decode is not enabled yet during boot-time
driver matching.

**Lesson:** a read-only reconnaissance kext costs a couple of hours and
de-risks the entire project. Do it before forking anything.

## Phase 2 — Fork and the invisible `probe()`

Forked DexterSLamb/CmlSOFAudio, deleted the I2S codec path, kept DSP boot and
the DMIC pipeline.

First bug, and a nasty one: the driver class overrides `probe()` and casts its
provider to `IOPCIDevice`. Changing the provider to `IOResources` made that
cast fail — **silently**. `start()` was simply never called. No panic, no log,
nothing to grep for. `probe()` had to be patched alongside `start()`.

**Lesson:** in IOKit, a failed provider cast in `probe()` looks exactly like a
driver that didn't match.

## Phase 3 — Boot the firmware without breaking anything

The reference driver performs a global `GCTL` controller reset during
bring-up. It owns its device; this one does not, and a `GCTL` reset would
destroy AppleHDA's state. This was the largest open risk in the whole project:
if firmware boot required the reset, coexistence was impossible.

It did not. `FW_ENTERED` and `FW_READY` both confirmed, with AppleHDA holding
the HDA controller throughout and audio still working.

Then the IPC transport was proven rather than assumed:

- Read the firmware tag from the uplink mailbox at `0x81000` — got `57864`,
  matching what Linux reported. Mailbox address confirmed.
- Sent a deliberately invalid IPC and got `-EINVAL` back. Downlink at `0x82000`
  confirmed, in the only way that actually proves a bidirectional channel.

**Lesson:** prove the transport with a deliberate error before trusting a
success. A success can be a coincidence; a correctly-shaped failure cannot.

## Phase 4 — The machine hang

With the DMIC pipeline created (eight configuration IPCs accepted, derived from
this board's own NHLT data), a boot-time capture self-test was added to verify
DMA.

It hung the machine. Not a code bug — every loop is bounded — but because it
started a DMA engine during boot while AppleHDA was still initialising the same
controller. Recovery was done by editing `config.plist` from Windows.

The self-test was reverted permanently and replaced with a userspace tool
driving the kext's UserClient, so capture is only ever triggered by hand on an
idle, fully-booted machine.

**Lesson:** never start DMA at boot on a shared controller. And have a tested
recovery path before you need it — `mountvol X: /s` from Windows, or a USB with
a known-good EFI.

## Phase 5 — DMA that never moved

`startCapture` and `stopCapture` both returned success. The DMA position stayed
at zero for the entire run. Twice.

The cause was found by reading the source rather than adding logging:
`initDSP`'s `PPCTL` decouple write uses the member `capIdx` about 270 lines
*before* it is assigned. It therefore ran with the constructor's zero — so
**SD1 was never decoupled, and AppleHDA's SD0 was decoupled instead, on every
single boot.** That had been happening silently and harmlessly for days, which
is why nothing audible ever broke and why nobody noticed.

Meanwhile `startCaptureGated` had no `PPCTL` write of its own. A coupled input
stream sits waiting on the HDA link for tag-2 data that no codec will ever
send. `RUN=1`, IPCs acknowledged, position pinned at zero forever.

Fixed in patch 10: decouple the correct descriptor at capture start, re-couple
at stop, and set `no_stream_position=1` as a poll-only driver requires.

**Lesson:** when a hardware register write does nothing, check *when* the value
it depends on was assigned. And a bug that silently misconfigures someone
else's stream can hide indefinitely.

## Phase 6 — The speakers died

After the failed capture attempts, speaker output stopped working entirely
until reboot.

Mechanism: initialisation unmasked the DSP IPC interrupt (`HIPCCTL=0x03`,
`ADSPIC |= 1`), but firmware→host `HIPCTDR` notifications were only
acknowledged during init. The failed `TRIG_START` left the firmware posting
notifications nobody acked, holding the shared `00:1f.3` interrupt line
asserted until macOS throttled it — and that line is how AppleHDA receives
*its* interrupts.

Fixed in patch 11: never set `ADSPIC` bit 0 at all, and drain and acknowledge
`TDR` at capture start, after trigger, at stop entry, and after `PCM_FREE`.

**Lesson (first time):** on a shared PCI function, an interrupt you enable and
don't service breaks the *other* driver. Note that this lesson had to be
learned twice — see Phase 11.

## Phase 7 — Real audio

Patches 10 and 11 applied, one rebuild, one reboot:

- 241,000 frames in 5.02 seconds — exactly real-time rate
- 100% nonzero samples, both channels alive
- Peak 1.8% of full scale, as expected with no gain stage in the pipeline
- `ppctl=0xc0000002` — SD1 decoupled and nothing else
- Stop-time DPIB matched the tool's frame count exactly
- **Speakers still worked afterwards** — the first capture attempt that didn't
  kill audio

The recording contained recognisable speech. First internal microphone audio
captured on macOS on this hardware.

Gain was deliberately left out of the kext and placed in userspace, where it
becomes the input volume slider.

## Phase 8 — The signing saga

The CoreAudio HAL plugin was adapted from the reference (which had to be
recovered from GitHub, since the fork script had deleted the `plugin/`
directory) into an input-only device across 26 anchored edits.

Then it refused to load. `coreaudiod`'s driver service reported an invalid code
signature, with Security error **-67030** (`errSecCSInfoPlistFailed`) logged
twice before each refusal.

Hours went into the wrong explanation: that Sequoia had tightened policy
against ad-hoc signatures. Countermeasures included the `amfi=0x80` boot
argument and a library-validation override. **Neither was needed.**

The actual cause: the `Info.plist` had been downloaded by a browser and carried
a restrictive file mode. `sudo cp` preserved that mode. The audio service runs
as an unprivileged user and could not **read** the file it needed to hash —
while root could, so every signing operation "succeeded" and every
verification failed. macOS reports an unreadable `Info.plist` as an invalid
signature.

The fix is `chmod`, a clean `ditto`, signing the *installed* copy last, and a
`codesign --verify` gate before restarting `coreaudiod`. All four steps are now
encoded in the plugin's Makefile with a comment explaining why, so the failure
cannot recur.

**Lesson:** -67030 means "could not read", not "policy rejected". More
generally: when a security error appears under `sudo` but not otherwise,
suspect file permissions before suspecting policy.

## Phase 9 — Permissions, and a boot hang

The device appeared in Sound settings and the input meter moved. QuickTime
then failed to record with "the operation could not be completed", and the
Privacy → Microphone list was completely empty even after QuickTime asked.

That was `amfi=0x80` — the boot argument added during Phase 8 for a problem it
never solved. It breaks TCC permission prompts system-wide.

Removing it hung the machine at the Apple logo. Recovery from Windows again.
The reason turned out to be unrelated to audio: this EFI runs the OCLP
modern-WiFi stack, and *that* is what needs AMFI relaxed. Replacing the boot
argument with AMFIPass (plus `-amfipassbeta`, since that build predates
Sequoia) satisfied the WiFi stack without breaking permissions.

QuickTime then recorded. Both security relaxations were subsequently removed
and verified unnecessary — the machine runs stock AMFI enforcement.

**Lesson:** an unexplained boot hang after removing a boot argument means
something else in your EFI depended on it. Find out what before restoring it
blindly.

## Phase 10 — Working, and then not

Dictation worked system-wide. Sleep and wake survived. The driver was declared
finished.

Then: during a FaceTime call and during any sustained recording, **the speakers
would cut out about five or six seconds in**, the volume keys would stop
responding, and everything recovered the instant capture stopped. On calls, the
far side heard clean audio that degraded into crackle.

The five-second proof-of-capture test in Phase 7 had passed by a margin of
about one second.

## Phase 11 — One wrong theory, one profiler, one real fix

The theory was clock drift. The plugin's `GetZeroTimeStamp` was inherited from
Apple's NullAudio sample, which manufactures a timeline from
`mach_absolute_time()` at a *nominal* 48 kHz with nothing tying it to the DSP's
independent DMA clock. `coreaudiod` was logging
`HALS_IOA1Engine::EndWriting` failing with `0xE00002EE` — `kIOReturnIsoTooOld`,
"timestamp for the distant past" — and burning 25% CPU. It fit.

A probe was written to determine what the kext's position method actually
returns (frames within a 16384-frame ring, wrapping about 2.9 times per second,
not a running total). Patch 13 rewrote the clock to derive sample time from
counted DMA wraps with a correction filter.

**It changed nothing.** Identical symptoms, identical CPU, and the error count
over three minutes was still 5,752.

So: profile instead of theorise. `sudo sample coreaudiod 10` — and this
driver's symbols appeared **zero times**. The 25% CPU was `coreaudiod`
formatting its own overload reports: `SendAnyPendingOverloadReports` →
`AudioAnalyticsSendMessage` → Swift dictionary description → `NSNumber` →
CFString formatting. It was burning CPU *describing* the problem, not causing
it.

That reframing found the real cause in the kext, one register decode away.
`startCaptureGated` enabled interrupts at **both** levels — `INTCTL |= GIE |
CIE | SIE(capIdx)` at the controller and `SDCTL |= IOCE | FEIE | DEIE` at the
stream — in a driver that is poll-only, has no interrupt handler, and clears
`SDSTS` exactly once at start and never again. The BDL is one entry per 4 KB
page, so `BCIS` latched roughly **94 times per second** on the interrupt line
shared with AppleHDA.

AppleHDA's handler found none of its own status bits set and returned
not-handled. A few hundred unhandled interrupts later the kernel throttled the
line, AppleHDA stopped receiving its interrupts, and its output engine starved.
Five to six seconds. Recovery on stop, because clearing `RUN` ends the
completions.

Patch 14: clear our `SIE` bit rather than setting it, mask the stream interrupt
enables, set `RUN` alone, clear `SDSTS` at both ends. Nothing depended on those
interrupts — position has always come from polling DPIB.

Error count over three minutes: **5,752 → 0.** Speakers survive multi-minute
recordings. FaceTime calls are clean, which also confirmed the crackle was
never a separate bug.

**Lessons, and these are the ones worth carrying:**

- Profile before theorising about a CPU symptom. One command disproved an hour
  of reasoning.
- High CPU in a process does not mean high CPU in *your* code. It may be that
  process working hard to report your fault.
- This is the same lesson as Phase 6. It was missed the second time because the
  first fix addressed the DSP mailbox interrupt specifically, and the stream
  descriptor's own enables were a different mechanism in a different function.
  Fixing an instance is not fixing a class.
- A symptom in someone else's subsystem is still your bug.
- Patch 13 stayed in. Hardware-disciplined timestamps are the correct design
  even though they fixed nothing here.

## Phase 12 — Knowing when to stop

Two things were investigated and deliberately abandoned.

**Latency.** The plugin reports `kAudioDevicePropertyLatency = 0`, inherited
from NullAudio along with a comment explaining that the device "always vends
silence". A measurement tool was written: play a burst, record it, subtract the
output device's reported latency. It returned **negative latency** across six
runs — physically impossible.

The flaw was circular: arrival timestamps come from the input stream, which
this plugin generates. It measured the suspect clock with the suspect clock.
No amount of care in the test conditions can fix that, and rerunning it would
have produced the same impossible answer more precisely. A valid measurement
needs a second input device that is known-good.

**Echo.** The microphones pick up the speakers acoustically, which raised a
reasonable worry about echo on calls. The definitive test was a real call: the
far party never heard themselves. Echo cancellation works, which incidentally
proves macOS's voice-processing path accepts this device as a proper input, not
merely a recorder. At full speaker volume the far side can faintly hear local
music — expected on any laptop, since a linear echo canceller cannot model
speaker distortion.

**Lesson:** know which questions your instruments can actually answer. A tool
that returns an impossible result is telling you about itself.

---

## If you are porting this

Read `PORTING.md` for the procedure. From this log, the four things most likely
to cost you a day:

1. Verify under Linux first. Do not write kernel code to answer a hardware
   question.
2. Enable no interrupts, at any level, and clear `SDSTS` at both ends. The
   symptom of getting this wrong is *playback* breaking.
3. Never start DMA at boot.
4. Profile before theorising, and check whether the CPU you are looking at is
   actually yours.
