# Development history

What changed and in what order, from a fresh fork to a working driver.

`DEBUGGING-LOG.md` in the repository root explains what each phase was trying
to achieve and which attempts failed; this is the condensed index.

| # | Change |
|---|---|
| 1 | Created the fork: matching switched to `IOResources`, `pciDevice->open()` removed, D3-D0 power cycle removed, I2S codec init stubbed out |
| 2 | AppleHDA coexistence groundwork |
| 3 | IORegistry-based state reporting - the kernel log is unusable on this machine because graphics messages flood the buffer |
| 4 | Fixed `probe()` casting its provider to `IOPCIDevice`, which silently prevented `start()` from ever running once the provider became `IOResources` |
| 5 | IPC mailbox transport: uplink `0x81000`, downlink `0x82000`, both verified against hardware |
| 6 | IPC ABI decode fix |
| 7 | DMIC capture pipeline, configured from this board's own NHLT data |
| 8 | First capture attempt |
| 9 | Removed the boot-time self-test that hung the machine by starting DMA while AppleHDA was still initialising the shared controller |
| 10 | The capture fix: the `PPCTL` decouple write used `capIdx` some 270 lines before it was assigned, so SD1 was never decoupled - and AppleHDA's SD0 was instead, on every boot, silently |
| 11 | Stopped the DSP mailbox interrupt from starving AppleHDA on the shared IRQ line |
| 12 | DC-blocking high-pass in the HAL plugin, applied before software gain so the offset never consumes headroom |
| 13 | Hardware-disciplined timestamps. **Did not fix the bug it was written for** - retained because it is the correct design regardless |
| 14 | The actual fix: stream-descriptor interrupts were enabled but never serviced, throttling the interrupt line AppleHDA depends on |
| 15-22 | Wake-time re-init hardening after audio was found not to survive sleep: wait for `GCAP` to decode, force D0 via the PCI PM capability, let AppleHDA bring the link up first, save/restore the borrowed output descriptor, stop clobbering AppleHDA's `INTCTL` bit, re-map BARs on wake, drive re-init from a retry engine. Individually defensible, several necessary - **none of them fixed the sleep bug** |
| 23 | Written, never applied. Correctly identified one of the three post-restore writes, but not the `cleanup:` block, so it would not have fixed it either |
| 24 | The actual fix: the borrowed-stream contract - snapshot once, restore once through an idempotent function called from every exit path, and verify with `SD-Final` read after the last write rather than `SD-Return` written before it |
| 25 | Removed the one remaining `GCTL` reset (`shutdownDSPGated`'s teardown of shared BAR0 registers) and made the playback selectors refuse - on this board any StartPlayback was guaranteed to time out into exactly that reset |
| 26 | Post-fix audit rounds, all failure-path bugs: the interrupt mask from 24 sat above the `cleanup:` label so every timeout path exited with the shared line armed; busy preflights consumed the 12-try wake budget (~18 s of playback at wake = mic dead until next sleep); the capture re-arm latch was lost on back-to-back sleeps; a failed BAR remap disabled the driver until reboot while claiming the next wake would retry. A second verification round then caught the first round's own regression - a StartCapture refused during the not-ready wake window was swallowed forever once StopIO cleared the latch, so a refused start now latches the demand - plus capture-start timeout paths that leaked RUN/PPCTL state, and `stop()` stream-stop guards that sat after the PMstop that cleared them |
| 27 | DSP self-recovery: a capture IPC timeout now declares the DSP dead and hands rebuild to the wake retry engine — the field failure was WebRTC clients desyncing the firmware's PCM state with rapid open/close cycles |
| 27b | Recovery episodes bounded to 3 - without the cap, a rebuild that "succeeded" against a dead mailbox re-entered recovery forever, blocking the workloop ~8.5 s of every 10 |
| 28 | The free-stream experiment: scan for an unused output descriptor instead of borrowing SD7. The scan worked; the firmware then refused to load at all. The SOF loader requires SD(numISS). Reverted, source kept as evidence |
| 29 | Edge-locked clock: DPIB advances in a ~1 ms staircase, so a random-instant read under-reports the write head (17.84 frames RMS vs AppleHDA's 0.37). Spin for the staircase edge, pair it with a bracketed clock read, discard preempted samples. Plus: a spurious-wrap fix (position reads 0 while the DSP is down) and a free-running timeline through DSP-down windows |
| 30 | The two sleep asymmetries: IOAudioFamily resumes without ever calling performAudioEngineStart, and the family's own pause cleared the wake latch - a held session now restarts via a deferred latch that jackPoll completes after the rebuild verifiably succeeds. The load path gained the RUN-bit preflight it always lacked. Five adversarial review rounds fixed 23 defects here before first boot, including a use-after-free in kext-unload teardown |
| 31 | The AFG keep-alive moved in-kernel: AppleHDA idles the codec's Audio Function Group to D3 and, on the idle-jack-insert path, streams without restoring it. One SET_POWER_STATE D0 over the Immediate Command Interface - not AppleHDA's CORB/RIRB. Three hardware rounds to get the triggers right; see DEBUGGING-LOG Phase 16 |
| 32 | Capture moved SD1 → SD6: a dead codec pin does not mean a dead DMA engine, and layout 92's ghost inputs run real streams that landed on SD1. Plus: a refused capture start re-arms a given-up retry engine, and rebuilds carry hot/cold context - cold wakes borrow a programmed SD7 immediately, rebuilds under session churn wait for a clean window |

The gap between 13 and 14 is the instructive part. 13 was a confident theory
about clock drift that a profiler disproved in a single command; the real cause
was two registers away, in what looked like a different subsystem entirely.

15-22 repeat that shape at larger scale, and are worth reading as a group. Eight
patches, each one simultaneously the experiment and the cure, against a bug
whose only instrument was the driver's own telemetry - telemetry that turned out
to be reporting an intermediate state. The step that actually worked was reading
the post-load path for *writes* instead of theorising about registers.

`FORK-NOTES.md` here is the original milestone-1 plan, kept as written at the
time. Several of its "still to do" items were answered later - notably that the
global `GCTL` reset turned out to be unnecessary, and that its plan to put the
code loader on SD1 is not what happened: SD1 became the capture stream, and the
loader runs on SD7, which is AppleHDA's. SD1 later proved unsafe too, once the
codec layout published a second AppleHDA input engine, and capture now lives on
SD6 (entry 32). See "The borrowed-stream contract" in `ARCHITECTURE.md`.
