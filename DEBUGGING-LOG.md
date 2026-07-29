# Debugging log

How this driver actually got written, in order, including the parts that were
wrong. The patch series in `history/` corresponds to the numbered steps.

This is here because the finished code hides the expensive part. Every fix
below looks obvious in hindsight and none of them were, and three of them
presented as symptoms in a completely different subsystem. If you are porting
this to another Comet Lake machine, the failures will be more useful to you
than the architecture.

Three days. Two to get it working, one more to find out it did not survive
sleep — Phase 13 is that day, and it is the one most worth reading.

Phase 14 came later and is a different kind of entry. Nothing was broken; a
conclusion this project had already published was simply false. Finding that
out took nine hours and came close to costing the speakers.

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

Fixed in patch 11: keep `ADSPIC` bit 0 masked in steady state, and drain and
acknowledge `TDR` at capture start, after trigger, at stop entry, and after
`PCM_FREE`. (One caveat found much later: the firmware loader must unmask
that bit for the ROM handshake, so the real rule is that `initDSP()` masks it
again on every exit path — a post-fix audit found the failure paths didn't,
and closed them; see patches 25–26 in `history/README.md`.)

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

## Phase 13 — The telemetry that lied

The driver was declared finished at the end of Phase 12. Then it went to sleep.

After any sleep, audio broke. *Which* audio varied between wakes — sometimes
the mic, sometimes the speakers, sometimes both. A reboot always restored
everything. Worth separating two bugs here: the original failure was mic-only
and speakers survived; speaker failure appeared **later**, introduced by
patches that made wake-time re-init actually succeed. Succeeding is what
reached the code that did the damage.

Eight patches went in over a day: wait for `GCAP` to decode before deriving
anything from it, walk the PCI PM capability and force D0, wait for AppleHDA to
bring the link out of reset and then settle, save and restore the borrowed
stream, stop clobbering AppleHDA's `INTCTL` bit, re-map the BARs on wake,
replace the single synchronous wake attempt with a retry engine driven off the
existing 500 ms timer. Each was individually defensible. Several were necessary.
None of them fixed it.

By the end of that day the retry engine reported first-try success, a clean
borrow and restore, and `Status OK` on **every** wake — and audio still failed,
in three different combinations across four wakes. Two of the patches required
booting Windows to recover the machine.

The mistake was structural, and it is the transferable part: each patch was
simultaneously the experiment and the cure. When one appeared to help there was
no way to tell which of its changes did it, and the driver's own telemetry was
the only instrument. So the next step was not another patch — it was reading
the post-load path line by line, looking for writes rather than theorising about
registers.

There were three, all after the stream had been handed back:

```c
// Clean up FW loader state — NO GCTL reset (would break DSP DMA)
if (spibCap) wr32(hda, spibCap + 0x04, 0);   // zeroes SPIB for ALL streams
streamReset(hda, sd);                        // resets AppleHDA's SD7 again
```

and, further down, a `cleanup:` block zeroing `BDLPL`/`BDLPU`/`CBL`/`LVI`. That
one is the real culprit, and it hid in plain sight for a reason worth
internalising: **`cleanup:` is not only a `goto` target — the success path falls
straight into it.** It reads as error handling. It ran on every successful init,
wiping the DMA descriptor of a stream we had just promised to return untouched.
The same block also ran on the two IPC-timeout paths, which `goto cleanup` and
so jump clean over the hand-back entirely.

And the reason a week of measurement pointed nowhere: `SD-Return`, the property
reporting the restore, is written *immediately after* the restore — before all
three of those. It reported a perfect hand-back, accurately, every time. The
damage happened afterwards.

The fix was not to delete three lines but to make the violation unrepresentable:
snapshot into a struct, restore through one idempotent function called from
every exit path, and add `SD-Final`, read after the last write the function
makes. `SD-Borrow` and `SD-Final` must match field for field. Four sleep/wake
cycles later — including one on battery through a clamshell close — they did.

Two conclusions from earlier in the week also turned out to be wrong, which is
its own lesson about how confidently a debugging session records things. "The
loader cannot be moved off SD7, the hardware cares which descriptor carries the
code load" was a stream-tag collision: the attempt moved the descriptor and left
the tag at 1, which is the tag AppleHDA already drives SD7 with. And "the mic
dies because coreaudiod holds a stale IO session" was untrue — macOS tears the
input session down across sleep, so the re-arm written to fix it never fires.

**Lesson:** telemetry that reports success proves only that the reporting line
ran. If a function keeps working after it publishes a result, the result
describes an intermediate state, not the outcome. Read the register *after* the
last write, and when you borrow another driver's hardware, make the give-back
idempotent and call it from every exit path — including the ones that fail.

