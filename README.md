# LatSOFAudio

Internal microphone driver for the Intel SOF DSP on **Comet Lake** hackintosh
laptops, tested on the **Dell Latitude 3410** under **macOS Sequoia**.

Comet Lake laptops with PDM digital microphones (DMICs) have no working
internal mic under macOS: the mics are wired to the Intel Smart Sound DSP
(PCI `8086:02c8`), which Apple has no driver for, and on many boards
(including this one, ALC236) the codec has **no analog mic pin at all** —
so the headset jack cannot carry a mic either. This project boots the open
SOF firmware (Sound Open Firmware, Intel's open-source DSP firmware) on that
DSP, builds a DMIC capture pipeline, and publishes the result to macOS as a
normal CoreAudio input device.

**Status: working.** 48 kHz / 2-channel stereo capture — QuickTime, system
Dictation, FaceTime, browser conferencing, live input metering, and **Siri**,
on the bare laptop with nothing plugged in. Survives sleep/wake, self-heals if
the DSP wedges, and runs under stock AMFI with no security boot-args.

To the author's knowledge this is the first working internal-DMIC capture path
on macOS for this platform — and the first documentation anywhere of what
macOS actually requires before it will let Siri use a microphone (see
[Siri](#siri) below).

## Quick start

Assuming a working OpenCore hackintosh. Full walkthrough, including SIP
requirements and troubleshooting, in [`INSTALL.md`](INSTALL.md).

```sh
xcode-select --install        # once, if you don't have Command Line Tools
git clone https://github.com/shubhambanekar/LatSOFAudio.git
cd LatSOFAudio
# supply the firmware — see "Firmware" below for where to get it:
cp /path/to/sof-cml.ri kext/LatSOFAudio/Firmware/
cd kext && make               # produces LatSOFAudio.kext

sudo ditto LatSOFAudio.kext /Library/Extensions/LatSOFAudio.kext
sudo chown -R root:wheel /Library/Extensions/LatSOFAudio.kext
sudo chmod -R 755 /Library/Extensions/LatSOFAudio.kext
sudo kmutil load -p /Library/Extensions/LatSOFAudio.kext
```

That last command printing `Code=28 … requires a reboot` is **success**.

If it prints `Code=27 … not approved to load` instead, macOS is waiting on
you: System Settings → Privacy & Security, scroll to the **bottom**, click
**Allow**, enter your password, then run the same `sudo kmutil load -p …`
command again — it should now report `Code=28`. With `0x803`, the common
hackintosh `csr-active-config`, this prompt appears on the first install **and
again after every rebuild**, because the new binary has a new UUID. Using
`0xA03` instead (`030A0000` in the plist — see [`INSTALL.md`](INSTALL.md) §0)
adds the skip-approval bit and retires the prompt for good, at the cost of one
more SIP bit lowered.

Reboot, then pick **LatSOF DMIC capture** in System Settings → Sound → Input.

> ### The kext does *not* go in your EFI
>
> This is the one thing people get wrong, and it fails **silently** — no error
> anywhere, the kext simply never loads.
>
> The driver depends on Apple's `IOAudioFamily`, which lives in the *System*
> kernel collection. OpenCore injects into the *Boot* collection and can only
> link against that, so it drops the bundle without a word. Installing to
> `/Library/Extensions` lets macOS's own linker — which can see every
> collection — do the job. If you have an old `Kernel → Add` entry for
> `LatSOFAudio.kext`, disable it.
## Updating to a newer build

Don't copy a new kext over the old one — remove it first, and check the hash.
Run this from the `kext/` directory where `make` left the new bundle:

```sh
sudo rm -rf /Library/Extensions/LatSOFAudio.kext
sudo ditto LatSOFAudio.kext /Library/Extensions/LatSOFAudio.kext
md5 -q LatSOFAudio.kext/Contents/MacOS/LatSOFAudio                      # the new build
md5 -q /Library/Extensions/LatSOFAudio.kext/Contents/MacOS/LatSOFAudio  # what landed
sudo chown -R root:wheel /Library/Extensions/LatSOFAudio.kext
sudo chmod -R 755 /Library/Extensions/LatSOFAudio.kext
sudo kmutil load -p /Library/Extensions/LatSOFAudio.kext
```

The two hashes must match. That check is not paranoia: a `ditto` onto an
existing bundle has been observed here to report success and leave the **old**
binary in place, and nothing else catches it.

**Expect the approval prompt again.** The binary changed, so macOS sees a new
extension and `kmutil` answers `Code=27 … not approved to load` instead of the
`Code=28` that means success. Open System Settings → Privacy & Security, scroll
to the **bottom**, click **Allow**, then re-run the same `kmutil load -p`
command — it should give `Code=28`. This recurs on every build whose binary
differs; see [`INSTALL.md`](INSTALL.md) §4.

Quit anything that's playing audio before you load. Loading the kext while
AppleHDA is mid-stream can hijack that stream and produce loud static on the
speakers, curable only by physically replugging the jack.

Then reboot — `kmutil showloaded` reports the kext that is *running*, which is
the old one until you do; `kmutil inspect` is what shows you the bundle staged
on disk. Verify with the three checks in [`INSTALL.md`](INSTALL.md) §5. Keep the
previous bundle: rolling back is this same procedure with the old kext
(§9 removes the driver outright).

## Which name is which

| You will see | What it is |
|---|---|
| `LatSOFAudio.kext` | the driver — goes in **`/Library/Extensions`**, not your EFI |
| `LatSOFAudioDevice` | the hardware/DSP IOKit class — what you query with `ioreg` |
| `LatSOFKernelAudioDevice` | the audio device it publishes to CoreAudio |
| **LatSOF DMIC capture** | the input device you select in Sound settings |
| `LatSOFAudioPlugin.driver` | **retired.** The old userspace HAL plugin, replaced by the kernel audio engine. Source kept under `plugin/` for reference; do not install it alongside the kext |

## How it works (short version)

One kext, two halves:

1. **The DSP half** (`LatSOFAudioDevice`) boots SOF firmware on the DSP and
   runs the DMIC capture DMA. Crucially, it does **not** claim the PCI device:
   it matches `IOResources` and locates the HDA controller by registry walk, so
   **AppleHDA keeps working** — speakers, headphones and AppleALC are
   untouched. The two drivers share the controller by partitioning stream
   descriptors (AppleHDA keeps SD0; this driver uses capture SD1, tag 2), with
   one carefully-managed exception: during each firmware load the code loader
   briefly borrows AppleHDA's first output descriptor and hands it back
   byte-identical (see the FAQ, and "The borrowed-stream contract" in
   [`ARCHITECTURE.md`](ARCHITECTURE.md)).

2. **The audio half** (`LatSOFKernelAudioDevice` / `LatSOFKernelAudioEngine`)
   publishes that capture ring to CoreAudio as a kernel `IOAudioEngine` —
   zero-copy, with software gain and a DC-blocking high-pass, back-dated
   timestamps, and the `'imic'` data source that makes macOS treat it as a real
   built-in microphone.

Full details in [`ARCHITECTURE.md`](ARCHITECTURE.md).

For how it was actually built — including the wrong turns, the machine hang,
the signing misdiagnosis, the interrupt bug that presented as somebody else's
playback failing, and the night Siri finally worked — see
[`DEBUGGING-LOG.md`](DEBUGGING-LOG.md). If you are porting this, that file will
save you more time than this one.

## Siri

**Siri works on the internal microphone.** This took a kernel driver and a
disassembler to figure out, and the answer isn't documented anywhere else, so
here it is in full.

macOS decides whether a microphone is acceptable to Siri like this:

- If the device's **transport type** is `'usb '`, `'blue'` or `'line'`, it is
  accepted with no further questions. *This is the only reason USB microphones
  always work.*
- If the transport type is `'bltn'` (built-in), the device must **also**
  publish an input-scope **data source** of `'imic'` (internal microphone) —
  otherwise CoreSpeech nils the route and the panel says **"Siri Not Available
  — Connect a microphone."** (CoreSpeech itself would also accept `'emic'`,
  but AVFAudio's built-in-microphone lookup — the path behind voice
  activation — demands `'imic'` specifically, so publish `'imic'`.)
- Anything else is rejected outright.

The driver satisfies this by publishing an `IOAudioSelectorControl` whose
single selection is `'imic'` — the same mechanism AppleHDA uses to advertise
its speakers as `'ispk'`.

Two traps for anyone debugging this themselves:

- **The log line `AVVCAudioDeviceManager.mm:723 … supported : 0` is a red
  herring.** That function checks whether the device's ModelUID contains
  `05AC:1114` — the USB ID of the Apple Studio Display. Every microphone on
  earth returns 0, including the one in a real MacBook.
- **The property-read failures `doml` and `crss` in the log are the actual
  message.** Those fourccs are byte-reversed: `crss` is `ssrc`
  (`kAudioDevicePropertyDataSource`) and `doml` is `lmod`
  (`kAudioObjectPropertyModelName`). If you see those failing, you have found
  your bug.

An earlier version of this README asserted that Siri admits only
kernel-published devices and rejects userspace `AudioServerPlugIn` devices *by
class*. **That was wrong.** The gate is purely HAL properties, and is identical
for kexts, HAL plugins and DriverKit drivers — the old plugin here failed for
want of an `'imic'` data source, not for being userspace. The kernel port is
still the right destination (AMFI blocks the unsigned plugin on this machine,
and IOAudioFamily provides the selector, transport and terminal-type plumbing
for free), but the stated reason was wrong for weeks and is corrected here.

**Normal behavior that looks broken:** there is no listening tone when Siri
opens. macOS suppresses the beep on built-in microphones without hardware echo
cancellation so the mic can't hear itself. Real MacBooks do the same.

**If Siri answers on screen but doesn't speak:** System Settings → Apple
Intelligence & Siri → Siri Responses → turn on **Voice feedback**. It is off by
default on some installs.

**If Siri hears nothing on *any* microphone,** check DNS before blaming audio:
`dscacheutil -q host -a name guzzoni.apple.com`. A poisoned cache entry
pointing Siri's speech endpoint at `0.0.0.0` fails every request server-side.
Fix with `sudo dscacheutil -flushcache && sudo killall -HUP mDNSResponder`.

## Headphone crackle (not a microphone problem, but read this)

Static or crackling from the 3.5 mm jack on ALC laptops is almost always the
classic **44.1 kHz problem**: set the output to **48,000 Hz** in Audio MIDI
Setup.

**The trap:** changing the rate live is *not* a valid test. The codec path is
only fully reprogrammed when the audio engine is rebuilt, so you must **sleep
and wake the machine, unplug and replug the jack, or reboot** before judging
whether it worked. A live switch produces a false negative that cost this
project an evening.

**Booting with headphones already plugged in** is the case where the engine
can come up at 44.1 kHz and *stay* there — replugging alone just rebuilds at
44.1 again. The order is the whole trick: pin 48 kHz *first*, replug *after*.
Full procedure in [`INSTALL.md`](INSTALL.md) §7.

**Automating the pin is possible, but only one way.** Plugging the jack makes
AppleHDA rebuild the engine, and it passes *through* 44.1 kHz before settling
at 48 kHz by itself. A watcher that "corrects" that transient writes the rate
while the codec is still being programmed, leaving stream and codec
disagreeing — harsh static on headphones **and** speakers, curable only by
physically replugging. That was measured here, the hard way: the first
automation caused a worse fault than the one it fixed, and was retracted.

`latsof-setrate --enforce 48000` is the rewrite, and it is safe because it
**reacts to quiet rather than to change**: notifications only restart a timer,
and the rate is corrected solely after it has been wrong *continuously* for
eight seconds. A jack plug settles long before that and never triggers it; a
call app holding the output at 44.1 does, and gets corrected once. Both
behaviours were verified before it shipped. Run it from a LaunchAgent if you
want it permanent — the recipe is in [`INSTALL.md`](INSTALL.md) §7 — or keep
using the one-shot `latsof-setrate 48000` followed by a replug or sleep/wake.
Remember that pinning the rate alone never reprograms the codec path; only an
engine rebuild does.
`CodecCommander.kext` helps with jack pops but does **not** fix this; the
sample rate does.

**If it still crackles at 48 kHz, it's a different fault.** Constant static on
every sound is the rate problem above; *occasional* bursts that come and go are
CoreAudio missing render deadlines — usually caused by CPU throttling under
Low Power Mode or a low battery, not by audio configuration at all. Both the
log check and the fix are in [`INSTALL.md`](INSTALL.md) §7.

**Crackling in FaceTime and other call apps is a third fault, with an instant
cure.** This microphone is 48 kHz only, and some apps drag the *output* to
44.1 kHz when a call starts. CoreAudio then bridges two engines running at
different rates and its IO thread misses deadlines — measured at 746 overloads
in 60 seconds during a FaceTime call, all from FaceTime's own audio daemon on
an otherwise idle machine. Run `latsof-setrate 48000` **during the call**; the
overloads stop within the same second. This is the one case where a live rate
change is the fix rather than a false lead, because nothing depends on
reprogramming the codec — only on the two engines agreeing. Full details in
[`INSTALL.md`](INSTALL.md) §7.

## A note on naming

Everything in this tree carries the `LatSOF` prefix — bundle, identifiers,
classes, files. The parent project it derives from is DexterSLamb's
**CmlSOFAudio** (*Cml* for Comet Lake); that name appears in this repository
only when referring to the parent. Attribution lives in
[`CREDITS.md`](CREDITS.md), [`NOTICE`](NOTICE), and the plugin's source header.

## Requirements

- Comet Lake laptop with SOF-attached PDM DMICs (verify on Linux first —
  see [`PORTING.md`](PORTING.md); if your mic records under Linux with the
  `sof-cml` firmware, you have the right hardware)
- OpenCore with the usual unsigned-kext setup, and **SIP relaxed enough to
  load unsigned kexts** (`csr-active-config` with bit `0x1` set; `0x803` is
  the common hackintosh value and works)
- macOS Sequoia (15.x tested; other versions unverified — see
  [macOS Tahoe](#a-word-on-macos-tahoe-26) below)
- Command Line Tools (to build the kext)
- **The SOF firmware blob, which is not in this repo** — see "Firmware" below.
  The kext will not build without it.
- **No** `amfi=0x80`, **no** library-validation overrides — the driver runs
  under stock enforcement. In fact `amfi=0x80` will *break* microphone
  permission prompts system-wide; if something else in your EFI needs AMFI
  relaxed (OCLP-patched Wi-Fi is the usual culprit), use
  [AMFIPass](https://github.com/acidanthera/AMFIPass) instead.

## Reference hardware

Everything in this repository was developed and tested on exactly one machine.
If yours differs, `PORTING.md` §0 is the five-minute Linux check that matters
far more than matching this list.

| | |
|---|---|
| Model | Dell Latitude 3410 (SKU `09EC`), board `0MYG77` rev A00 |
| BIOS | 1.36.0, 07 Aug 2025 |
| CPU | Intel Core i5-10210U — Comet Lake-U, 4 cores / 8 threads, 1.60 GHz base |
| Host bridge | Comet Lake-U v1 4c `[8086:9b61]` |
| Graphics | Intel UHD Graphics, CometLake-U GT2 `[8086:9b41]` — **integrated only, no discrete GPU** |
| Memory | 2 × 8 GB DDR4-2400 (16 GB) |
| Audio controller | Comet Lake PCH-LP cAVS `[8086:02c8]`, subsystem `[1028:09ec]` |
| Codec | Realtek ALC3204 (ALC236 family, `[10ec:0236]`) at HDA address 0 |
| Microphones | 2 × PDM DMIC on one PDM controller, wired to the DSP — **not to the codec** |
| Storage | SanDisk WD PC SN5000S M.2 2280 NVMe `[15b7:5036]` |
| Ethernet | Realtek RTL8111/8168 `[10ec:8168]` |
| Wi-Fi / BT | Intel Wi-Fi 6 AX201 (CNVi) `[8086:02f0]`, subsystem `[8086:4070]` |
| macOS | Sequoia 15.7.7 (24G720) |
| SMBIOS | `MacBookPro16,2` |

The two rows that actually decide whether this project applies to you are
**Microphones** and **Codec**. On this board the ALC236 has no analog
microphone pin at all, so the only path to the internal mics is through the
DSP — which is the entire reason this driver exists. A machine with the same
CPU but mics wired to the codec does not need any of this.

## Firmware — you must supply this yourself

`sof-cml.ri` is **not distributed here**. The kext embeds it at build time, so
the build fails without it. Obtain it from the
[sof-bin](https://github.com/thesofproject/sof-bin) releases or from
`linux-firmware` (`intel/sof/sof-cml.ri`), check the licence that ships with
it, and place it at exactly:

    kext/LatSOFAudio/Firmware/sof-cml.ri

Use the **IPC3** generation. Newer IPC4 firmware speaks a different protocol
than this driver.

## Known limitations

- **Input only.** Playback stays with AppleHDA/AppleALC by design.
- **Input latency is reported as 240 frames (5 ms) and is provisional** — a
  best estimate of the pipeline delay, not a measurement. Echo cancellation in
  conferencing apps adapts to the real delay and works in practice; the
  reported number will be refined once measured against a trusted second input.
- Apple's "Ambient noise reduction" checkbox will not appear for this device —
  that processing lives inside Apple's own built-in-mic driver and cannot be
  requested by third-party devices. A DC-blocking high-pass is built in;
  app-level suppression (Zoom, Teams, etc.) works normally.
- **Voice Isolation / Wide Spectrum** mic modes are likewise Apple-internal.
- **Apple Intelligence** is hardware-gated (T2 / Apple silicon) and no driver
  can change that. Siri itself works.
- **You may see a second, dead "Internal Microphone"** in Sound settings —
  that's AppleHDA's codec path, which records silence on this hardware because
  no mic is wired to the codec. An output-only AppleALC layout removes it; see
  the next section.

## Side results that may be useful elsewhere

- **An output-only codec layout works on Sequoia** — believed to be a first: an
  AppleALC layout for the ALC236 with **zero input paths** (id 90 in this
  project's fork) boots and runs. The analog output engine publishes,
  speakers/headphones and jack switching work, and the phantom input devices
  are gone from every app's device picker. Sequoia's AppleHDA does not require
  an ADC path in an analog PathMap. Useful for any dead-codec DMIC board
  wanting an honest device list; a candidate for upstreaming to AppleALC.
- **The full `'imic'` recipe for Siri acceptance** (see [Siri](#siri)) applies
  to any third-party macOS audio driver — kext, HAL plugin or DriverKit —
  wanting to be treated as a built-in microphone.
- **Self-healing DSP recovery.** If a capture IPC times out (observed after
  some WebRTC sessions), the driver rebuilds the DSP automatically within a few
  seconds, bounded to three attempts so a genuinely dead firmware can't spin
  forever.

## A word on macOS Tahoe (26)

**This driver has not been tested on Tahoe, and Tahoe removes AppleHDA
entirely.** Since this project's design depends on AppleHDA being present —
both for playback and for the borrowed stream used during firmware load — you
should assume the whole stack needs AppleHDA restored (the community does this
by root-patching a KDK-extracted AppleHDA back onto the system volume) before
this driver has anything to coexist with. That patch is erased by every macOS
update and must be reapplied.

`IOAudioFamily` itself does still ship in Tahoe, so the driver is not
architecturally blocked — but treat Tahoe as an experiment on a spare volume,
not an upgrade. Apple is steering third-party audio toward AudioDriverKit, so a
DriverKit port is this project's likely long-term direction.

## FAQ

**Will this work on my laptop?** Only on Comet Lake machines with SOF-attached
DMICs — and you can find out in five minutes with a Linux live USB before
building anything: [`PORTING.md`](PORTING.md) §0.

**Do I need to change any code for my machine?** On a Dell Latitude 3410, no.
On any other board, every machine-specific value is catalogued in `PORTING.md`
§3a ("Board-specific values"); the DMIC topology messages must be adapted to
your own NHLT table, which is the real work of a port.

**Is there a prebuilt kext to download?** Not yet — building takes about two
minutes (Quick start above). A prebuilt would also embed the firmware blob, and
that redistribution decision is still open.

**Why doesn't it go in my EFI like every other kext?** Because it links
`IOAudioFamily`, which OpenCore can't resolve. See the box in Quick start.

**Do I need to disable SIP?** Partially — enough to load unsigned kexts
(`csr-active-config` bit `0x1`). Most hackintoshes already run `0x803`. Do
**not** add `amfi=0x80`; it breaks microphone permission prompts system-wide.

**Will this break my speakers?** Using it: no — coexisting with AppleHDA is the
entire design. The driver services no interrupts, and the one shared resource
it uses — AppleHDA's first output stream descriptor, which the DSP's code
loader runs over during every firmware load — is borrowed under a strict
snapshot/restore contract and verified byte-identical afterwards (the
`SD-Borrow`/`SD-Final` ioreg properties). Modifying the interrupt code or the
borrow/restore path can absolutely break playback: read "The
interrupt-starvation bug" and "The borrowed-stream contract" in
`ARCHITECTURE.md` first.

**Can I keep the old HAL plugin installed too?** No. Two publishers on one DMA
ring means either one's stop kills the other's capture. Remove the plugin —
`INSTALL.md` §9.

**Other macOS versions?** Sequoia 15.x is what's tested. Reports from other
versions are welcome.

## Safety notes

This is kernel code driving DMA engines on a shared PCI function. Understand
these before experimenting:

- **Never start capture during boot.** A capture DMA started while AppleHDA is
  still initialising the same controller hangs the machine. The driver only
  starts DMA on demand.
- **Don't hot-load the kext while audio is playing.** Every firmware load
  borrows AppleHDA's first output stream descriptor. The wake path checks the
  descriptor's RUN bit and defers while AppleHDA is streaming; the initial load
  path does not yet, so loading over live playback hijacks the running stream
  and produces immediate loud static, curable only by replugging the jack.
  Install or update, then **reboot**.
- Keep a recovery path for EFI edits (another OS that can mount the ESP, or a
  USB with a known-good EFI).
- Don't run standalone capture test tools while the audio device is in use —
  two masters on one ring, and a stop from either stops both.

## Credits and license

See [`CREDITS.md`](CREDITS.md) for full attribution.

- Fork of [DexterSLamb/CmlSOFAudio](https://github.com/DexterSLamb/CmlSOFAudio)
  (BSD-3-Clause), originally written for an HP Chromebook with the same DSP
  generation. The I2S codec path was removed; the DSP boot and DMIC pipeline
  were kept and re-based for AppleHDA coexistence.
- SOF firmware by the [Sound Open Firmware project](https://thesofproject.github.io/).
  The firmware binary (`sof-cml.ri`) is **not** included in this repo — obtain
  it from `sof-bin` releases or `linux-firmware` and check the licence file
  that ships with it (it is generally redistributable, but verify for
  yourself).
- The retired HAL plugin derives from Apple's `NullAudio` sample code; its
  original license header is retained.
- All modifications: BSD-3-Clause, same as the parent project.

Issues and portings to other CML machines welcome — start with
[`PORTING.md`](PORTING.md).
