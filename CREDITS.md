# Credits

Formal third-party licence notices live in [`NOTICE`](NOTICE); this file is the
narrative version and the record of what is original here.

This project is a derivative work. Almost none of the hard architectural
groundwork is original to it, and the parts that are original are narrow. This
file records who did what.

## DexterSLamb — CmlSOFAudio

<https://github.com/DexterSLamb/CmlSOFAudio> — BSD-3-Clause.

The direct parent of this project, written for an HP Chromebook with the same
Comet Lake SOF DSP. It established essentially everything hard: the IOKit
driver skeleton, the SOF firmware loading sequence over the code-loader DMA
path, the IPC3 mailbox transport, stream descriptor programming, and the
CoreAudio HAL plugin structure. Without it this project would not exist, and
would not have been attempted.

This fork removed the I2S codec path and reworked the driver to coexist with
AppleHDA rather than own the device. The DSP bring-up is theirs.

## Sound Open Firmware project

<https://thesofproject.github.io/> — firmware under its own licence.

The DSP firmware (`sof-cml.ri`) and, just as importantly, the Linux SOF driver
whose source served as the reference for correct register sequences,
`no_stream_position` handling, DPIB as the position source on Comet Lake, and
the DAI configuration derived from NHLT data.

The firmware binary is **not redistributed here** — see the README for where to
obtain it and where to place it.

One upstream Linux commit worth citing here: `a792bfc1c2bc`, which
established that the couple/FMT/decouple sequence this driver mirrors is
pre-5.17 behaviour and unnecessary on Comet Lake. It was kept anyway,
as harmless and locally proven.

## Apple — NullAudio sample code

The CoreAudio HAL plugin derives from Apple's `NullAudio` AudioServerPlugIn
sample. Its original licence header is retained in
`plugin/LatSOFAudioPlugin.c`.

Two of the bugs documented in `ARCHITECTURE.md` come directly from NullAudio
assumptions that do not hold for real hardware — the manufactured host-clock
timeline, and `kAudioDevicePropertyLatency` returning 0 with a comment
explaining that the device "always vends silence". If you derive from
NullAudio, audit every property that describes the hardware.

## Acidanthera

Lilu, AppleALC, and the `alc-verb` tool — used for this machine's analog audio
and for the codec-level diagnosis documented in the project notes. AMFIPass
(`dhinakg`, distributed with OpenCore Legacy Patcher) is what allows this
machine to run with stock AMFI enforcement instead of `amfi=0x80`, which
matters because that boot argument breaks microphone permission prompts
system-wide.

## What is original here

For honesty's sake, the contributions specific to this project are:

- A coexistence architecture: matching `IOResources` and locating the HDA
  controller by registry walk rather than claiming the PCI device, so AppleHDA
  keeps playback on the same PCI function. Includes the stream-descriptor
  partitioning, per-stream decoupling, and the discovery that the reference's
  global `GCTL` reset is unnecessary.
- The requirement that a coexisting driver raise **no** interrupts at any
  level, and the diagnosis of what happens when it does — a failure that
  presents as someone else's playback breaking. Documented in
  `ARCHITECTURE.md` and `PORTING.md`.
- Board-specific configuration for the Dell Latitude 3410 derived from its own
  NHLT data, and an input-only HAL plugin with software gain and DC blocking.
- The install procedure in `INSTALL.md`, including the `-67030` diagnosis,
  which is a file-permission fault that masquerades as a code-signing policy
  rejection.

## Licence

BSD-3-Clause, matching the parent project. See `LICENSE`.
