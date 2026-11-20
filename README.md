# SL811_command

Source-available, noncommercial project. An Arduino Due serial bench tool for the SL811HS USB 1.1 host controller. It exercises the parallel bus, USB enumeration, HID reports, USB mass storage, and Amiga Rigid Disk Block (RDB) inspection. It is a hardware reference for aRCiUSB, whose native AmigaOS driver will access the controller through the Amiga clockport.

The final bench firmware builds from `src/main.cpp`. The [original sketch](reference/SL811HS_Poseidon_Due_MSD_WRITE_VERIFY.ino) is a byte-identical archive outside the build source directory.

## Bench wiring

The pin assignments and levels below describe what the firmware does at the Due GPIO pins. Consult the prototype schematic to establish connector pinout, voltage compatibility, bus buffering, grounding, and USB VBUS supply.

| Signal | Due pin | Firmware use |
| --- | ---: | --- |
| Spare CS | 24 | Low across address and data phases, high when idle |
| INT6 | 26 | Input. Firmware polls controller status over the bus |
| IORD | 28 | Low during data sampling |
| IOWR | 30 | Low to strobe the selected address or write data |
| A2 | 32 | Low for address, high for data |
| RESET | 50 | Low for 10 ms, then high with a 10 ms settle delay |
| D0–D7 | 48, 46, 44, 42, 40, 38, 36, 34 | Bidirectional, in bit order |

These assignments apply to the Arduino fixture. The Amiga clockport board requires its own address decoding, access timing, reset and interrupt handling.

## Build and monitor

Use Python 3 and PlatformIO Core, or install the recommended PlatformIO IDE extension in the VS Code WSL window and open a PlatformIO terminal. For Core on its own, install with `pipx install platformio` and ensure the executable is on your PATH. The first build downloads the Atmel SAM platform, the Arduino SAM framework, and the Arm toolchain.

```sh
pio --version
pio run
pio device list
pio device monitor --baud 115200 --eol LF
```

The `due` environment uses Arduino on the SAM3X8E. Build outputs are under `.pio/build/due/`. Open the Due programming serial port used by Arduino `Serial` and add `--port <device>` if several ports are available. The monitor uses 115200 baud, 8N1 and LF or CRLF line endings. The project enables local echo. This procedure assumes the Due already runs the tester firmware.

## Commands

Enter command names entirely in uppercase or lowercase. Register arguments are hexadecimal bytes. Raw writes can change controller state.

| Command | Alias | Behavior |
| --- | --- | --- |
| `HELP` | | List primary commands. |
| `R XX` | | Read a register or packet RAM byte. |
| `W XX YY` | | Write a register or packet RAM byte. |
| `RESET` | | Pulse the SL811HS hardware reset. |
| `POSEIDON` | | Check revision, reset the USB bus, classify the port, initialize host channels, and print the buffered trace. |
| `ENUM8` | | Trace the first SETUP and DATA1 IN stages of an eight-byte device descriptor read at address zero. Leaves the status stage unsent. |
| `ENUM18` | | Read the complete device descriptor, finish control stages, assign address 1, and verify it. |
| `IDENTIFY` | | Read strings and configuration, interface, and endpoint descriptors at address 1. |
| `MOUSE` | | Legacy Full-Speed descriptor and VID/PID test. |
| `MSDINQUIRY` | `MSD` | Find and configure a SCSI-transparent BOT interface, query maximum LUN, and print INQUIRY data and status. |
| `MSDREAD` | `MSDSECTOR` | Test readiness, decode sense data on failure, query capacity, and dump one logical block at LBA 0 in hex and ASCII. |
| `MSDWRITE` | `MSDWRITETEST` | **Destructive.** Write one patterned logical block near the middle of LUN 0 and compare every byte after reading it back. |
| `MSDRDB` | `RDB` | Scan LBA 0–15 for a checksum-valid RDSK and print geometry and list pointers. Read-only on media. |
| `MSDRDBINFO` | `RDBINFO` | Follow and decode bounded PART and FSHD lists. Read-only on media. |
| `MOUSEIRQ` | | Set Boot Protocol and capture Interrupt IN reports for up to 12 seconds. |
| `MOUSEIRQ2` | | Run GET_PROTOCOL, GET_REPORT and SET_IDLE(20 ms), then capture Boot Protocol Interrupt IN reports. |
| `MOUSEFEATURES` | | Decode the HID report descriptor and capture raw Report Protocol Input reports. |
| `MOUSEINPUTS` | | Read input report IDs and run six compact capture stages for primary and side buttons, motion, wheel, pan, and special controls. |
| `MOUSEINPUTS2` | | Run six independent large captures for X/Y, left/right/middle buttons, wheel, and manual DPI comparison. |

## Typical session

