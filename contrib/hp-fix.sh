#!/bin/zsh
# hp-fix.sh — cure the idle-plug headphone static by powering up the
# ALC236 Audio Function Group.
#
# MEASURED ROOT CAUSE (1 Aug 2026). On a jack insert that lands on an IDLE
# engine, a codec dump in the static state vs the (replug-)clean state
# differs in exactly two lines: the HP pin 0x21 and its DAC 0x03 report
# GET_POWER_STATE = 0x30 — requested D0, ACTUAL still D3. Everything else
# (pin control 0xc0, EAPD, amp gains, connect-sel, stream format) is
# byte-identical, which rules out the rate, the amp and the routing.
#
# The reason they are stuck is one level up: node 0x01, the Audio Function
# Group, reads 0x233 = requested D3 / actual D3. In HDA the AFG is the
# parent of every widget, so while it sits in D3 no child can reach D0 —
# commands to 0x21/0x03 return success and change nothing. AppleHDA drops
# the AFG to D3 when the engine idles and, on the idle-jack-insert path,
# starts streaming without restoring it. A replug forces its full re-init,
# which is why replugging "cures" the static.
#
# Setting the AFG to D0 cascades: 0x01, 0x21 and 0x03 all go to 0x00 at
# once. SET_POWER_STATE D0 is idempotent and safe to re-run at any time.
NODES=(0x01 0x21 0x03)
echo "before:"
for n in $NODES; do printf "  %s power=%s\n" "$n" "$(alc-verb $n GET_POWER_STATE 0 2>/dev/null | tail -1)"; done

# The AFG first — the children follow it. Errors are NOT suppressed.
alc-verb 0x01 SET_POWER_STATE 0x0 2>&1 | grep -iv '^nid' | grep -v '^0x00000000$'
sleep 1

echo "after:"
for n in $NODES; do printf "  %s power=%s\n" "$n" "$(alc-verb $n GET_POWER_STATE 0 2>/dev/null | tail -1)"; done
echo
echo "0x00 = D0 requested AND actual (good)."
echo "0x30 on 0x21/0x03 = stuck in D3 -> check 0x01; 0x233 there is the real fault."
