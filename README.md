# SL811_command

Source-available, noncommercial project. An Arduino Due serial bench tool for the SL811HS USB 1.1 host controller. It exercises the parallel bus, resets the controller and USB device, and traces endpoint-zero transactions used during enumeration. It is a hardware reference for aRCiUSB, whose native AmigaOS driver will access the controller through the Amiga clockport.

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
pio device monitor --baud 9600 --eol LF
```

The `due` environment uses Arduino on the SAM3X8E. Build outputs are under `.pio/build/due/`. Open the Due programming serial port used by Arduino `Serial` and add `--port <device>` if several ports are available. The monitor uses 9600 baud, 8N1 and LF or CRLF line endings. The project enables local echo. This procedure assumes the Due already runs the tester firmware.

## Commands

Enter command names entirely in uppercase or lowercase. Register arguments are hexadecimal bytes. Raw writes can change controller state.

| Command | Behavior |
| --- | --- |
| `HELP` | List recognized commands |
| `R XX` | Read byte at address XX, including packet RAM addresses |
| `W XX YY` | Write byte YY at address XX |
| `RESET` | Pulse the controller hardware reset |
| `POSEIDON` | Check revision, reset the USB bus, classify the attached port, set SOF and initialize host channels as applicable, then print the buffered trace |
| `ENUM8` | Diagnose the first SETUP and DATA1 IN stages of an eight-byte device descriptor request at address zero. Leaves the status stage unsent |
| `ENUM18` | Obtain EP0 maximum packet size, read the complete device descriptor, finish the control stages, assign address 1 and verify it with a descriptor read |
| `IDENTIFY` | After a successful ENUM18, inspect strings and configuration, interface and endpoint descriptors at address 1 |
| `MOUSE` | Run the separate legacy Full-Speed reset and device descriptor test, displaying VID/PID |

## Typical session

1. Connect the Due to the SL811HS test fixture and attach a known Full-Speed device directly to the fixture. Use a device that answers standard endpoint-zero descriptor requests. A Full-Speed mouse receiver is suitable. A directly attached Low-Speed mouse will fail the Full-Speed enumeration checks.
2. Open the serial monitor. Use `HELP` to check the shell and `R 0E` to read the silicon revision. The Poseidon path expects `20` hexadecimal.
3. Run `POSEIDON` and check for `CONNECTED / FULL-SPEED`. For a packet-level diagnostic, optionally run `ENUM8`, then issue `POSEIDON` again to start with a fresh bus.
4. Run `ENUM18` and look for `ENUMERATION CHECKPOINT PASSED`. Run `IDENTIFY` to read additional descriptors.
5. Rerun `POSEIDON` before independent enumeration attempts and after resets, swapping devices or raw writes that alter controller state.

The Poseidon diagnostic uses one device, endpoint zero, and Host-A for transactions. Low-Speed classification and register settings exist, but the enumeration commands require Full-Speed. IDENTIFY reads descriptor metadata without configuring a device. MOUSE identifies a device by descriptors and does not read HID reports. The command set has no hub management, endpoint scheduler, or mass-storage transfers.

See [Architecture](docs/architecture.md), [Amiga backend mapping](docs/amiga-backend.md), and [Test results](docs/test-results.md).

## Licensing

The project makes its source publicly available for inspection and local builds. The original project software identified in [NOTICE](NOTICE) is available under the [PolyForm Noncommercial License 1.0.0](LICENSE). The license permits personal hobby use, experimentation, education, modification, and noncommercial redistribution under its terms. This project uses a source-available noncommercial license outside the OSI-approved open-source model. In the firmware, BSD-2-Clause attribution covers the PortScan disconnect fallback and the Full-Speed SOF threshold and comparison. ResetHW and ResetUSB are behavioral references. The PlatformIO scaffold retains Apache-2.0 terms. See the [third-party notices](THIRD-PARTY-NOTICES.md) for the exact scope.

Any original hardware design files expressly listed in `hardware/NOTICE` are available under `hardware/LICENSE-HARDWARE.txt`. These custom noncommercial terms fall outside the Open Source Hardware Association definition. They permit personal and community fabrication, group PCB orders, free redistribution with attribution, and reimbursement of documented direct external production and shipping costs without markup. They allow a paid manufacturer to fulfill a qualifying order under limited conditions. Commercial production and sales, product lines, and stocking for commercial resale require separate permission.

The initial distribution consists of source files. Build the firmware locally using the instructions above. Release BIN and ELF images are withheld pending review of the linked Arduino components and LGPL redistribution obligations.