---

## Phase 14 — Siri hears the laptop

This phase starts from a conclusion this project had already written down, in
the README, in confident language: *Siri's device selector admits only
kernel-published `IOAudioFamily` devices and excludes userspace
`AudioServerPlugIn` devices by class.* It came out of an experiment that looked
decisive. With a custom output-only AppleALC layout (id 90) making the HAL
plugin's microphone the **only** input device in the entire system, Siri still
said "Siri Not Available — Connect a microphone" and `corespeechd` logged
`deviceId = (null)` — while a USB dongle passed the same filter on the same
machine on the same day. One device is admitted, one is refused, no preference
or layout changes it, and the only difference left standing is the class. That
is about as clean as a conclusion drawn from black-box behaviour ever looks.

It was wrong. Everything below is it coming apart, and the driver arriving at
the right destination for the wrong reason.

### Three review rounds, and four bugs that never reached hardware

A kernel `IOAudioEngine` port was written on that theory —
`LatSOFKernelAudio.cpp`/`.hpp` — and then, before it was allowed near the
machine, put through three rounds of adversarial review: seven major findings,
then one, then none. Four of them are worth naming because they were real, and
because of what they have in common.

- The `Info.plist` declared an `IOAudioFamily` dependency floor of **`1.0.0b1`**.
  The family's compatible version is `1.0`, and `1.0.0b1` sorts *below* `1.0`.
  The kext could not have linked. It would simply never have loaded, and the
  first hardware session would have been spent hunting a phantom.
- The wrap detector's timestamps were not back-dated, injecting 0–100 ms of
  clock jitter into a driver whose whole timing story is counted DMA wraps.
- Clearing the demand latch after a *failed* engine start used a second,
  separate command-gate `runAction`. The 500 ms `jackPoll` tick could interleave
  between the refusal and the clear, start headless DMA, and leave nothing in
  the system able to stop it.
- The DC-filter state guard required `dcNextFrame == 0` to carry filter state
  across blocks. The audio family's first read is rarely at frame 0, so the
  filter would have restarted on **every** block — a repeated DC step, which is
  to say a sound indistinguishable from the crackle an earlier evening had
  already been spent chasing.

Note what that list is: four bugs, none of them found by running anything. The
`1.0.0b1` one in particular is invisible to every test you could run except
"boot the machine and observe nothing happening", which is the most expensive
test available. Reading code you just wrote, adversarially, in rounds, with the
explicit goal of finding reasons it cannot work, is cheaper than a reboot.

### The install that was perfect and did nothing

The kext went into `EFI/OC/Kexts`, md5-verified in place, and the machine
rebooted. The kext did not load. There was no error — not in `dmesg`, not in
the system log, nothing to grep for. The bundle was simply absent, as though it
had never been listed.

The instinct at that moment was to suspect the code, and that instinct is the
trap. The install was verifiably correct: the right bytes, in the right
directory, with the right `config.plist` entry. When the deployment is provably
fine, the fault "must" be in the thing deployed. It was not.

`kmutil inspect`, piped through `awk` to attribute every loaded kext to the
collection it lives in, showed **`IOAudioFamily` is in the System kernel
collection**. OpenCore injects into the *Boot* kernel collection and can only
link injected kexts against that collection. It hit a dependency it could not
resolve, dropped the bundle, and said nothing at all about it.

**Lesson:** on modern macOS, "which kernel collection is my dependency in?" is a
question you should answer before the first install, not after the first silent
failure. And a deployment that verifies perfectly is not evidence about the
code.

### The tempting wrong fix, which was nearly taken

The obvious repair is to inject a copy of `IOAudioFamily` into the Boot
collection too. This is not a wild idea; **this machine already does exactly
that for Wi-Fi.** `IOSkywalkFamily.kext` and `IO80211FamilyLegacy.kext` sit in
its EFI right now, and the loaded `IOSkywalkFamily`'s UUID matches the EFI
copy — the injection demonstrably works and the machine has been running on it
for months.

So the work was done. A KDK (15.7.4, build 24G517) was downloaded,
`IOAudioFamily.kext` extracted along with `vecLib.kext` — its own dependency,
itself System-collection-only — both staged, and a patched `config.plist`
prepared. Everything was ready to install.

Research killed it, with about ten minutes to spare. Two findings:

- On Sequoia the kexts in `/S/L/E` are binary-less stubs. The KDK is not a
  convenience here, it is the *only* source of the binary at all — which should
  itself have been a signal about how far off the beaten path this was.
- Nobody in the kernel-collection era has demonstrated an injected
  `IOAudioFamily` coexisting with a **live** AppleHDA. The injected copy would
  win registration, and AppleHDA — statically prelinked against the System
  collection's own copy — would be left in a mixed-linkage state that nothing
  in the ecosystem has tested.

