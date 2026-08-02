# Porting to other machines

The kext is written for the Dell Latitude 3410 but the architecture is
generic Comet Lake. If your laptop has SOF-attached DMICs, most of the
work is configuration, not code. Work through this in order — each step
is a gate for the next.

## 0. Will this work on my machine? (five minutes, no building)

Boot any Linux live USB (Ubuntu is fine) on the target laptop and run:

```sh
lspci -nn | grep -i audio
```

You are looking for Intel Smart Sound at PCI ID **`8086:02c8`**
(Comet Lake-LP). A different ID means different silicon and a much bigger
port than this guide covers.

```sh
arecord -l
```

You want an `sof-hda-dsp` card whose device list includes a **DMIC**
capture device. Then record five seconds from it and play it back —
card/device numbers come from the `arecord -l` output (on the Latitude
3410 the DMIC PCM is device 6):

```sh
arecord -D plughw:0,6 -f S16_LE -r 48000 -c 2 -d 5 test.wav
aplay test.wav
```

If you hear yourself, your hardware qualifies and everything below is
worth your time. If Linux cannot record, macOS never will — stop here.

While you're booted in Linux, save the files §1 asks for; you will not
want to reboot back for them.

## 1. Prove the hardware under Linux first

Boot any recent live Linux and check:

    lspci -nn | grep -i audio          # want 8086:02c8 (CML-LP cAVS)
    dmesg | grep -i sof                # SOF should bind and load sof-cml.ri
    arecord -l                         # find the DMIC capture device
    arecord -D hw:X,Y -f S32_LE -r 48000 -c 2 test.wav   # speak, play back

If Linux cannot record from the DMICs, stop — no amount of macOS work
will fix hardware or wiring. While you're there, archive:

- `/sys/firmware/acpi/tables/NHLT` (your DMIC configuration lives here)
- the exact firmware file SOF loaded (`/lib/firmware/intel/sof/sof-cml.ri`)
- `amixer` state and the capture device number, for reference

## 2. Read-only feasibility on macOS

Before running any code that writes to the DSP, verify coexistence
passively (the project did this with a throwaway kext; the checks matter
more than the vehicle):

- Map BAR4 and read `ADSPCS`. You want `0x00000f0f` — all cores in reset,
  DSP dormant, proving AppleHDA is not using it. (Reads of all-ones early
  in boot are a timing artifact: memory decode may not be enabled yet.)
- Read `GCAP` for the input-stream count, and note which descriptors
  AppleHDA is using (SD0 on this board). Pick a free input descriptor and
  a stream tag no one uses; this port uses SD6 / tag 7 — the HIGHEST input
  descriptor, and that choice is a lesson. It originally used SD1 on the
  theory "AppleHDA keeps SD0, so SD1 is free". That held only while the codec
  layout published a single AppleHDA input engine: layouts that publish more
  will run REAL DMA streams for every input device a user can select — a dead
  codec pin does not mean a dead engine — and AppleHDA allocates those
  streams from the bottom. The second engine landed on SD1, over the capture
  ring, and wedged the mic (patch-32). Park capture as high as GCAP allows.

## 3. Board configuration

Everything board-specific sits in a small cluster of constants:

- **DAI config from your NHLT**, not this repo's: `num_pdm_active`, which
  PDM controllers carry mics, sample rate, container width. A 4-mic board
  will need the 4-channel stream format (`0x0043`) and a matching change to
  `kEngineChannels` plus the channel handling in
  `LatSOFKernelAudioEngine::convertInputSamples`
  (`kext/LatSOFAudio/LatSOFKernelAudio.cpp`).
- **Stream format**: `0x0041` = 48 kHz / 2 ch / 32-bit container. Derive
  yours from the HDA SDxFMT encoding if your NHLT differs.
- **PDM clock range**: this port narrows `pdmclk_min` to 2.4 MHz (from
  the canonical 500 kHz). If your DMA advances but every sample is zero,
  widen it back to 500000 — canonical CML 2-mic values are
  pdmclk 500 kHz–4.8 MHz, 40/60 duty, 32-bit FIFO at 48 kHz.
- **Firmware**: stay on `sof-cml.ri` (IPC3). Newer IPC4 firmware speaks a
  different protocol than this driver.

## 3a. Board-specific values — the complete map