1. Connect the Due to the SL811HS test fixture and attach one known Full-Speed device directly to its USB port. Verify fixture voltage compatibility, bus wiring and VBUS supply before use. A directly attached Low-Speed mouse will fail the Full-Speed enumeration checks.
2. Open the programming-port serial monitor at 115200 baud. Run `HELP` and use `R 0E` to check the silicon revision. The Poseidon path expects hexadecimal `20`.
3. Run `POSEIDON` and check for `CONNECTED / FULL-SPEED`. If you run the optional partial `ENUM8` trace, rerun `POSEIDON` afterward.
4. Run `ENUM18` and look for `ENUMERATION CHECKPOINT PASSED`. Run `IDENTIFY` for the descriptor inventory, then select the applicable storage or HID diagnostic.
5. Rerun `POSEIDON` and `ENUM18` for a fresh session after swapping devices, resetting the controller or changing controller state through raw register writes.

## Storage and HID limits

Storage discovery selects alternate setting zero of a Full-Speed interface with
class/subclass/protocol `08/06/50` and valid Bulk IN and OUT endpoints. The BOT
diagnostics issue CBW, data and CSW phases with separate endpoint DATA toggles.
The supported SCSI tests are INQUIRY, TEST UNIT READY, REQUEST SENSE,
READ CAPACITY(10), READ(10) and WRITE(10). They use LUN 0, exercise one-block
I/O, and have no READ CAPACITY(16), full BOT reset recovery or filesystem mounting.
The RDB tests inspect metadata and report LSEG pointers without loading filesystem
payloads.

**Use sacrificial media for both write-command spellings.** `MSDWRITE` and
`MSDWRITETEST` overwrite one logical block at half the reported last LBA on
LUN 0. They proceed without an interactive confirmation and leave the pattern in
place. The midpoint can contain data or metadata. A failed write or read-back
can still leave the medium modified. Keep valuable media disconnected and
verify the target before issuing either command. Run read-only diagnostics first.

The HID tests use configuration 1, interface 1 and Interrupt IN endpoint
`0x82` with a 2 ms polling target. The staged decoders target the
Trust/YICHIP receiver `145F:02CF`. The Boot diagnostics may continue with
that fixed mapping after warning about a different receiver. `MOUSE`
remains a separate descriptor-only test. The bench uses one device,
Host-A, and synchronous serial commands. It has no hub support,
automatic mounting, general HID driver, or continuous hotplug task.
Low-Speed classification does not imply Low-Speed enumeration support.

## Verified on the physical Due

The restored image was flashed and verified. The bench reproduced SL811HS
revision 0x20, Full-Speed enumeration and descriptor inspection. The
Trust/YICHIP receiver passed all six large Report Protocol capture stages
with 2,253 input reports, zero controller errors and zero drops. The
capture confirmed X/Y movement, left/right/middle button edges and wheel
activity. The read-only BOT session passed INQUIRY, TEST UNIT READY and
READ CAPACITY(10). Two READ(10) requests returned identical 512-byte
blocks at LBA 0.

Boot Protocol control setup passed, but Boot GET_REPORT stalled and Boot
interrupt input was not demonstrated. The tested storage medium had no
valid RDSK in blocks 0–15, so PART/FSHD traversal remains unverified on
this build. Low-Speed classification, the SCSI REQUEST SENSE branch,
and destructive write verification were not exercised in this run.

See the [physical bench report](docs/physical-bench-test-2026-11.md),
[complete serial capture](docs/physical-bench-test-2026-11-serial.txt),
[architecture](docs/architecture.md), [Amiga backend mapping](docs/amiga-backend.md),
[build results](docs/test-results.md), and [recovery audit](docs/recovered-full-sketch-audit.md).

## Licensing

The project makes its source publicly available for inspection and local builds. The original project software identified in [NOTICE](NOTICE) is available under the [PolyForm Noncommercial License 1.0.0](LICENSE). The license permits personal hobby use, experimentation, education, modification, and noncommercial redistribution under its terms. This project uses a source-available noncommercial license outside the OSI-approved open-source model. In the firmware, BSD-2-Clause attribution covers the PortScan disconnect fallback and the Full-Speed SOF threshold and comparison. ResetHW and ResetUSB are behavioral references. The PlatformIO scaffold retains Apache-2.0 terms. See the [third-party notices](THIRD-PARTY-NOTICES.md) for the exact scope.

Any original hardware design files expressly listed in `hardware/NOTICE` are available under `hardware/LICENSE-HARDWARE.txt`. These custom noncommercial terms fall outside the Open Source Hardware Association definition. They permit personal and community fabrication, group PCB orders, free redistribution with attribution, and reimbursement of documented direct external production and shipping costs without markup. They allow a paid manufacturer to fulfill a qualifying order under limited conditions. Commercial production and sales, product lines, and stocking for commercial resale require separate permission.

The initial distribution consists of source files. Build the firmware locally using the instructions above. Release BIN and ELF images are withheld pending review of the linked Arduino components and LGPL redistribution obligations.
