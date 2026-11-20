# Test results

## Incomplete public snapshot build, 2026-11-20

Ran `pio run -t clean` followed by `pio run` for the Due Programming Port environment using an isolated PlatformIO cache. System Python was `/usr/bin/python3` 3.14.4. PlatformIO Core 6.2.0 ran from its managed virtual environment with Python 3.14.4. The WSL VS Code server had the Linux x64 PlatformIO IDE extension 3.3.4 installed. The build was verified through Core in the shell. The IDE Build button and serial UI were outside the test.

| Component | Version |
| --- | --- |
| Atmel SAM platform | 9.0.0 (pinned in platformio.ini) |
| Arduino SAM framework | 1.6.12 |
| Arm toolchain package | 1.70201.0 (GCC 7.2.1) |
| CMSIS | 1.40500.0 (4.5.0) |
| CMSIS Atmel | 1.2.2 |
| SCons package | 4.41101.0 |

The build succeeded. PlatformIO reported 3,036 bytes RAM and 53,208 bytes flash used. The Arduino SAM framework emitted a misleading-indentation warning in its USB core. The firmware source was preserved byte-for-byte.

| Artifact | Imported SHA-256 | WSL clean-build SHA-256 |
| --- | --- | --- |
| firmware.bin | `be28ef06968192294e00f33eb568c84e3af4e0d160a3196239314aee26d0105d` | `be28ef06968192294e00f33eb568c84e3af4e0d160a3196239314aee26d0105d` |
| firmware.elf | `52b6c7820d4e77a2a77f639a1530f48f06c67f542a82aaeb98cb009750f735bc` | `b1a6a0ec83ccc9c55081dc6621202ba8498ae2ffff8ce4f316f142f2a7722323` |

The 55,800-byte BIN files compare byte-for-byte. ELF section comparison isolated the changes to the symbol and string tables. The decoded symbol listing changed only the file paths for `crti.o` and `crtn.o` from the Windows toolchain directory to the WSL cache. The ELF shrank from 164,676 to 164,668 bytes.

## Historical prototype bench results

The project owner reported successful SL811HS identification and initialization, Full-Speed bus reset, enumeration, address assignment and descriptor retrieval on an Arduino Due bench setup. No raw serial logs, device inventory, or hardware captures were included with this import. The source contains a buffered register trace for the Poseidon-derived initialization and per-packet traces for enumeration. These can capture register values, poll status, PID/toggle, residual length, attempt counts and NAKs in a repeat run.

The owner also reported Control, Bulk and Interrupt traffic, HID mouse reports with buttons and wheel, BOT endpoint discovery and basic BOT commands, SCSI discovery and capacity, and a one-sector WRITE(10)/READ(10) with a byte-exact 512-byte match in broader bench testing. The initial public import exposed only the endpoint-zero diagnostics. Its MOUSE command reads the device descriptor and VID/PID, and IDENTIFY decodes descriptor metadata including endpoint types. That snapshot had no commands for HID report polling, BOT transfers, or sector I/O. The owner identified the recovered full sketch as the final firmware used for the completed Due bench work.

## Restored final bench build, 2026-11-20

A clean PlatformIO Core 6.2.0 build used the pinned Arduino Due Programming
Port configuration with Atmel SAM 9.0.0, Arduino SAM 1.6.12, CMSIS 4.5.0,
Atmel CMSIS 1.2.2 and Arm GCC 7.2.1 (PlatformIO package 1.70201.0).
The build ran in an isolated output directory. No existing local firmware
artifact was replaced.

| Measurement or artifact | Result |
| --- | --- |
| Static RAM | 58,068 / 98,304 bytes (59.1%) |
| Flash | 106,144 / 524,288 bytes (20.2%) |
| Canonical source and archival sketch SHA-256 | `cd8a23164b45166b7648efe39665aa5e5df3805fcc3809b8c49bc450cb518708` |
| firmware.bin SHA-256 | `6dd2a0596a0d3f910ee9e4a6c17d9745271a507b789bd5ac0f3319ab5d4ba9f7` |
| firmware.elf SHA-256 | `595d1754a2800f77b6c4aa525f5a7fb726cd5caf60fcabb80bab1e7ffac8d9e5` |

RAM and flash counts match the temporary C++ build in the recovery audit
exactly. Both linked artifacts also match it byte-for-byte. The only
compiler warning was `-Wmisleading-indentation` at Arduino SAM
`cores/arduino/USB/USBCore.cpp:688`. It originates in the framework
and was also present in the historical baseline build.

Restoration builds ran without uploading. The subsequent physical session
flashed and verified this image on the Due, then passed Full-Speed
enumeration, descriptor inspection, Report Protocol mouse input and
read-only BOT/SCSI tests. It confirmed identical 512-byte results on two
reads of LBA 0. Read-only RDB scans completed with no valid RDSK on the
test device. See the [physical bench report](physical-bench-test-2026-11.md)
for the session-specific PASS, FAIL and NOT RUN results and serial evidence.

## Still to verify

- Confirm Boot Protocol interrupt input with a controlled physical
  stimulus. The Boot GET_REPORT data phase stalled in the bench test.
- Inspect PART/FSHD on test media with a known valid RDB. Exercise the
  SCSI REQUEST SENSE path with a suitable non-destructive failure case.
- Repeat write verification only in a separately authorized session with
  sacrificial media. A matching read-back would not establish power-cycle
  durability.

- Confirm Low-Speed transfers. The source classifies speed and sets Low-Speed control bits. ENUM8, ENUM18 and IDENTIFY require Full-Speed.
- Capture timed parallel-bus and IRQ traces and repeat insertion/removal checks on the actual fixture. The source samples state at command checkpoints and while polling Host-A.
- Exercise timeout, STALL, NAK exhaustion and different EP0 packet sizes against known devices. ENUM8 is intentionally a partial control transfer, and MOUSE uses separate legacy helpers.
- Validate the native AmigaOS driver, Poseidon 2.2 HCI behavior, root hub and actual clockport hardware independently. Multi-block storage remains unverified.

This audit ran no hardware tests and performed no firmware upload.
