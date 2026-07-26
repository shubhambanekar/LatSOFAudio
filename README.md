# LatSOFAudio

Internal microphone driver for the Intel SOF DSP on **Comet Lake** hackintosh
laptops, tested on the **Dell Latitude 3410** under **macOS Sequoia**.

Comet Lake laptops with PDM digital microphones (DMICs) have no working
internal mic under macOS: the mics are wired to the Intel Smart Sound DSP
(PCI `8086:02c8`), which Apple has no driver for, and on many boards
(including this one, ALC236) the codec has **no analog mic pin at all** —
so the headset jack cannot carry a mic either. This project boots the open
SOF firmware (Sound Open Firmware, Intel's open-source DSP firmware) on that DSP, builds a DMIC capture pipeline, and publishes the
result to macOS as a normal CoreAudio input device.

**Status: working.** 48 kHz / 2-channel stereo capture, QuickTime recording,
system-wide Dictation, live input metering, survives sleep/wake, runs under
**stock AMFI** with no security boot-args.

To the author's knowledge this is the first working internal-DMIC capture
path on macOS for this platform.

## Quick start

The complete path, assuming a working OpenCore hackintosh (details, the
exact `config.plist` entry, and troubleshooting in [`INSTALL.md`](INSTALL.md)):

```sh
xcode-select --install        # once, if you don't have Command Line Tools
git clone https://github.com/shubhambanekar/LatSOFAudio.git
cd LatSOFAudio
# supply the firmware — see "Firmware" below for where to get it:
cp /path/to/sof-cml.ri kext/LatSOFAudio/Firmware/
cd kext && make               # produces LatSOFAudio.kext
```

Copy `LatSOFAudio.kext` into `EFI/OC/Kexts`, add the `Kernel → Add` entry
shown in `INSTALL.md`, and reboot. Then:

```sh
cd plugin && make install     # builds, signs, verifies, restarts coreaudiod
```

Finally pick **LatSOF Internal Microphone** in System Settings → Sound →
Input, and test with a QuickTime audio recording.

## Which name is which

| You will see | What it is |
|---|---|
| `LatSOFAudio.kext` | the kernel driver — goes in `EFI/OC/Kexts` |
| `LatSOFAudioPlugin.driver` | the CoreAudio plugin — installed for you by `make install` |
| `LatSOFAudioDevice` | the kext's IOKit class — what you query with `ioreg` |
| **LatSOF Internal Microphone** | the input device you select in Sound settings |
| Internal Microphone (Built-in) | AppleHDA's dead codec path — *not* this driver; records silence |

## How it works (short version)

Two components:

1. **`LatSOFAudio.kext`** — boots SOF firmware on the DSP and runs the DMIC
   capture DMA. Crucially, it does **not** claim the PCI device: it matches
   `IOResources` and locates the HDA controller by registry walk, so
   **AppleHDA keeps working** — speakers, headphones and AppleALC are
   untouched. The two drivers share the controller by partitioning stream
   descriptors (AppleHDA keeps SD0; this driver uses capture SD1, tag 2).

2. **`LatSOFAudioPlugin.driver`** — a userspace CoreAudio HAL plugin
   (input-only) that maps the kext's capture ring and publishes
   "LatSOF Internal Microphone" as a system input device, with volume
   control, software gain and a DC-blocking high-pass.

Full details in [`ARCHITECTURE.md`](ARCHITECTURE.md).

For how it was actually built — including the wrong turns, the machine hang, the
signing misdiagnosis, and the interrupt bug that presented as somebody else's
playback failing — see [`DEBUGGING-LOG.md`](DEBUGGING-LOG.md). If you are
porting this, that file will save you more time than this one.

## A note on naming

Everything in this tree carries the `LatSOF` prefix — bundle, identifiers,
classes, files. The parent project it derives from is DexterSLamb's
**CmlSOFAudio** (*Cml* for Comet Lake); that name appears in this repository
only when referring to the parent. Attribution lives in
[`CREDITS.md`](CREDITS.md), [`NOTICE`](NOTICE), and the plugin's source
header. Use `LatSOFAudioDevice` when querying the driver with `ioreg`.

## Requirements

- Comet Lake laptop with SOF-attached PDM DMICs (verify on Linux first —
  see [`PORTING.md`](PORTING.md); if your mic records under Linux with the
  `sof-cml` firmware, you have the right hardware)
- OpenCore with the usual unsigned-kext setup (this is standard hackintosh
  territory; nothing beyond what your EFI already does)
- macOS Sequoia (15.x tested; other versions unverified)
- Command Line Tools (to build the kext and the HAL plugin)
- **The SOF firmware blob, which is not in this repo** — see "Firmware" below.
  The kext will not build without it.
- **No** `amfi=0x80`, **no** library-validation overrides — the driver runs
  under stock enforcement. In fact `amfi=0x80` will *break* microphone
  permission prompts system-wide; if something else in your EFI needs AMFI
  relaxed (OCLP-patched Wi-Fi is the usual culprit), use
  [AMFIPass](https://github.com/dortania/OpenCore-Legacy-Patcher) instead.

## Firmware — you must supply this yourself

`sof-cml.ri` is **not distributed here**. The kext embeds it at build time, so
the build fails without it. Obtain it from the
[sof-bin](https://github.com/thesofproject/sof-bin) releases or from
`linux-firmware` (`intel/sof/sof-cml.ri`), check the licence that ships with
it, and place it at exactly:

    kext/LatSOFAudio/Firmware/sof-cml.ri

Use the **IPC3** generation. Newer IPC4 firmware speaks a different protocol
than this driver.

## Install

See [`INSTALL.md`](INSTALL.md). Short form: kext into `EFI/OC/Kexts` plus a
`Kernel → Add` entry, then `make install` in `plugin/` — the Makefile
handles code-signing, permissions and verification, and refuses to restart
`coreaudiod` unless the installed bundle verifies.

## Known limitations

- **Input only.** Playback stays with AppleHDA/AppleALC by design.
- **Input latency is reported as 0 and has not been reliably measured.** Echo
  cancellation in conferencing apps adapts to the real delay and works fine in
  practice (verified on FaceTime calls), but the reported value is wrong. See
  the note in `ARCHITECTURE.md` on why measuring it needs a second, trusted
  input device.
- Apple's "Ambient noise reduction" checkbox will not appear for this
  device — that processing lives inside Apple's own built-in-mic driver
  and cannot be requested by third-party devices. A DC-blocking high-pass
  is built in; app-level suppression (Zoom, Teams, etc.) works normally.
- You will see **two** "Internal Microphone"-ish entries in Sound settings:
  the `Built-in` one is AppleHDA's dead codec path (it records silence on
  this hardware with or without this driver); select **LatSOF Internal
  Microphone**.
- "Hey Siri" and Apple Intelligence are hardware-gated by Apple (T2 /
  Apple silicon) and are not fixable by any driver.

## FAQ

**Will this work on my laptop?** Only on Comet Lake machines with
SOF-attached DMICs — and you can find out in five minutes with a Linux
live USB before building anything: [`PORTING.md`](PORTING.md) §0.

**Do I need to change any code for my machine?** On a Dell Latitude 3410,
no. On any other board, every machine-specific value is catalogued in
`PORTING.md` §3a ("Board-specific values"); the DMIC topology messages
must be adapted to your own NHLT table, which is the real work of a
port.

**Is there a prebuilt kext to download?** Not yet — building takes about
two minutes (Quick start above). A prebuilt would also embed the firmware
blob, and that redistribution decision is still open.

**Do I need to disable SIP or add security boot-args?** Nothing beyond
what your working OpenCore setup already does. Do **not** add
`amfi=0x80` — it breaks microphone permission prompts system-wide.

**Will this break my speakers?** Using it: no — coexisting with AppleHDA
is the entire design, and the driver enables no interrupts and never
touches AppleHDA's stream. Modifying the interrupt code can: see Safety
notes and `ARCHITECTURE.md` first.

**Other macOS versions?** Sequoia 15.x is what's tested. Reports from
other versions are welcome.

## Safety notes

This is kernel code driving DMA engines on a shared PCI function.
Understand these before experimenting:

- **Never start capture during boot.** A capture DMA started while
  AppleHDA is still initialising the same controller hangs the machine.
  The driver only starts DMA on demand from userspace.
- Keep a recovery path for EFI edits (another OS that can mount the ESP,
  or a USB with a known-good EFI).
- Don't run standalone capture test tools while the HAL device is in use —
  two masters on one ring, and a stop from either stops both.

## Credits and license

See [`CREDITS.md`](CREDITS.md) for full attribution.


- Fork of [DexterSLamb/CmlSOFAudio](https://github.com/DexterSLamb/CmlSOFAudio)
  (BSD-3-Clause), originally written for an HP Chromebook with the same
  DSP generation. The I2S codec path was removed; the DSP boot and DMIC
  pipeline were kept and re-based for AppleHDA coexistence.
- SOF firmware by the [Sound Open Firmware project](https://thesofproject.github.io/).
  The firmware binary (`sof-cml.ri`) is **not** included in this repo —
  obtain it from `sof-bin` releases or `linux-firmware` and check the
  licence file that ships with it (it is generally redistributable, but
  verify for yourself).
- The HAL plugin derives from Apple's `NullAudio` sample code; its
  original license header is retained.
- All modifications: BSD-3-Clause, same as the parent project.

Issues and portings to other CML machines welcome — start with
[`PORTING.md`](PORTING.md).