Everything in the code that belongs to the Dell Latitude 3410 rather than
to the design. If you port, this is the checklist; search for the symbol
named.

| Value | Where | What it means | Getting yours | 3410 value |
|---|---|---|---|---|
| Capture channels | `kLatSOF_CapChannels` in `kext/LatSOFAudio/LatSOFAudioDevice.hpp` | how many DMIC channels the pipeline captures | the channel count that worked in your §0 Linux test / your NHLT | 2 |
| PDM clock window | the `pdmclk` fields of the DAI_CONFIG IPC — search `pdmclk` in `LatSOFAudioDevice.cpp` | allowed DMIC clock range the firmware may pick from | start with the canonical CML window 500000–4800000; narrow only if needed | min narrowed to 2400000 |
| Stream engine + tag | `capIdx` / `capTag` in `LatSOFAudioDevice.cpp` | which HDA input DMA engine and stream tag this driver uses — must not collide with AppleHDA | use the HIGHEST input descriptor GCAP advertises: AppleHDA allocates input streams from the bottom, one per input engine the codec layout publishes, and dead pins still get real DMA engines. SD1 "looked safe" here until a second input engine trampled it (patch-32) | SD6, tag 7 |
| Loader stream + tag | `sIdx` / `sTag` in `initDSP()` | which **output** engine carries firmware loads. `SD(numISS)` is AppleHDA's first output engine, and the ROM binds the code-load gateway by *tag* — this is a **borrow**, not a free descriptor, governed by the contract in §5 | derived from `GCAP` (`sIdx = numISS`); do not relocate without moving the tag too | SD7, tag 1 |
| DMIC topology payloads | `kext/LatSOFAudio/tplg_ipc_data.h` | the captured IPC topology stream — recorded by the parent project from a working Linux session on the donor C1030 — with the DMIC/DAI messages adapted to this board's NHLT | adapt the DMIC/DAI messages to **your** NHLT (`sudo cat /sys/firmware/acpi/tables/NHLT > nhlt.bin` under Linux), or capture your own stream the same kprobe way — either path is the substantive porting work, see §3 | this board's NHLT |
| Device names | `setDeviceName` / `setDeviceShortName` / `setManufacturerName` / `setDeviceModelName` calls in `LatSOFKernelAudioDevice::initHardware`, and `setDescription` in `LatSOFKernelAudioEngine::initHardware` (`kext/LatSOFAudio/LatSOFKernelAudio.cpp`) | the `initHardware` calls set the device identity seen in `ioreg` (IOAudioDeviceName) and the manufacturer/model strings; **the name Sound settings displays is the engine's `setDescription`** ("LatSOF DMIC capture") | cosmetic — change freely. Do **not** touch the transport type or the `'imic'` input selector next to them: those are what make Siri accept the device | ioreg: "LatSOF Internal Microphone"; Sound settings: "LatSOF DMIC capture" |
| Controller identity | located by registry walk on the HDA controller at `00:1f.3` | where BAR4 and the DSP live | your §0 `lspci` check — must be `8086:02c8` | `8086:02c8` |

Everything not in this table is design, not configuration — if you find
yourself changing it, read `ARCHITECTURE.md` first.

## 3b. Do not enable stream interrupts

Read this before you copy the stream-start sequence from the reference driver,
because it will look correct and will cost you an evening.

The reference sets `INTCTL |= GIE | CIE | (1 << streamIndex)` and
`SDCTL |= IOCE | FEIE | DEIE`. Do neither. A driver that shares the PCI
function with AppleHDA and polls DPIB must raise no interrupts at all: set
`RUN` alone, leave the enables masked, clear our `SIE` bit rather than setting
it, and clear `SDSTS` (write-1-to-clear) at both start and stop.

If you get this wrong, **capture works fine and playback breaks.** See the
symptom list in §4.

## 4. Order of debugging when capture is silent

1. Start/stop IPCs return 0 but position never moves → the decouple did
   not hit *your* descriptor (check `PPCTL`), or the descriptor is
   contended.
2. Position advances, samples all zero → BIOS mic switch; then PDM clock
   range (§3); then your NHLT-derived DAI config.
3. IPC errors: `-EINVAL` = topology/params mismatch (usually format or
   channel count), `-ENOMEM` = shrink period/buffer sizes, `-EBUSY` =
   stream-tag collision with AppleHDA.