The Wi-Fi precedent does not transfer, and the reason is precise: there,
OpenCore can `Block`/`Exclude` the original, so exactly one copy exists. You
cannot block a kext in the System collection. The precedent only proves that
injection works *when you can remove the duplicate*.

And the answer already existed. acidanthera/bugtracker **#1658** is a kX/
VoodooHDA user hitting this identical wall on 11.3, and the upstream
maintainer's reply is one line long: use `/Library/Extensions`. A closed issue
from five years ago, describing this exact problem, with the fix in it.

This is recorded as the near miss it was. The thing at risk was not the
microphone project — it was the speakers, on a machine whose recovery path runs
through booting Windows to edit `config.plist`.

### The route that worked, and a success that reads as an error

Install to `/Library/Extensions` and let macOS's own linker resolve the
dependency into the **auxiliary** kernel collection, which can link against
System. That is all.

Two wrinkles worth recording for anyone following:

`sudo kmutil install --update-all` **fails**, with `Missing Developer Kit: …
you will need to install a KDK matching your build 24G720`. That subcommand
rebuilds the Boot and System collections, which is not what is wanted and
requires a KDK for the exact running build. The correct command only touches
the auxiliary collection:

```
sudo kmutil load -p /Library/Extensions/LatSOFAudio.kext
```

and its **success** message is:

```
Error Domain=KMErrorDomain Code=28 "Loading extension(s): com.hackintosh.LatSOFAudio requires a reboot"
```

That is the load working. It is shaped exactly like a failure — the word
`Error`, a domain, a numeric code — and it means "done, reboot to see it". No
SIP change was needed (`csr-active-config` was already `0x803`) and no approval
dialog ever appeared.

### The half-victory

After reboot: `ioreg -rc LatSOFKernelAudioDevice` found the device, CoreAudio
showed **LatSOF DMIC capture**, Transport: Built-in, selected as default input.

Then clicking Siri produced, for the first time in the history of this project:

```
RecordRoute: LatSOFKernelAudioEngine:0
```

That field had been `deviceId = (null)` in every previous experiment. Siri
opened an `AudioQueue` on this driver's DMA ring and recorded from it for about
140 ms.

And then the panel said "Siri Not Available — Connect a microphone" and
cancelled the session.

The honest reading of that moment is that the kernel-vs-userspace theory had
just been half-confirmed and half-refuted in the same second. Selection now
worked. Admission still did not. A theory that explains one of those and not the
other is not a theory, and it should have been abandoned right there instead of
one debugging step later.

### The red herring: a log line about a monitor

The line that looked like the answer, sitting right at the failure:

```
AVVCAudioDeviceManager.mm:723  device ID : LatSOFKernelAudioEngine:0, supported : 0
```

`supported : 0`, naming our device, at the exact moment of refusal. It is hard
to imagine a more inviting piece of evidence.

Disassembly put line 723 inside
`+[AVVCAudioDeviceManager IsSiriSupportedExternalDevice:]`, which reads
`kAudioDevicePropertyModelUID` and tests whether the string contains
**`05AC:1114`** — the USB vendor and product ID of the Apple Studio Display.

It returns 0 for every audio device in the world except that one monitor. A
real MacBook's built-in microphone fails it too. Anyone who chases that log line
is debugging a check about a display.

**Lesson:** a log line that names your device and reports failure is not
necessarily about your device's failure. Find the code before believing the
string.

### The actual gate, which had been in the logs for hours

Two independent disassembly passes — AVFAudio and CoreSpeech, taken from this
machine's own dyld shared cache — converged on the same rule:

- For a device whose transport type is **`'bltn'`**, CoreSpeech requires the
  **input-scope** HAL data source to read `'imic'` or `'emic'`. Otherwise it
  nils the route. AVFAudio's built-in-mic finder demands `'imic'` specifically.
- Devices with transport **`'usb '`** skip the data-source test entirely.

That second bullet is the whole explanation of the USB dongle, retroactively,
for weeks of experiments. The dongle was never passing a class filter. It was
skipping a test.

This driver reported transport `'bltn'` correctly, and published **no data
source at all**. Which had been visible the entire time, as two property-read
failures that had been sitting in the logs being read as noise:

```
_AudioObjectGetCFTypeRef  Failed getting audio property doml
_AudioObjectGetIntValue   Failed getting audio property crss
```

The fourccs are byte-reversed in that output. `crss` is `ssrc` —
`kAudioDevicePropertyDataSource`. `doml` is `lmod` —
`kAudioObjectPropertyModelName`. The log had been printing the name of the
missing property, repeatedly, for hours, and it read as boilerplate because it
appears in a burst alongside genuinely irrelevant probes.

