# Installing LatSOFAudio

A complete walkthrough that assumes nothing except a hackintosh that already
boots macOS with OpenCore. Every step shows the command and what its output
should look like. Tested on a Dell Latitude 3410 under macOS Sequoia 15.7.7.

**Time:** about 20 minutes and two reboots.

**What you get when you finish:** a working internal microphone — Dictation,
QuickTime, FaceTime, Zoom/Teams/Meet, and **Siri**, on the laptop's own mics
with nothing plugged in.

> **Read this before you start.** As of July 2026 this kext installs to
> `/Library/Extensions`, **not** into your EFI. If you followed an older
> version of these instructions and put `LatSOFAudio.kext` in `EFI/OC/Kexts`,
> it silently never loaded. Section 6 explains why. If you are upgrading from
> the old HAL-plugin version, do section 9 (uninstall) first.

## Contents

0. [What you need](#0-what-you-need)
1. [Get the source](#1-get-the-source)
2. [Get the firmware](#2-get-the-firmware--required)
3. [Build the kext](#3-build-the-kext)
4. [Install it](#4-install-the-kext)
5. [Reboot and verify](#5-reboot-and-verify)
6. [Why not the EFI?](#6-why-not-the-efi)
7. [Fix headphone crackle](#7-fix-headphone-crackle-optional-but-recommended)
8. [Turn on Siri](#8-siri)
9. [Updating, rolling back, uninstalling](#9-updating-rolling-back-uninstalling)
10. [Troubleshooting](#10-troubleshooting)

## 0. What you need

- **The right hardware.** A Comet Lake laptop whose internal mics work under
  Linux with the `sof-cml` firmware. Verify this first with a Linux live USB —
  see [`PORTING.md`](PORTING.md) §0. It takes five minutes and saves days.
- **A working OpenCore install.** If your EFI already loads Lilu and
  VirtualSMC, you have everything you need.
- **SIP partially disabled** so macOS will load an unsigned kext. Check:

  ```sh
  nvram -p | grep csr-active-config
  ```

  On a typical hackintosh the output looks exactly like this (there is a tab
  between the name and the value):

  ```
  csr-active-config	%03%08%00%00
  ```

  Each `%XX` is one byte, lowest byte first — so `%03%08%00%00` is the value
  `0x803`, which allows unsigned kexts and is fine. Any value whose **first**
  byte is odd (`%01`, `%03`, `%67`, …) works, because that's the
  allow-unsigned-kexts bit.

  If the command prints nothing, or the first byte is even (e.g.
  `%00%00%00%00`), set `csr-active-config` to `03080000` in your
  `config.plist` under `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82`.
  Two traps: it is a **Data** entry, not a String — in ProperTree paste
  `03080000` as hex — and OpenCore only applies the `Add` value if the
  variable isn't already set, so also list `csr-active-config` under
  `NVRAM → Delete` for the same GUID (or reset NVRAM from the OpenCore boot
  menu). Reboot and re-run the check before continuing.

- **Command Line Tools:**

  ```sh
  xcode-select --install
  ```

- **A recovery path.** Another OS that can mount the EFI partition, or a USB
  stick with a known-good EFI. You probably won't need it. Have it anyway.
- **Back up your EFI partition before you start.**

## 1. Get the source

```sh
git clone https://github.com/shubhambanekar/LatSOFAudio.git
cd LatSOFAudio
```

## 2. Get the firmware — required

`sof-cml.ri` is **not** distributed in this repo, and the build fails without
it. In order of preference:

1. **From the Linux install you verified with** — the exact binary you already
   proved works on your board: `/lib/firmware/intel/sof/sof-cml.ri`. If it's a
   symlink, copy the file it points to. **Copy it while you're still booted in
   Linux** — onto the live USB itself, a second FAT-formatted stick, or the
   EFI partition — because macOS cannot read the Linux filesystem afterwards.
   Some distros ship the file compressed as `sof-cml.ri.zst` or `.ri.xz`;
   decompress first (`zstd -d sof-cml.ri.zst` or `unxz sof-cml.ri.xz`) — the
   kext needs the raw `.ri`.
2. From a [sof-bin](https://github.com/thesofproject/sof-bin/releases) release
   in the **v2.2.x** series: `sof-v2.2.x/sof-cml.ri`.
3. From `linux-firmware`: `intel/sof/sof-cml.ri`.

Use the **IPC3 generation (the v2.2.x line)**. Newer IPC4 firmware speaks a
different protocol and will not work.

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

If you see `No rule to make target ... sof-cml.ri`, the firmware isn't at the
path from step 2.

## 4. Install the kext

This copies the kext where macOS itself can load it, sets the ownership macOS
requires, and asks the system to link it. **All four commands matter, in this
order.** The `chown` is not cosmetic: `ditto` reproduces the source bundle's
ownership, so even under `sudo` the copy arrives owned by your account
(`yourname:staff`) — and `kmutil` refuses to load a kext in
`/Library/Extensions` that isn't `root:wheel`.

> **Already have a copy in `/Library/Extensions`? Do not run this block.** Use
> §9's update block instead — it removes the old bundle first and proves the
> copy with `md5`. A `ditto` onto an existing bundle has been observed here to
> report success and leave the **old** binary in place, which cost a debugging
> session on 29 July 2026. Once you have installed once, every later install is
> an update.

Run this from the `kext/` directory where `make` left `LatSOFAudio.kext` — if
you opened a new terminal since step 3, `cd` back first (e.g.
`cd ~/LatSOFAudio/kext`):

```sh
sudo ditto LatSOFAudio.kext /Library/Extensions/LatSOFAudio.kext
sudo chown -R root:wheel /Library/Extensions/LatSOFAudio.kext
sudo chmod -R 755 /Library/Extensions/LatSOFAudio.kext
sudo kmutil load -p /Library/Extensions/LatSOFAudio.kext
```

**The expected result of the last command looks like an error and is not:**

```
Error Domain=KMErrorDomain Code=28 "Loading extension(s):
com.hackintosh.LatSOFAudio requires a reboot"
```

That message means macOS accepted the kext and staged it for the next boot.
Confirm it landed:

```sh
kmutil inspect | grep -i latsof
```

You should see `com.hackintosh.LatSOFAudio` listed with
`/Library/Extensions/LatSOFAudio.kext`.

> **Do not use `kmutil install --update-all`.** That subcommand rebuilds the
> Boot and System collections and demands a Kernel Debug Kit matching your
> exact macOS build — on a hackintosh it fails with _"Missing Developer Kit"_.
> `kmutil load -p` is the right command; it only touches the auxiliary
> collection, which is where third-party kexts belong.

**The other thing `kmutil` may answer is `Code=27` — the approval gate:**

```
Error Domain=KMErrorDomain Code=27 "Extension with identifiers
com.hackintosh.LatSOFAudio not approved to load. Please approve using
System Settings."
```

Nothing is wrong with the kext; macOS is asking for your consent and will not
stage it until you give it. Do this:

1. Open **System Settings → Privacy & Security**.
2. **Scroll to the very bottom of that pane.** The approval row sits below
   everything else and is easy to miss. (Some builds also post a _"System
   Extension Blocked"_ notification — same control, same place.)
3. Click **Allow** and enter your password.
4. Run the **same** `kmutil load -p` command again. It should now answer with
   the `Code=28` "requires a reboot" message above.

> **This prompt comes back every time the kext binary changes.** It is not a
> one-off for first installs: rebuild the kext, reinstall it, and macOS sees a
> new identity (a new UUID) and asks again. Expect to approve-and-re-run every
> time you replace the kext, not just on the first install.
>
> The reason is the SIP value from step 0. `0x803` sets the
> allow-unsigned-kexts bit (`0x1`) but **not** the skip-approval bit (`0x200`),
> so each new binary needs a human. If you rebuild often you can set
> `csr-active-config` to `030A0000` (that is `0xA03` = `0x803 | 0x200`) the same
> way as in step 0 — a **Data** entry, plus the `NVRAM → Delete` line — and the
> prompt stops for good, at the cost of one more SIP bit lowered. Optional, and
> only worth it on a machine you are developing on.

Now reboot.

## 5. Reboot and verify

Three checks, in order. Each one tells you something different.

**Is the kext loaded?**

```sh
kmutil showloaded | grep -i latsof
```

Expect a line containing `com.hackintosh.LatSOFAudio (1.1.5)`.

**Did the DSP come up and did it publish an audio device?**

```sh
ioreg -rc LatSOFAudioDevice -d 1 -w0 | grep -E "Status|SD-Borrow|SD-Final"
ioreg -rc LatSOFKernelAudioDevice -w0 | grep IOAudioDeviceName
```

You want `"Status" = "OK"`, the device name `LatSOF Internal Microphone`, and
the borrow check to pass. The two SD lines look like this (stream number and
register values vary by machine — these are the reference laptop's):

```
"SD-Borrow" = "sd7 ctl=0x040000 fmt=0x0000 bdl=0x00000000 ppctl=0x40000000"
"SD-Final"  = "sd7 ctl=0x040000 fmt=0x0000 bdl=0x00000000 cbl=0 lvi=0 ppctl=0x40000000 spiben=0x00000000"
```

They are **not** textually identical — `SD-Final` reports three extra fields
(`cbl`, `lvi`, `spiben`). The check is **field-for-field**: every field that
appears in _both_ lines (`ctl`, `fmt`, `bdl`, `ppctl`) must show the same
value in each. That's the proof AppleHDA's borrowed stream was handed back
untouched — see [`ARCHITECTURE.md`](ARCHITECTURE.md).

> **Status vocabulary since patches 30–32.** `"Status" = "OK"` is still the
> pass, and a bare `FAILED: FW load` on a quiet cold boot still means roll
> back. But several states that _look_ alarming are working-as-designed:
>
> | Property                                                   | Meaning                                                                                                                                                                                                                                                                                                                              |
> | ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
> | `Status = "deferred: AppleHDA output busy at load"`        | the output descriptor was busy at load; the retry engine takes over. Under the default `latsof_strictborrow=1` a merely-_programmed_ SD7 never frees again this session, so this is **not** a wait for quiet — it is bounded at `kProgrammedWaitRounds` × 1.5 s ≈ 30 s, after which it borrows anyway and publishes `Borrow-Warning` |
> | `Status = "deferred: output became busy before borrow"`    | the descriptor changed state between preflight and borrow, and the attempt bailed before writing anything. **Both `SD-Borrow` and `SD-Final` are normally absent** — the bail happens before `SD-Borrow` is written                                                                                                                  |
> | `Status = "deferred: init attempt n/12 failed — retrying"` | a rebuild attempt failed with budget left; still in progress                                                                                                                                                                                                                                                                         |
> | `Status = "deferred: retrying after FAILED: … (n/12)"`     | same, carrying the underlying failure forward                                                                                                                                                                                                                                                                                        |
> | `Status = "FAILED: DSP init retries exhausted (last: …)"`  | terminal for _this_ round — but a sleep/wake or simply re-selecting the mic re-arms it (`Wake-Retry = "re-armed by capture demand"`)                                                                                                                                                                                                 |
> | `Wake-Retry = "deferred: SD7 programmed"`                  | the wake-path equivalent of row 1                                                                                                                                                                                                                                                                                                    |
>
> **Any `Status` beginning `deferred:` is in-progress, never terminal.** Only a
> bare `FAILED: …` is a verdict.
>
> Other properties worth knowing: `AFG-Probe` (proof the in-kernel codec
> access works — appears on first playback), `AFG-Wake` (count of headphone
> power-state corrections), `Engine-Resume` (sleep/wake restart machinery),
> and `Capture-Debug` should show `ppctl=0x40000040 ctl=0x74xxxx` — capture
> lives on **SD6, tag 7** since patch-32.

**Does macOS see a microphone?**

```sh
system_profiler SPAudioDataType | grep -A8 "LatSOF DMIC"
```

Expect a block like:

```
LatSOF DMIC capture:

  Default Input Device: Yes
  Input Channels: 2
  Manufacturer: Dell Latitude 3410
  Current SampleRate: 48000
  Transport: Built-in
  Input Source: Internal Microphone
```

**Now test it:** System Settings → Sound → Input. **You will see more input
devices than this laptop has microphones** — pick **LatSOF DMIC capture**. The
others are AppleHDA's codec paths: **Built-in Microphone**, which records only
silence here because no mic is wired to the codec, and — on the minrate layout
from §7 (`alcid=92`), which publishes two input engines — also **Built-in Line
Input**, the combo jack's mic half. Their presence is expected and they
**cannot be removed**; see the box at the top of §7.

**Do not use the `Input Source` field to tell them apart:** it reads
`Internal Microphone` for **LatSOF DMIC capture** too, as the block above
shows. Go by the device name. Since patch-32 moved capture to SD6, selecting
one of the codec inputs and switching back no longer wedges the driver.

Speak and watch the
level meter move, then try Dictation (press the dictation key or Edit → Start
Dictation) and say a sentence.

## 6. Why not the EFI?

Older versions of this project injected the kext through OpenCore. **That no
longer works, and the failure is silent** — no error in any log, the kext
simply never appears.

The reason: this kext depends on Apple's `IOAudioFamily`, which lives in the
**System** kernel collection. OpenCore injects into the **Boot** collection and
can only resolve dependencies found there. It hits an unresolvable dependency
and drops the bundle without complaining. You can see the split yourself:

```sh
kmutil inspect | awk '/collection at/{c=$0} /IOAudioFamily/{print c; print "  " $0}'
```

Installing to `/Library/Extensions` hands the job to macOS's own linker
(`kernelmanagerd`), which can see every collection. That's the supported path
for third-party kexts that depend on system frameworks, and it's what the
OpenCore maintainers recommend for exactly this situation.

**If you previously added a `Kernel → Add` entry for `LatSOFAudio.kext`, set
its `Enabled` to `false`** (or remove it). Leaving both in place risks two
copies of the driver racing for the same hardware.

## 7. Fix headphone crackle (optional but recommended)

> **SUPERSEDED on the reference machine (2 Aug 2026), kept as a field guide.**
> The rate fault is now closed at the source: a custom AppleALC build
> (`contrib/alc236-layout91-rebuild.sh`, boot with `alcid=92`) adds
> `MinimumSampleRate = 48000` to the ALC236 output path, so AppleHDA never
> publishes 44.1 kHz at all — the stuck cold boot, the replug roulette and
> the mid-call rate drag described below **cannot occur**. Disassembly of
> the running `AppleHDAPath::isAudioStreamSupported` confirmed the key is a
> hard gate, not advisory. Consequences for this section:
>
> - `latsof-setrate` is still a fine one-shot _diagnostic_ (`latsof-setrate`
>   with no argument reports the rate), and the correct tool on machines
>   running a stock layout.
> - **Do not install the `--enforce` LaunchAgent** described at the end of
>   this section — under the minrate layout it has nothing to do, and this
>   project's history shows resident audio-state writers must earn their
>   keep. The reference machine runs none.
> - Do not remove the ghost "Built-in Microphone"/"Built-in Line Input"
>   devices the stock-derived layout publishes: layout variants 90–97
>   proved on hardware that every route to removing them breaks the
>   headphone amp through analog state no codec register exposes, while the
>   capture-side hazard they created was fixed in the driver instead
>   (patch-32 moved capture to SD6). See `DEBUGGING-LOG.md`.

Unrelated to the microphone, but it bites nearly every ALC laptop and this is
where people look for it.

**Symptom:** static, crackling or popping from the 3.5 mm headphone jack.

**Cause:** the codec is running at 44,100 Hz. It wants 48,000 Hz.

**The trap that costs people an evening:** switching the rate in Audio MIDI
Setup while audio is playing is **not** a valid test. The codec path is only
fully reprogrammed when the audio engine is rebuilt — so after changing the
rate you must **sleep and wake the machine** (or reboot) before judging it. A
live switch will falsely tell you the sample rate wasn't the problem.

**The fix:** Open **Audio MIDI Setup** (Applications → Utilities), select your
output device, set Format to **48,000 Hz**, then sleep and wake the machine.

> **Use lid-close sleep.** On the reference machine, sleeping from the Apple
> menu fails with an EFI/Bootrom `0x1F` error and costs a reboot; closing the
> lid sleeps normally. This is an open OpenCore issue on this board, unrelated
> to audio — `config-noonln.plist` is staged as a candidate fix but is
> untested. Wherever this guide says "sleep and wake", close the lid.

> ### Do NOT automate this with a resident rate-pinning agent
>
> This was tried on the reference machine, and it **caused a worse fault than
> the one it fixed.** Measured on 29 July 2026:
>
> When the headphone jack is plugged in, AppleHDA rebuilds the output engine
> and passes _through_ 44,100 Hz before settling at 48,000 Hz on its own. A
> resident watcher that reacts to that transient writes 48,000 Hz **while the
> codec path is still being programmed for 44,100** — leaving the stream and
> the codec disagreeing. The result is harsh static on **both** headphones and
> speakers, curable only by physically replugging the jack (a `coreaudiod`
> restart does not fix it, because only a real jack event reprograms the codec
> path).
>
> With no watcher running, the same plug settles at 48,000 Hz by itself and
> sounds clean. The transient is normal behaviour, not a fault to correct.
>
> The general lesson, which this project learned twice: **never write the
> sample rate while the engine is being rebuilt.** If you want a persistent
> pin, it must wait until the device has been quiet for several seconds and
> only then correct a rate that is genuinely stuck — the naive version
> actively breaks audio.
>
> **That safe version now exists** — `latsof-setrate --enforce 48000`, described
> at the end of this section. It is built around exactly the rule above: it
> reacts to _quiet_, never to change. Do not replace it with something that
> writes the rate from a notification callback, which is what failed here.

### The `latsof-setrate` helper — install it once

> **Stock-layout machines only.** On the minrate layout (`alcid=92` — see the
> box at the top of §7) 44.1 kHz is never published, so the fault below cannot
> occur. Kept as a field guide for machines running a stock AppleALC layout.

The rest of this section uses a small command-line tool that reports and pins
the analog output's sample rate. It is under 200 lines of CoreAudio: no
firmware, no kext, no background process. Build it and put it on your `PATH`
once — every later mention in this document is the bare command name.

```sh
cd /path/to/LatSOFAudio                       # the repo you cloned in step 1
clang -O2 -framework CoreAudio -framework CoreFoundation \
      -o latsof-setrate contrib/latsof-setrate.c
sudo install -d -m 755 /usr/local/bin
sudo install -m 755 latsof-setrate /usr/local/bin/
rm latsof-setrate                             # keep only the installed copy
```

That last `rm` is not housekeeping. `.gitignore` here covers `*.o`, `*.kext/`
and `build/`, but not a bare `latsof-setrate` binary sitting at the repo root
— leave it there and you will eventually commit a Mach-O by accident.

`/usr/local/bin` is first on the default `PATH`, is not SIP-protected, and
survives macOS updates — so the tool is still there on the day you need it,
which may be months from now and long after this checkout has moved or been
deleted. That is the whole reason it goes there rather than being run out of
the repo. Note what this does **not** install: no LaunchAgent, no login item,
nothing that runs on its own.

Two forms, both one-shot:

```sh
latsof-setrate                 # report the current rate
latsof-setrate 48000           # pin to 48 kHz
```

The report form prints one line per analog output engine (the UID suffix is
machine-specific — yours will differ):

```
AppleHDAEngineOutput:1F,3,0,1,0:0  current=48000
```

`current=48000` is the healthy reading; `current=44100` is the fault this
section is about. The pin form prints a timestamped line when it changes
something —

```
14:22:07 AppleHDAEngineOutput:1F,3,0,1,0:0  44100 -> 48000 (status 0)
```

— and **prints nothing at all when the rate is already correct**: silence
means it had no work to do, not that it failed.

> **`--watch` is an alias for `--enforce`, not the retracted agent.**
> `contrib/latsof-setrate.c:268-270` parses both flags identically into the
> same debounced, quiet-triggered path — the source comment on that line reads
> "same safe mode now". The dangerous version, which re-pinned from a
> notification callback and produced static on both outputs, was removed in
> `05289f3`/`2c677c6` and exists only in git history. Running `--watch`
> therefore **cannot** reproduce the fault described above. (An earlier
> revision of this box said the opposite; it predated those commits.)
>
> Either flag still starts a **resident process**, which is a separate question
> from whether it is safe. See "Enforcing 48 kHz automatically" below — and
> skip that subsection entirely if you built the minrate layout, where 44.1 kHz
> is unpublishable and there is nothing to enforce. The reference machine runs
> no resident rate agent.

### The cold-boot case, and its 20-second cure

> **Stock-layout machines only.** On the minrate layout (`alcid=92` — see the
> box at the top of §7) 44.1 kHz is never published, so the fault below cannot
> occur. Kept as a field guide for machines running a stock AppleALC layout.

Measured on the reference machine: if you **boot with headphones already
plugged in**, the output engine can come up at 44,100 Hz and _stay_ there.
Replugging alone does not help — each replug rebuilds at 44.1 again — so the
static appears permanent until the machine is slept and woken, which is when
it lands on 48,000 Hz and goes clean. macOS stores 48,000 in
`/Library/Preferences/Audio/com.apple.audio.DeviceSettings.plist`, but does
not reliably apply it during a cold boot with the jack occupied.

The cure, without sleeping the machine — **order matters**:

```sh
latsof-setrate 48000                                              # 1. pin
# 2. unplug and replug the headphones
# 3. play something — clean
```

Step 1 alone does nothing audible (a live rate switch never reprograms the
codec path). Step 2 is what rebuilds the engine, and because the rate is
already pinned, it rebuilds _at_ 48 kHz. Sleep/wake achieves the same thing;
this is just faster. If you rarely boot with headphones connected, you will
probably never meet this at all.

**If you ever find the output genuinely stuck at 44,100 Hz** (checked while
idle, not during a plug), the same two steps apply — pin, then rebuild:

```sh
latsof-setrate                   # report the current rate
latsof-setrate 48000             # pin it once
```

Then sleep/wake or replug the jack so the codec path is reprogrammed at the
new rate. That is the whole fix; no background process required. (If the shell
answers `command not found`, you have not built and installed the helper yet —
see "The `latsof-setrate` helper" above.)

`CodecCommander.kext` is also worth having in your EFI for jack-related pops,
but it does **not** fix this particular problem — the sample rate does.

### If crackle persists at 48 kHz — the other causes

> **Only the rate fault above is layout-scoped.** Everything in this
> subsection is layout-independent and applies on the minrate layout too.

Three different faults all sound like "headphone static", and fixing one does
nothing for the others.

**Fault B — a powered-down function group (the idle-plug case).** If the static
appears specifically after plugging headphones into an _idle_ engine and then
pressing play, and a replug clears it, this is it. AppleHDA idles the codec's
Audio Function Group (node `0x01`) to D3 and, on that path, starts streaming
without restoring it — so audio is driven through a powered-down path. The
headphone pin and DAC report "requested D0, actually D3"; writing D0 to them
directly succeeds and changes nothing, because the blocker is their parent.

**This is fixed in the driver since patch-31** — the kext restores the AFG
itself. Confirm it is working:

```sh
ioreg -rc LatSOFAudioDevice -d 1 -w0 | grep -E 'AFG-Probe|AFG-Wake'
```

`AFG-Probe` proves the in-kernel codec access works; `AFG-Wake` counts the
corrections it has made. If `AFG-Wake` reads `ICI unavailable — keep the
daemon`, the kext could not reach the codec and the retired userland helper
`/usr/local/bin/latsof-afgwake` would be needed — that string should never
appear on a machine booting `alcverbs=1`. Background:
[`ARCHITECTURE.md`](ARCHITECTURE.md), "The AFG keep-alive".

**Fault C — CoreAudio missing a render deadline.** Distinguishable from the
rate fault:

|             | 44.1 kHz fault                                       | dropout fault                     |
| ----------- | ---------------------------------------------------- | --------------------------------- |
| Sounds like | constant, on every sound                             | occasional bursts, comes and goes |
| Depends on  | the sample rate                                      | CPU load / power state            |
| Verify      | `system_profiler SPAudioDataType \| grep SampleRate` | the log check below               |

The second is CoreAudio missing an audio render deadline. Check for it:

```sh
log show --last 1h --predicate 'subsystem == "com.apple.coreaudio"' \
  --style compact | grep -i "overload\|skipping cycle"
```

A line like `HALC_ProxyIOContext::IOWorkLoop: skipping cycle due to overload`
is a confirmed dropout — you heard that one.

On a laptop the usual trigger is **power management**, not audio at all:

```sh
pmset -g | grep -i lowpowermode        # 1 = Low Power Mode is on
pmset -g batt                          # on battery? low charge?
```

macOS enables Low Power Mode automatically when the battery gets low, and the
CPU throttling that follows is enough to make a hackintosh miss audio
deadlines. Plug in the charger, or turn Low Power Mode off in System Settings
→ Battery, and see whether the bursts stop. (They are usually more audible in
headphones than through laptop speakers, which mask short glitches — so
"headphones only" does not by itself mean the codec is at fault.)

### Crackle during calls — the full-duplex rate mismatch

> **Stock-layout machines only.** On the minrate layout (`alcid=92` — see the
> box at the top of §7) 44.1 kHz is never published, so the fault below cannot
> occur. Kept as a field guide for machines running a stock AppleALC layout.

There is a **third** fault, and it is the one that hits video-call apps. It has
nothing to do with the codec or with CPU load, and it is worth knowing because
the cure is instant.

**This microphone is 48 kHz only.** The DSP pipeline runs at 48 kHz and the
kernel engine publishes exactly that one rate — there is no 44.1 kHz to offer.
Some apps nevertheless drag the _output_ device to 44,100 Hz when a call
starts: FaceTime does this reliably, on the reference machine, even for
audio-only calls. CoreAudio is then bridging a 48 kHz input engine and a 44.1
kHz output engine for every buffer — sample-rate conversion plus drift
compensation — and its IO thread starts missing deadlines.

Measured during a live FaceTime call on 29 July 2026: **746 overload events in
60 seconds**, all of them logged by `avconferenced` (FaceTime's audio daemon)
rather than the system at large, with the machine otherwise idle.

Diagnose it in one line — note the process names, which is what distinguishes
this from the CPU-load case above:

```sh
log show --last 2m --predicate 'subsystem == "com.apple.coreaudio"' \
  --style compact | grep -i overload | awk '{print $4}' | sort | uniq -c
```

If the count is concentrated in one app's audio daemon (`avconferenced`,
`Slack Helper`, a browser helper) while the machine is not busy, check the
output rate — it will be 44,100.

**The cure, safe to run mid-call:**

```sh
latsof-setrate 48000
```

The overloads stop immediately and the crackle goes with them; on the reference
machine the count went from roughly six per second to zero within the same
second. Note that this is the one situation where a live rate change _is_ the
fix rather than a false lead: nothing here depends on reprogramming the codec
path, only on the two engines agreeing on a rate. The rate reverts to 48 kHz by
itself when the call ends.

A call that runs at 48 kHz on both sides never shows this at all — Slack calls
on the same machine were clean throughout.

### Enforcing 48 kHz automatically

> **Stock-layout machines only.** If you built the minrate layout (`alcid=92`,
> `MinimumSampleRate = 48000` — see the box at the top of §7), skip this whole
> subsection. 44.1 kHz is unpublishable there, so the enforcer has nothing to
> correct and the reference machine runs none. Installing it anyway adds a
> resident audio-state writer for no benefit, which this project's history
> argues against. This section is kept for machines on a stock layout, where
> the problem it solves is real.

Running the command by hand every time a call app pulls the rate down gets old.
`--enforce` does it for you, and it is safe _because of how it is shaped_:

```sh
latsof-setrate --enforce 48000
```

It **reacts to quiet, never to change.** Device notifications only restart an
eight-second timer; the rate is corrected solely once it has been wrong
continuously for that long with nothing else happening. That separates the two
cases exactly:

| Situation                        | 44.1 kHz lasts                    | Enforcer      |
| -------------------------------- | --------------------------------- | ------------- |
| Headphone jack plugged in        | under a second, settles by itself | never fires   |
| Call app holding the output down | the whole call                    | corrects once |

The first row is the important one: writing the rate during a jack rebuild is
what produced the harsh static described in the box above, so the enforcer is
built specifically not to be there. Both behaviours were verified before this
was documented — a transient flip produced zero corrections, a persistent one
produced exactly one. There is also a runaway guard: if something keeps
resetting the rate, it backs off to one attempt a minute and says so, rather
than fighting in a loop.

To run it at login, install it as a LaunchAgent. Paste the whole block:

```sh
mkdir -p ~/Library/LaunchAgents ~/Library/Logs
cat > ~/Library/LaunchAgents/com.hackintosh.latsof.setrate.plist <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>com.hackintosh.latsof.setrate</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/local/bin/latsof-setrate</string>
        <string>--enforce</string>
        <string>48000</string>
    </array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>StandardOutPath</key><string>$HOME/Library/Logs/latsof-setrate.log</string>
    <key>StandardErrorPath</key><string>$HOME/Library/Logs/latsof-setrate.log</string>
</dict>
</plist>
EOF
plutil -lint ~/Library/LaunchAgents/com.hackintosh.latsof.setrate.plist
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.hackintosh.latsof.setrate.plist
```

`plutil` must print `OK`, and `bootstrap` prints nothing when it works. Check it
with `launchctl list | grep latsof` (an entry with a PID) and
`cat ~/Library/Logs/latsof-setrate.log`, which should open with
`enforcing 48000 Hz`. Every later line in that log is a correction it made —
useful evidence, and it should be a short file. To remove it:

```sh
launchctl bootout gui/$(id -u)/com.hackintosh.latsof.setrate
rm ~/Library/LaunchAgents/com.hackintosh.latsof.setrate.plist
```

**Do not mistake this for a performance problem.** It looks like one: the
symptom is dropped audio, and the machine is a 4-core laptop. But after pinning
48 kHz on the reference machine, the same FaceTime call was switched from
audio-only to **full video** — adding hardware decode, the camera daemon and
roughly 20% more CPU — and stayed perfectly clean. Rate agreement is what
matters here; headroom is not. Chasing CPU usage in this situation wastes the
afternoon (it did).

## 8. Siri

Siri works with this driver as of the July 2026 kernel-audio release, on the
bare laptop with nothing plugged in.

First, make sure Siri is actually enabled: System Settings → Apple
Intelligence & Siri → turn **Siri** on and pick an activation shortcut. Don't
be alarmed that **Apple Intelligence** itself is absent or greyed out in that
same pane — it is hardware-gated by Apple (T2 / Apple silicon) and no driver
can change that. Siri and Apple Intelligence are separate things; only Siri
is on offer here.

If Siri misbehaves, work through these in order — the first two are settings,
not bugs.

**"Siri Not Available — Connect a microphone"**

1. Confirm the mic itself works (Dictation test in step 5). If Dictation
   fails, this is a driver problem, not a Siri problem — go to Troubleshooting.
2. Confirm you are running the kernel-audio build. Older builds published the
   mic from a since-retired userspace plugin that never advertised the
   `'imic'` data source Siri requires:

   ```sh
   ioreg -rc LatSOFKernelAudioDevice -w0 | head -2
   ```

   No output means you're on an old build; rebuild and reinstall.

3. Check what Siri actually chose:

   ```sh
   log show --last 5m --predicate 'process == "corespeechd"' --style compact \
     | grep -i recordroute | tail -3
   ```

   `RecordRoute: LatSOFKernelAudioEngine:0` means Siri selected our
   microphone.

**Siri answers on screen but doesn't speak**

System Settings → Apple Intelligence & Siri → **Siri Responses** → turn on
**Voice feedback**. It is off by default on some installs and produces exactly
this symptom.

**Siri hears nothing and gives up (any microphone)**

Check DNS before blaming audio. A poisoned resolver entry pointing
`guzzoni.apple.com` (Siri's speech endpoint) at `0.0.0.0` makes every request
fail server-side regardless of microphone:

```sh
dscacheutil -q host -a name guzzoni.apple.com
```

If you see `0.0.0.0`, fix it with:

```sh
sudo dscacheutil -flushcache && sudo killall -HUP mDNSResponder
```

**Things that are normal and not faults**

- **No listening tone when Siri opens.** macOS deliberately suppresses the beep
  for built-in microphones without hardware echo cancellation, so the mic can't
  hear it. Real MacBooks behave the same way. If you _used_ to hear the tone
  with a USB mic, that was the external-microphone code path.
- **The log line `supported : 0`** next to your device ID. Ignore it. That
  check asks whether the device is an Apple Studio Display; every microphone in
  the world fails it.

**Why this needed a kernel driver at all:** Siri only accepts a built-in-type
device if it advertises an _input data source_ of `'imic'` ("internal
microphone"). USB devices skip that check entirely — which is why a USB mic
always worked. The kext publishes the `'imic'` selector so the internal mics
qualify. Details in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## 9. Updating, rolling back, uninstalling

### Updating to a new build

Never copy a new kext _over_ the installed one. Remove it, copy, then **prove**
you actually got the new binary. Run this from the `kext/` directory `make`
wrote to, as in step 4:

```sh
sudo rm -rf /Library/Extensions/LatSOFAudio.kext
sudo ditto LatSOFAudio.kext /Library/Extensions/LatSOFAudio.kext
md5 -q /Library/Extensions/LatSOFAudio.kext/Contents/MacOS/LatSOFAudio
md5 -q LatSOFAudio.kext/Contents/MacOS/LatSOFAudio          # must match
sudo chown -R root:wheel /Library/Extensions/LatSOFAudio.kext
sudo chmod -R 755 /Library/Extensions/LatSOFAudio.kext
sudo kmutil load -p /Library/Extensions/LatSOFAudio.kext
```

**The two `md5` lines are not paranoia.** On the reference machine on 29 July
2026 the `ditto` printed no error and the _old_ binary was still sitting in
`/Library/Extensions` — every later check looked fine and the fix under test
simply wasn't there. The hash is what caught it. If the two hashes differ, the
copy did not happen: confirm the old bundle was really removed, and that you
are copying from the directory `make` just wrote to and not an older tree.

**Expect the approval prompt again.** A new binary is a new identity to macOS,
so `kmutil load -p` will usually answer this rather than the `Code=28` from
step 4:

```
Error Domain=KMErrorDomain Code=27 "Extension with identifiers
com.hackintosh.LatSOFAudio not approved to load. Please approve using
System Settings."
```

Open System Settings → Privacy & Security, scroll to the **bottom**, click
**Allow**, enter your password, then re-run the same `kmutil load -p` command
— it should then give the `Code=28` "requires a reboot" message. This happens
on every install _and every rebuild_, because the `csr-active-config` value
from step 0 (`0x803`) allows unsigned kexts but does not include the
skip-approval bit. If you rebuild often and would rather never see the prompt,
`0xA03` — `030a0000` in `config.plist` — adds that bit, at the cost of one more
lowered SIP bit.

**Don't do this while audio is playing — and prefer rebooting outright.** The
driver borrows AppleHDA's first output stream while it starts up. Since
patch-30 a _streaming_ descriptor is detected and the DSP init is deferred to
the retry engine, publishing `Status = "deferred: AppleHDA output busy at
load"` (see §5 — that is working-as-designed, not a failure). But a descriptor
that is merely **programmed** — RUN clear, buffers still live, which is its
state for the rest of the session once AppleHDA has played anything — is
deferred only under the default `latsof_strictborrow=1`. The reference machine
boots `latsof_strictborrow=0` and borrows it anyway, which can produce static
that replugging does **not** clear. Stop playback first, and if this session
has played audio at all, skip the live load and just reboot.

**Then reboot.** `kmutil showloaded` reports the kext that is _running_, which
is still the old one until you restart — a new hash on disk together with the
old build in `showloaded` is normal _before_ the reboot and a bug _after_ it.
Check both.

**After the reboot, verify — step 5's three checks, in the same order:**

```sh
kmutil showloaded | grep -i latsof
ioreg -rc LatSOFAudioDevice -d 1 -w0 | grep -E "Status|SD-Borrow|SD-Final"
system_profiler SPAudioDataType | grep -A8 "LatSOF DMIC"
```

In order, you want: `com.hackintosh.LatSOFAudio (1.1.5)` followed by a UUID —
and if you noted the old build's UUID before updating, this one must be
_different_, which is your proof the running kext is the new one; then
`"Status" = "OK"`, with `SD-Borrow` and `SD-Final` naming the same stream and
agreeing on every field the two have in common (`SD-Final` prints three more:
`cbl`, `lvi`, `spiben`). `sd7` here is the **output** stream the loader borrows
and returns — distinct from capture, which has run on **SD6, tag 7** since
patch-32. Seeing both numbers is correct, not a contradiction. (The number varies by
machine, and step 5 explains the comparison); then a `LatSOF DMIC capture`
block reading `Default Input Device: Yes`, `Input Channels: 2`,
`Current SampleRate: 48000`, `Transport: Built-in` and
`Input Source: Internal Microphone`. Step 5 shows all three in full.

If `Status` reads `FAILED: FW load` instead, the new build did not bring the
DSP up and the microphone is dead — roll back, then see Troubleshooting.

### Rolling back

There is no separate rollback procedure: **keep the previous `LatSOFAudio.kext`
bundle** and reinstall it with exactly the update steps above. Copy it out
before you overwrite it — the build directory is not a backup, since the next
`make` relinks the binary in place and `make clean` deletes the bundle
outright:

```sh
sudo ditto /Library/Extensions/LatSOFAudio.kext ~/LatSOFAudio-known-good.kext
```

Then roll back by running the update block with `~/LatSOFAudio-known-good.kext`
as the `ditto` source. The `md5` check matters _more_ here, not less: two
builds of this kext are indistinguishable from the outside, so the hash is the
only thing that tells you which one you are running. If the approval prompt
reappears, approve and re-run as above; either way, reboot and verify.

### Uninstalling

**Remove the kext:**

```sh
sudo rm -rf /Library/Extensions/LatSOFAudio.kext
```

Reboot. macOS rebuilds the auxiliary collection from what's present in
`/Library/Extensions`, so the driver is gone after the restart.

**Coming from the old HAL-plugin version?** Remove it, or the two will fight
over the same DMA ring and each one's stop will kill the other:

```sh
sudo rm -rf /Library/Audio/Plug-Ins/HAL/LatSOFAudioPlugin.driver
sudo killall coreaudiod
```

**Remove the command-line helpers** — the install steps put these outside the
bundle, so removing the kext does not take them with it:

```sh
sudo rm -f /usr/local/bin/latsof-setrate /usr/local/bin/latsof-afgwake
launchctl bootout gui/$(id -u)/com.hackintosh.latsof.afgwake 2>/dev/null
rm -f ~/Library/LaunchAgents/com.hackintosh.latsof.afgwake.plist
rm -f ~/Library/Logs/latsof-afgwake.log ~/Library/Logs/latsof-setrate.log
```

The LaunchAgent plist is the one that leaves a stray entry in System Settings →
General → Login Items if you forget it.

If you have a `LatSOFAudio.kext` entry in your `config.plist` under
`Kernel → Add`, set it to `Enabled = false`. (Following §6 you will not — the
kext never belonged in the EFI. The reference machine keeps the entry present
and disabled.)

## 10. Troubleshooting

**Build fails: `No rule to make target ... sof-cml.ri`** — the firmware is
missing or misplaced. Step 2; the path must be exactly
`kext/LatSOFAudio/Firmware/sof-cml.ri`.

**Any `kmutil` command complains "Missing Developer Kit … KDK matching your
build"** — you ran `kmutil install --update-all` (or another collection-wide
subcommand) instead of `kmutil load -p`. Use `kmutil load -p` as shown in
step 4; the KDK is never needed on this path.

**Kext absent from `kmutil showloaded` after reboot** — in order:

1. Is SIP allowing unsigned kexts? `nvram -p | grep csr-active-config` (step 0).
2. Is it actually in the collection? `kmutil inspect | grep -i latsof`.
3. Did macOS block it? System Settings → Privacy & Security → **Allow**.
4. Is ownership right? `ls -ld /Library/Extensions/LatSOFAudio.kext` must show
   `root wheel`.
5. Are you accidentally _also_ injecting it from EFI? Disable that entry
   (step 6).

**Kext loaded but no microphone in Sound settings** —

```sh
ioreg -rc LatSOFAudioDevice -d 1 -w0 | grep Status
```

`"Status" = "OK"` means the DSP booted. A `deferred: …` status is **neither
pass nor fail** — the bring-up is still running; wait and re-check (see the
Status vocabulary box in §5). If the status is a `FAILED: …` other than the
cases below, the firmware didn't load — wrong firmware generation (step 2) or
wrong hardware (step 0). If status is OK but no device appears, check the audio
side:

```sh
ioreg -rc LatSOFKernelAudioEngine -w0 | grep IOAudioEngineState
```

**`"Status" = "FAILED: FW load"`** — the DSP firmware did not load, and the
microphone is dead however healthy everything else looks: the kext is loaded
and there is nothing behind it. This is the specific case of the generic
"firmware didn't load" above, and it carries one extra diagnostic:

```sh
ioreg -rc LatSOFAudioDevice -d 1 -w0 | grep -E "Status|Wake-Retry"
```

If the mic died across a sleep/wake rather than at boot, you will also see
`"Wake-Retry-Done" = "GAVE UP after 12 tries"`: the driver re-attempted the
bring-up twelve times and the DSP never answered. If that command prints
_nothing at all_ while `kmutil showloaded` still lists the kext, the failure
happened during `start()` and macOS detached the device — the reason is only
in the log then:

```sh
log show --last 10m --predicate 'process == "kernel"' --style compact \
  | grep -i "latsof.*FAILED"
```

Causes, in order of likelihood: wrong firmware generation — it must be IPC3,
the v2.2.x `sof-cml.ri`, not IPC4 (step 2); a truncated file or a copied
symlink (step 2's 500–600 KB check); the board's mics aren't on the DSP at all
(step 0 / [`PORTING.md`](PORTING.md)); or, if you have modified the source, a
change to _which_ stream descriptor the code loader borrows. Keep it on
`SD(numISS)`, AppleHDA's first output stream (`sd7` on the reference laptop) —
scanning for a genuinely free descriptor instead has been tried, and it
produces exactly this failure: the borrow looks perfect in ioreg and the
firmware still refuses to load. See "The borrowed-stream contract" in
[`ARCHITECTURE.md`](ARCHITECTURE.md) before touching `initDSP()`.

If this appeared right after installing a new build, put the previous `.kext`
bundle back: `sudo rm -rf /Library/Extensions/LatSOFAudio.kext` first, then
step 4 verbatim — `ditto` onto an existing bundle can silently leave the old
binary in place. That separates a bad build from bad hardware in one reboot.

**Extra input entries / level meter dead** — you selected one of AppleHDA's
codec paths, **Built-in Microphone** or **Built-in Line Input**, both of which
record silence on this hardware. Select **LatSOF DMIC capture** instead
(step 5), going by the **device name**: the `Input Source` field reads
`Internal Microphone` for our device as well, so it cannot be used to tell them
apart. Their presence is normal and they cannot be removed.

**Microphone present but records silence (and you did select LatSOF DMIC
capture)** — **check `Status` first.** The device is published before the DSP
comes up, so a `deferred: …` status means the bring-up has not finished, not
that anything is wrong; wait and re-check. If the status is `OK`, then it is
wrong firmware (must be IPC3 `sof-cml`), or your board's mics aren't on the DSP
(step 0 / `PORTING.md`).

**Microphone permission prompts never appear / Privacy → Microphone is
empty** — you have `amfi=0x80` in your boot-args. (If you already run
AMFIPass and have no `amfi=0x80` — which is what the reference machine does —
this is not your cause; reset TCC for the app instead.) Remove it; it breaks TCC
prompts system-wide. If something else in your EFI needs AMFI relaxed
(OCLP-patched Wi-Fi is the usual culprit), use
[AMFIPass](https://github.com/acidanthera/AMFIPass) instead.

**The mic wedges after a video call** (device present, meter dead) — the
driver self-heals: it detects the wedged DSP and rebuilds it within a few
seconds. If it doesn't come back, check for a recovery attempt:

```sh
log show --last 10m --predicate 'process == "kernel"' --style compact \
  | grep -i "latsof.*recover"
```

**Speakers or playback misbehave after you modified the source** — read "The
interrupt-starvation bug" and "The borrowed-stream contract" in
[`ARCHITECTURE.md`](ARCHITECTURE.md). The stock driver enables no interrupts,
deliberately, and hands AppleHDA's borrowed stream back byte-identical.

**Something else** — open an issue with: your `ioreg -rc LatSOFAudioDevice -d 1`
output, `kmutil showloaded | grep -i latsof`, your macOS build
(`sw_vers`), and your laptop model and codec.
