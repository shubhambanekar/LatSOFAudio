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

The gap between 13 and 14 is the instructive part. 13 was a confident theory
about clock drift that a profiler disproved in a single command; the real cause
was two registers away, in what looked like a different subsystem entirely.

`FORK-NOTES.md` here is the original milestone-1 plan, kept as written at the
time. Several of its "still to do" items were answered later - notably that the
global `GCTL` reset turned out to be unnecessary.
