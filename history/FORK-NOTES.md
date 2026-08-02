# LatSOFAudio — Latitude 3410 fork

Goal of this stage: **boot the DSP firmware and reach FW_READY.** Nothing else.

## Applied automatically

- matching changed to `IOResources` + registry walk (coexists with AppleHDA)
- `pciDevice->open()` removed
- D3→D0 power cycle removed
- `initI2C()` / `initRT5682()` stubbed to return true

## STILL TO DO BY HAND (the important ones)

1. **Remove the GCTL controller reset** in `initDSP()`.
   Search for `GCTL` near the "HDA CONTROLLER INIT" comment. The reference
   resets the whole HD Audio controller; AppleHDA has already initialised it
   and a reset would disrupt playback. This is the main open question of
   this milestone — try without it first.

2. **Stream indices.** The reference picks its own; we know SD0 is taken by
   AppleHDA and SD1–SD6 are free. Use **SD1** for the code loader.

3. **Stop after FW_READY.** In `initDSP()`, find the
   `==================== PIPELINE 1: SSP0 Headphone ====================`
   comment and `return true;` immediately before it. Pipelines come later.

4. **Capture channels** — `kCmlSOF_CapChannels` 4 → 2 (your DMIC is stereo).

## Build & test

```sh
make
sudo diskutil mount disk0s1
cp -R LatSOFAudio.kext "/Volumes/NO NAME/EFI/OC/Kexts/"
# add LatSOFAudio.kext under Kernel -> Add, reboot
sudo dmesg | grep -iE "CmlSOF|LatSOF"
```

Success looks like `FW_ENTERED` / firmware version in the log, and audio
still working.
