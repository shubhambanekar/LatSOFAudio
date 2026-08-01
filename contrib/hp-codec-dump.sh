#!/bin/zsh
# hp-codec-dump.sh — snapshot the ALC236 headphone path so the STATIC state
# can be diffed against the CLEAN state. Read-only (GET verbs only).
#
# Requires boot-arg alcverbs=1 (AppleALC user client).
#
# Usage:
#   ./hp-codec-dump.sh static     # right after an idle-plug that hisses
#   ./hp-codec-dump.sh clean      # right after the curing replug
#   diff /tmp/hp-static.txt /tmp/hp-clean.txt
#
# The differing node/verb IS the fix — encode it as a CodecCommander
# custom command for node 0x21 (headphone pin) / 0x03 (its DAC).
LABEL="${1:-dump}"
OUT="/tmp/hp-$LABEL.txt"

# nid  verb                        param   comment
READS=(
  "0x21 GET_PIN_WIDGET_CONTROL 0   HP-pin: out/HP-amp enable bits"
  "0x21 GET_EAPD_BTLENABLE     0   HP-pin: EAPD (amp power) — classic Realtek dropout"
  "0x21 GET_PIN_SENSE          0   HP-pin: jack presence"
  "0x21 GET_POWER_STATE        0   HP-pin: D0/D3"
  "0x21 GET_CONNECT_SEL        0   HP-pin: which DAC is selected"
  "0x03 GET_POWER_STATE        0   HP DAC: D0/D3"
  "0x03 GET_CONV               0   HP DAC: stream/channel assignment"
  "0x03 GET_STREAM_FORMAT      0   HP DAC: format (expect 48k)"
  "0x14 GET_PIN_WIDGET_CONTROL 0   Speaker pin (control)"
  "0x14 GET_EAPD_BTLENABLE     0   Speaker pin: EAPD"
  "0x02 GET_STREAM_FORMAT      0   Speaker DAC: format"
)

{
  echo "=== $LABEL @ $(date '+%F %T') ==="
  for r in "${READS[@]}"; do
    set -- ${=r}
    nid=$1; verb=$2; param=$3; shift 3; comment="$*"
    val=$(alc-verb "$nid" "$verb" "$param" 2>/dev/null | tail -1)
    printf "%-6s %-26s = %-12s # %s\n" "$nid" "$verb" "${val:-ERR}" "$comment"
  done
  echo "--- amp gains (HP pin + DAC, both channels) ---"
  for nid in 0x21 0x03; do
    for idx in 0x8000 0xa000; do   # 0x8000=output/left, 0xa000=output/right
      val=$(alc-verb "$nid" GET_AMP_GAIN_MUTE "$idx" 2>/dev/null | tail -1)
      printf "%-6s GET_AMP_GAIN_MUTE %-8s = %s\n" "$nid" "$idx" "${val:-ERR}"
    done
  done
} | tee "$OUT"

echo
echo "saved -> $OUT"