4. Speakers die during experiments → you left something unacknowledged on the
   shared interrupt line. Reboot, then read the IPC section *and* "The
   interrupt-starvation bug" in `ARCHITECTURE.md`.
5. **Playback dies a few seconds into a long capture**, the volume keys stop
   responding, and everything recovers when capture stops → stream-descriptor
   interrupts are enabled and unserviced. See §3b. This one presents as an
   *output* fault: `coreaudiod` logs `HALS_IOA1Engine::EndWriting` with
   `0xE00002EE` against AppleHDA's output engine and burns 20%+ CPU, while your
   capture keeps working perfectly. Profile `coreaudiod` before assuming the
   CPU is yours — `sudo sample coreaudiod 10`, then grep the output for your
   driver's symbols. If none appear, the CPU is not yours and neither is the
   bug you are looking for.
6. Echo cancellation misbehaving in calls → check what you report for
   `kAudioDevicePropertyLatency`. Do not measure it using your own device's
   timestamps; see `ARCHITECTURE.md`.
7. **Cold boot works perfectly; audio breaks only after sleep** — and which
   half breaks (mic, speakers, both) varies between wakes. This is the
   hardest symptom on this list to attribute, and it cost a full day here:
   it is the borrowed loader descriptor being written *after* the
   hand-back. At boot that descriptor is empty and AppleHDA programs it
   after you, so every violation is free; after a wake it is fully live,
   and anything you write destroys a running playback engine. Diff
   `SD-Borrow` against `SD-Final` in ioreg — if they differ, the differing
   field names the write. If they match and audio still broke, distrust
   any success telemetry emitted before the function's *last* write to the
   descriptor; ours lied for a week that way. Treat mic-death and
   speaker-death as two bugs, not one. Full story: Phase 13 in
   `DEBUGGING-LOG.md`; the rule that ends it: "The borrowed-stream
   contract" in `ARCHITECTURE.md`.

## 5. Rules that keep the machine bootable

- Never start capture DMA at boot. Only from userspace, machine idle.
- Never touch `GCTL` or any global controller state.
- Never write `PPCTL` bit 0, and never touch AppleHDA's descriptors in
  steady state. There is exactly **one** sanctioned exception, and your port
  cannot avoid it: the DSP's code loader must run over an output stream
  descriptor, and the ROM binds its code-load gateway by *stream tag* — on
  this hardware that meant borrowing AppleHDA's first output engine,
  `SD(numISS)`, for the duration of every firmware load. If you borrow, you
  are bound by the borrowed-stream contract: snapshot the descriptor (and
  SPIB/PPCTL) once, restore through one idempotent function called from
  **every** exit path including the timeouts, and never write the
  descriptor again after the hand-back. Publish before/after telemetry
  read *after* your last write (`SD-Borrow`/`SD-Final` here) and require
  them identical. Read "The borrowed-stream contract" in `ARCHITECTURE.md`
  before writing a single line of loader code — violating it produces
  symptom 7 in §4, the one that only appears after sleep.
- Keep every wait bounded. An unbounded poll in an IOKit workloop is a
  hang, not a bug report.
- Keep an EFI recovery path you have actually tested.

## 6. Kernel audio engine

The device macOS sees is published by the kext itself
(`kext/LatSOFAudio/LatSOFKernelAudio.cpp` — the old userspace HAL plugin
under `plugin/` is retired; don't install both). A port usually needs only
cosmetics here: the names in `LatSOFKernelAudioDevice::initHardware`, and —
if your channel count differs — `kEngineChannels` and the conversion loop
in `convertInputSamples`. The default gain (+30 dB) suits this board's DMIC
sensitivity; check your own raw levels before copying it.

Three things in this file are load-bearing, not cosmetic. The built-in
transport type plus the `'imic'` input selector are what make Siri accept
the device (see the Siri section in the README). The back-dated timestamps
in `stampBackdated` are what keep the CoreAudio clock honest. And the
install path is `/Library/Extensions` + `sudo kmutil load -p`, **not** your
EFI — the kext links `IOAudioFamily`, which OpenCore cannot resolve, and an
EFI entry fails silently (INSTALL.md §6).

If you port successfully, please open an issue with your board name, your
NHLT-derived DAI values, and which descriptor/tag you used — that is
exactly the information the next person needs.