**Lesson:** a failed property read is a system telling you the name of the thing
you did not implement. Reverse the fourcc before deciding it is noise.

### The fix, which is about eight lines

An `IOAudioSelectorControl` from `createInputSelector`, with one available
selection set to `kIOAudioSelectorControlSelectionValueInternalMicrophone`
(`'imic'`), registered with `addDefaultAudioControl`. This is not a workaround;
it is the same mechanism AppleHDA uses to publish its speakers as `'ispk'`,
confirmed by reading AppleHDA's own `IOAudioSelectorControl` out of `ioreg`
before writing a line.

Plus `setDeviceModelName` and a stream `setTerminalType(INPUT_MICROPHONE)` to
answer the other unexplained property error rather than leave it in the log for
the next person to dismiss.

Rebuild, replace, `kmutil load -p`, reboot.

### The result, and the last trap

Siri hears the internal microphone, understands it, and answers on screen.

And was silent. Two things then looked broken and were not:

- **The listening tone no longer plays.** This is deliberate macOS behaviour:
  the beep is suppressed when `supportsEchoCancellation` is 0 on a built-in
  microphone, because the microphone would otherwise hear it. The tone this
  project remembered was the *external*-mic code path, from all those months of
  testing with a dongle attached.
- **Ducking looked suspicious** and was verified healthy — clean mute/restore
  pairs throughout the log.

The actual cause of the silence: **System Settings → Apple Intelligence &
Siri → Siri Responses → Voice feedback was turned off.**

Nine hours of kernel work, a KDK, two disassembly passes and a near-miss with
the audio family, and the final defect was a preference toggle.

### The correction that matters

Siri never discriminated kernel devices from userspace ones. The gate is purely
HAL properties — transport type, and an input-scope data source — and it is
identical for kexts, HAL plugins and DriverKit drivers. The original HAL plugin
almost certainly failed for want of an `'imic'` data source, and might well have
worked all along with a few extra lines of property publishing.

The kernel port is still the right destination: AMFI blocks the unsigned plugin
on this machine, and `IOAudioFamily` supplies the selector, transport and
terminal-type plumbing for free, correctly, without reimplementation. But the
reason written in the docs for weeks — the one that motivated the entire port —
was false, and the experiment that established it was a valid experiment
answering a different question than the one it was asked.

This log is only worth anything if it does not flatter itself, so: the
conclusion in Phase 12's spirit ("know which questions your instruments can
actually answer") applies to reasoning as much as to tools. A single-variable
experiment isolates a variable. It does not tell you that the variable you
isolated is the one the system is testing.

**Lessons, the transferable ones:**

- A success can be shaped like an error. `Code=28 … requires a reboot` is a
  load that worked. Read the message, not the word `Error`.
- A log line naming your device at the moment of failure may be a check about
  something else entirely — in this case, one specific monitor.
- Property-read failures dismissed as noise were the defect. The system printed
  the name of the missing feature, byte-reversed, over and over.
- The community's answer to your exact problem may already exist in a
  five-year-old closed bug report. Search before you engineer; #1658 was worth
  more than the KDK.
- Adversarial review before hardware caught four real bugs, one of which
  (`1.0.0b1`) would have made the kext silently unloadable — a symptom
  indistinguishable from the OpenCore failure that came next, which would have
  made both of them much harder to find at once.

---

## If you are porting this

Read `PORTING.md` for the procedure. From this log, the six things most likely
to cost you a day:

1. Verify under Linux first. Do not write kernel code to answer a hardware
   question.
2. Enable no interrupts, at any level, and clear `SDSTS` at both ends. The
   symptom of getting this wrong is *playback* breaking.
3. Never start DMA at boot.
4. Profile before theorising, and check whether the CPU you are looking at is
   actually yours.
5. If you borrow a stream descriptor another driver owns — and the code loader
   forces you to — snapshot it, restore it through one idempotent function
   called from every exit path, and verify by re-reading the descriptor after
   your last write to it. Cold boot forgives every mistake here, because the
   descriptor is still empty; the machine has to sleep before the bug exists.
6. If you publish a kernel `IOAudioEngine`, install it to `/Library/Extensions`
   with `kmutil load -p` — not to your EFI. `IOAudioFamily` lives in the System
   kernel collection and OpenCore can only link injected kexts against the Boot
   one, so an EFI install fails silently with no log line anywhere. Then publish
   an input `IOAudioSelectorControl` reading `'imic'` alongside the built-in
   transport type. Without it, Siri will find your device, open it, record from
   it, and refuse it.
