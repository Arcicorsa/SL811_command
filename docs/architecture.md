# SL811HS bench architecture

The Due commands the USB host controller over an indexed, parallel eight-bit bus. The SL811HS handles USB signaling and packet handshakes. The firmware supplies token PID, endpoint, device address, packet RAM location, byte count, direction, DATA toggle and arm controls. Both Host-A and Host-B have transfer registers. The connected branch of the Poseidon sequence primes A with an SOF PID and RAM base 0x10, initializes B registers at A offsets plus 8 with zeros, and leaves B idle. Command-driven packets use A.

## Parallel bus

For a write, the Due drives D0–D7, asserts Spare CS low and selects the address phase with A2 low. It drives the address, pulses IOWR low for a programmed 1 µs, and waits 1 µs after raising IOWR. It sets A2 high, drives the value and repeats the write strobe before releasing CS. For a read, the address phase is the same. The Due then switches the data bus to input, raises A2, waits 1 µs, lowers IORD, waits 1 µs, samples the eight pins while IORD remains low, raises IORD, waits 1 µs and releases CS. GPIO calls and per-bit operations add overhead beyond the programmed delays. RAM helpers perform the same indexed cycles for each byte. These are source timings. Actual setup, hold, pulse width and turnaround depend on the fixture and GPIO runtime.

Hardware reset pulses the reset signal low for 10 ms, raises it and waits 10 ms. Startup leaves strobes and CS high and sets the data bus to input.

## Register map used by the commands

| Offset | Write / configuration | Readback used by firmware |
| --- | --- | --- |
| 0x00 | Host-A control | Host-A control |
| 0x01 | Host-A buffer base | Buffer base |
| 0x02 | Host-A requested length | Requested length |
| 0x03 | Host-A PID in upper nibble, endpoint in lower nibble | Packet status |
| 0x04 | Host-A device address | Residual byte count (HOSTTXLEFT) |
| 0x05 | Control 1 | Reset, Low-Speed, and SOF settings |
| 0x06 | Interrupt enable | Enabled sources |
| 0x08–0x0C | Host-B transfer bank | Host-B bank, unused for diagnostics |
| 0x0D | Interrupt status write-one-to-clear | Channel DONE, change and port state |
| 0x0E | SOF low reload | Silicon revision |
| 0x0F | Control 2, including master mode and SOF high settings | SOF high counter |

Interrupt status includes A DONE (0x01), B DONE (0x02), change (0x20), detect (0x40) and Full-Speed indication (0x80). W1C acknowledgment writes a one in the position to clear. Initialization broadly clears pending bits with 0xFF. The transfer path clears A DONE selectively with 0x01. The INT pin is configured as an input and completion uses register polling.

## Poseidon-derived reset and port scan

The comments identify `sl811hs_ResetHW()`, `sl811hs_ResetUSB()`, `sl811hs_PortScan()` and `sl811hs_XferIssue()` as historical references. `POSEIDON` expects revision 0x20, writes Control 2 = 0xAE, clears pending flags, disables IRQs and waits 40 ms. It asserts USB reset through Control 1 = 0x08 for 50 ms, releases it and samples status after clearing pending flags. The code treats bit 0x40 set as disconnected and uses 0x80 to distinguish Full-Speed from Low-Speed on a connected port. The disconnected branch clears detect and conditionally clears all flags again. The connected branch sets host mode, SOF reload and speed controls before initializing A and B. The sequence ends by enabling change and both DONE sources (0x23). A 40-entry trace is buffered and printed after programming. Speed and presence are sampled at command checkpoints. There is no continuous hotplug task.

## Host-A transactions

1. Clear stale A DONE and verify it cleared. For outbound packets, load packet RAM before programming buffer base, length, PID/endpoint and device address. The firmware uses buffer 0x10 and addresses 0 and 1. The EP0 wrapper supplies endpoint 0, while the generalized Host-A helper accepts the endpoint nibble for bulk and interrupt transfers. PID nibbles are SETUP 0xD, IN 0x9 and OUT 0x1.
2. Read SOF high immediately before arm. For Full-Speed the code requests SOF synchronization (control bit 0x20) when the counter is at or below `(length >> 3) + 3`. This follows the erratum workaround cited in the source. Direction OUT uses bit 0x04, DATA1 uses 0x40, and enable/arm use 0x02/0x01. Serial logging is deferred across this timing-sensitive window.
3. Poll status with 100 µs delays for up to 500 ms per attempt and stop early when detect indicates disconnect. On completion, clear A DONE and inspect packet status and residual count. The code decodes ACK, ERROR, TIMEOUT, SEQ, SETUP, OVERFLOW, NAK and STALL. A successful IN requires ACK and the expected sequence. OUT selects its DATA PID in control and checks the ACK. The caller validates the residual before subtracting it from requested length.
4. The EP0 retry wrapper issues at most eight attempts total. NAK waits 200 µs before retrying the same PID and toggle. A missing completion, disconnect or stale-DONE failure aborts the loop. Completed non-ACK/sequence failures without NAK also stop.
5. Control reads start with DATA0 SETUP and DATA1 IN. Subsequent IN packets alternate toggles on successful receipt, ending on a short packet or the requested length, followed by zero-length DATA1 OUT. SET_ADDRESS uses DATA0 SETUP and a zero-length DATA1 IN at address 0, then waits 2 ms before testing address 1. ENUM8 deliberately stops before its status stage.

All commands block the serial loop until they return. ENUM18 consumes the Full-Speed initialization state and stores the descriptor and EP0 packet size only after the address-1 verification. IDENTIFY uses that state for read-only descriptor requests with a 512-byte configuration buffer and up to 16 extra string indices. The legacy MOUSE path has its own reset, packet issue and polling helpers and prints the device descriptor and VID/PID. See [Amiga backend mapping](amiga-backend.md) for how these algorithms fit the native driver.


## Class diagnostics

The shared Host-A helper services EP0, descriptor-selected BOT bulk endpoints
and the receiver-specific HID interrupt endpoint. Bulk NAK retries use elapsed
time and consume a toggle only after an accepted packet. BOT checks the CSW
signature, tag, length, status and residue bounds. The shared SCSI executor
supports no-data, IN and OUT phases. Its read-only RDB diagnostics validate
checksums and bound linked-list walks. The write-verification command leaves
one overwritten media block in place.

HID commands add Boot and Report Protocol control requests, descriptor
decoding and timed reports. The large six-stage capture pools are statically
allocated. Polling and serial command handling are synchronous. The capture
logic delays report printing until after a sampling window.

The bench class diagnostics serve as reference traces. The Amiga HCI adapter
supplies packet I/O and leaves enumeration and class handling to Poseidon.
