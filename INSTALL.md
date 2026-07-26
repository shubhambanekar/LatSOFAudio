# Installing LatSOFAudio

A complete walkthrough, assuming nothing beyond a working OpenCore
hackintosh. Every step shows the command to run and what its output should
look like. Tested on a Dell Latitude 3410 under macOS Sequoia 15.x.

Time: about 15 minutes, plus one reboot.

## 0. What you need

- A Comet Lake laptop whose DMICs work under Linux with the `sof-cml`
  firmware — verify this first, see [`PORTING.md`](PORTING.md). If Linux
  can't record, macOS won't either, and you'll save yourself days.
- A working OpenCore install. If your EFI already loads Lilu and
  VirtualSMC — it does, or you wouldn't be booted — it will load this kext
  the same way. Nothing extra is needed.
- Command Line Tools:

  ```sh
  xcode-select --install
  ```

- A way to recover if an EFI edit goes wrong: another OS that can mount
  the EFI partition, or a USB stick with a known-good EFI. You will
  probably never need it. Have it anyway.
- Back up your EFI partition before you start.

## 1. Get the source

```sh
git clone https://github.com/shubhambanekar/LatSOFAudio.git
cd LatSOFAudio
```

## 2. Get the firmware — required, the build fails without it

`sof-cml.ri` is not distributed in this repo. In order of preference:

1. **From the Linux install you verified with** — this is the best source,
   because it's the exact binary you already proved works on your board:
   `/lib/firmware/intel/sof/sof-cml.ri` (if it's a symlink, copy the file
   it points to). Copy it out via a USB stick.
2. From a **[sof-bin](https://github.com/thesofproject/sof-bin/releases)**
   release in the **v2.2.x** series — inside the archive the file is at
   `sof-v2.2.x/sof-cml.ri`.
3. From the `linux-firmware` repository: `intel/sof/sof-cml.ri`.

Use the **IPC3 generation (the v2.2.x line)**. Newer IPC4 firmware speaks a
different protocol and will not work with this driver.

Place it at exactly this path inside the repo, then sanity-check the size:

```sh
cp /path/to/sof-cml.ri kext/LatSOFAudio/Firmware/
ls -l kext/LatSOFAudio/Firmware/sof-cml.ri
```

Expect roughly **500–600 KB**. A tiny file means you copied a symlink.

## 3. Build the kext

```sh
cd kext
make
```

The last line should be:

```
=== Build complete: LatSOFAudio.kext ===
```

If instead you see `No rule to make target ... sof-cml.ri`, the firmware
isn't at the path from step 2.

Building on the target machine with Command Line Tools is the tested path;
the Makefile can also cross-compile from an Apple silicon Mac.

## 4. Copy the kext into your EFI

Find and mount the EFI partition (type `EFI`, usually ~200 MB, usually
`disk0s1`):

```sh
diskutil list
sudo diskutil mount disk0s1
```

Note the volume name the mount prints — commonly `EFI` or `NO NAME` — and
use it below:

```sh
sudo ditto LatSOFAudio.kext "/Volumes/<your ESP>/EFI/OC/Kexts/LatSOFAudio.kext"
```

## 5. Tell OpenCore about it

Open `EFI/OC/config.plist` in your usual editor
([ProperTree](https://github.com/corpnewt/ProperTree) if you don't have
one). Under **Kernel → Add**, add this entry anywhere **after Lilu** —
last is fine:

```xml
<dict>
	<key>Arch</key>
	<string>Any</string>
	<key>BundlePath</key>
	<string>LatSOFAudio.kext</string>
	<key>Comment</key>
	<string>SOF DMIC capture</string>
	<key>Enabled</key>
	<true/>
	<key>ExecutablePath</key>
	<string>Contents/MacOS/LatSOFAudio</string>
	<key>MaxKernel</key>
	<string></string>
	<key>MinKernel</key>
	<string></string>
	<key>PlistPath</key>
	<string>Contents/Info.plist</string>
</dict>
```

What the fields mean: `BundlePath` is the folder name you copied into
`Kexts`; `ExecutablePath` is the binary inside it; `PlistPath` is its
`Info.plist`. All three must match exactly or the kext silently fails to
load. If you manage your config with OCAuxiliaryTools, use its kext-sync
instead and check the generated entry against the values above.

Save, and reboot.

## 6. Verify the kext loaded

```sh
kmutil showloaded --list-only | grep -i sof
```

Expect a line containing:

```
com.hackintosh.LatSOFAudio (1.1.5)
```

If it's missing, see Troubleshooting below — it's almost always one of the
three paths in step 5.

## 7. Build and install the CoreAudio plugin

```sh
cd plugin
make install
```

It will ask for your password (it installs into `/Library/Audio/Plug-Ins/HAL`
and restarts the audio server). The output must include:

```
=== build seal OK ===
SEAL-OK
device published
```

The Makefile handles permissions, extended attributes, code-signing, and
verification, and refuses to restart `coreaudiod` unless the installed
bundle verifies — the ordering matters, so use `make install` rather than
copying by hand.

## 8. Select the microphone and test

System Settings → Sound → Input → **LatSOF Internal Microphone**.

You will see two similar entries; the plain "Internal Microphone
(Built-in)" is AppleHDA's dead codec path and records silence on this
hardware — see "Which name is which" in the README. Pick the **LatSOF**
one, watch the level meter move when you speak, and confirm with a short
QuickTime audio recording.

Done. Sleep/wake, Dictation, and FaceTime all work from here.

## Troubleshooting

**Build fails with `No rule to make target ... sof-cml.ri`** — the
firmware is missing or misplaced. Step 2; the path must be exactly
`kext/LatSOFAudio/Firmware/sof-cml.ri`.

**Kext absent from `kmutil showloaded`** — in the config entry, check
character-for-character: `BundlePath` = `LatSOFAudio.kext`,
`ExecutablePath` = `Contents/MacOS/LatSOFAudio`, `PlistPath` =
`Contents/Info.plist`; confirm the bundle really is inside `EFI/OC/Kexts`;
OCAuxiliaryTools users, re-run kext sync. A mismatched path fails
*silently* — no error anywhere.

**Kext loaded but no device in Sound settings** — check the driver
matched and the plugin installed:

```sh
ioreg -rc LatSOFAudioDevice -d 1 -w0 | head -3
cd plugin && make status
```

`make status` should end with `SEAL-OK` and `device published`.

**Plugin refuses to load with a code-signature error (Security -67030)** —
this is a *file permissions* fault masquerading as a signing-policy
rejection: the audio server's unprivileged user can't read a file inside
the bundle (typically an `Info.plist` that came through a browser download
with a restrictive mode). `make install` prevents it by construction; if
you installed by hand, don't — run `make install`.

**Microphone permission prompts never appear / Privacy → Microphone list
is empty** — you have `amfi=0x80` in your boot-args. Remove it; it breaks
TCC prompts system-wide. If something else in your EFI needed AMFI relaxed
(OCLP-patched Wi-Fi is the usual culprit), use AMFIPass instead of the
boot-arg.

**Device present but recordings are silent** — you selected the wrong of
the two microphone entries (step 8), or your firmware is not the IPC3
`sof-cml` build (step 2), or your board isn't Comet Lake with
SOF-attached DMICs (step 0 / `PORTING.md`).

**Speakers or playback misbehave after experimenting with the source** —
read "The interrupt-starvation bug" in
[`ARCHITECTURE.md`](ARCHITECTURE.md) before changing anything about
interrupts; the stock driver enables none, deliberately.
