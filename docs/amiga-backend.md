# Mapping the bench tester to the Amiga backend

| Arduino reference | Reusable controller behavior | Production driver responsibility |
| --- | --- | --- |
| `slReadRegister`, `slWriteRegister`, `slReadRAM`, `slWriteRAM` | Address selection, read/write aliases and indexed packet RAM | Implement and serialize the clockport address/data cycles using the board's MMIO mapping and timing. Verify access width, ordering and strobe behavior on the target. |
| `slReset`, `poseidonInitialization` | Revision check, host mode, reset ordering, SOF setup, port sampling and channel defaults | Supply board reset and timers, manage operational state, and report port status and change events through the HCI adapter. |
| `enumIssueChannelA` | Load payload, program base/length/PID/address, evaluate SOF synchronization, arm, check completion and decode status/residual count | Accept endpoint/address/speed and direction as parameters, allocate packet RAM, retain endpoint toggles and handle interrupt or polling completion with calibrated deadlines. |
| `enumIssueChannelA_with_retry` | NAK repeats the same packet/toggle. STALL, timeout, disconnect and sequence checks provide distinct diagnostic outcomes. | Schedule retries and periodic endpoints according to transfer policy. Handle cancellation, flush, disconnect and late completions with a single completion per request. |
| `poseidonEnum8`, `poseidonEnum18SetAddress`, `poseidonIdentify` | Transaction traces and expected EP0 stage ordering provide diagnostic checkpoints. | Poseidon 2.2 owns enumeration and USB class configuration. The HCI V1 adapter translates requests into a controller core that uses no Arduino or Poseidon structures. |

The Due's delay values describe its test fixture and software path. Measure and validate the clockport timing, IRQ handling, Low-Speed transfers and physical port behavior independently.
