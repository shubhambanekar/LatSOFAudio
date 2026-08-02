#!/bin/zsh
# check-layout.sh — one-shot post-boot verdict for a LatSOF layout test.
# Encodes the lessons of 1 Aug: count AppleHDAEngineInput INSTANCES (the
# IOAudioEngineDescription grep lied on layout 95 — disabled pins strip the
# name, not the engine), verify 44.1 kHz is unpublishable on the NEW output
# UID (a layout change renumbers engines onto UIDs with stale 44100 prefs),
# and check the default input survived the renumbering.
echo "=== layout ==="
nvram boot-args 2>/dev/null | sed 's/boot-args\t//' | awk '{print $1}'
ioreg -rxn HDEF -d 1 2>/dev/null | grep -i alc-layout

echo "\n=== INPUT ENGINES (the honest count) ==="
n=$(ioreg -rc AppleHDAEngineInput -w0 2>/dev/null | grep -c '"IOAudioEngineGlobalUniqueID"')
ioreg -rc AppleHDAEngineInput -w0 2>/dev/null | grep -o '"IOAudioEngineGlobalUniqueID" = "[^"]*"'
case $n in
  0) echo "-> 0 ghost inputs. (Unexpected under 97 — Lini should remain.)";;
  1) echo "-> 1 input engine = Lini only. GHOST MIC REMOVED — 97 goal met.";;
  2) echo "-> 2 input engines: the ghost mic SURVIVED (97 goal failed).";;
  *) echo "-> $n input engines?!";;
esac

echo "\n=== OUTPUT ==="
ioreg -rc AppleHDAEngineOutput -w0 2>/dev/null | grep -o '"IOAudioEngineGlobalUniqueID" = "[^"]*"'
r44=$(ioreg -rc AppleHDAEngineOutput -w0 -l 2>/dev/null | grep -o '"IOAudioSampleRateWholeNumber"=44100' | wc -l | tr -d ' ')
echo "44100 formats published: $r44   (MUST be 0 — the new UID carries a stale 44100 pref)"
ioreg -rc AppleHDAEngineOutput -w0 -l 2>/dev/null | grep -o '"IOAudioSampleRateWholeNumber"=[0-9]*' | sort -u

echo "\n=== MIC (ours) ==="
ioreg -rc LatSOFAudioDevice -d 1 -w0 2>/dev/null | grep -E '"Status"|"SD-Borrow"|AFG-Wake'
system_profiler SPAudioDataType 2>/dev/null | grep -B1 "Default Input Device: Yes" | head -3

echo "\n=== daemon ==="
launchctl list 2>/dev/null | grep -i afgwake || echo "afgwake agent not loaded"

echo "\nNEXT: play on speakers -> plug headphones (listen) -> unplug 10s ->"
echo "plug -> play (the idle case, listen). If static: run"
echo "  ~/Desktop/latsof-attempt2/hp-codec-dump.sh 97-static    (while hissing!)"
echo "then revert alcid to 92 and reboot — the dump makes even a failure useful."
