# Physical Arduino Due bench verification — November 2026

## Device and access

Windows enumerated COM3 as a USB serial device with VID/PID `2341:003D`
and PnP status OK. Arduino identifies this VID/PID as the Due Programming
Port. It matches the pinned PlatformIO `due` board and is the Windows
port for both upload and Arduino `Serial`. A Windows exclusive open at
115200 baud, 8 data bits, no parity, one stop bit succeeded with DTR and
RTS disabled. The handle was closed after the passive read. No other
process held COM3 during that check.
[Arduino programming-port VID/PID](https://github.com/arduino/Arduino/blob/master/build/windows/dist/drivers/arduino.inf)

PlatformIO under WSL listed `/dev/ttyS0` through `/dev/ttyS7` with no
hardware IDs. `/dev/ttyS3` belongs to the Linux serial8250 driver and
could not be identified as the Due. The session user has no read/write
permission on that root:dialout node. No `ttyACM`, `ttyUSB`, USB serial
symlink, or Linux USB device tree was present. The Windows access test
worked through WSL interoperability, so the Windows upload and monitor
route can use COM3 with the current driver. If using the Linux uploader
instead, WSL 2 requires Windows USB/IP sharing and attachment of the
specific Due USB device, followed by enumeration and permission checks
on the resulting Linux port. No USB attachment, driver or system
configuration was changed.
[Microsoft WSL USB attachment instructions](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)

## Build evidence

Fresh isolated PlatformIO build completed for `due` with Atmel SAM
9.0.0, Arduino SAM 1.6.12, and Arm GCC 7.2.1. It used 58,068 bytes RAM
and 106,144 bytes flash. The only warning was
`-Wmisleading-indentation` in Arduino SAM `USBCore.cpp:688`.
Both artifacts matched the documented [clean-build hashes](test-results.md#restored-final-bench-build-2026-11-20).

| File | SHA-256 |
| --- | --- |
| Restored source and reference sketch | `cd8a23164b45166b7648efe39665aa5e5df3805fcc3809b8c49bc450cb518708` |
| Fresh firmware.bin | `6dd2a0596a0d3f910ee9e4a6c17d9745271a507b789bd5ac0f3319ab5d4ba9f7` |
| Fresh firmware.elf | `595d1754a2800f77b6c4aa525f5a7fb726cd5caf60fcabb80bab1e7ffac8d9e5` |

The source files match byte-for-byte and retain their recorded hashes.
The existing project BIN and ELF were preserved. This is a build check,
separate from physical verification.

## Upload and startup

**PASS.** The owner authorized the standard Due erase, program, verify,
and flash-boot sequence. The Windows uploader and image were accessed
from WSL. PowerShell rechecked COM3's Arduino Programming Port VID/PID
and the full image SHA-256 before the 1200-baud touch. BOSSA identified
device `0x285e0a60`, wrote and verified all 108,736 bytes, selected
flash boot and reset the CPU. Exit code was 0. No separate bootloader,
fuse, lock or security operation was requested. No other image was sent.

The 115200 8N1 monitor captured a partial startup banner and the complete
`Ready. Type HELP.` prompt. The repeated HELP listing and command replies
were intact. The first command immediately after opening the Windows
handle did not consistently produce a reply, so a repeat HELP was used.
There was no repeated startup banner or exception in the observed windows.
The startup capture is reproduced verbatim below apart from line endings.

## Controller and device

The attached Full-Speed device was YICHIP Trust Wireless Mouse
`145F:02CF`, `bcdDevice=0x0002`. It returned one 59-byte configuration
with Boot Keyboard interface 0 and Boot Mouse interface 1. The mouse
endpoint was interrupt IN `0x82`, 64 bytes, 2 ms. Its HID report
descriptor length was 163 bytes.

Read-only register diagnostic `R 0E` returned `0x20`. RESET and
POSEIDON completed, with the buffered trace showing USB reset assertion
for a programmed 50 ms, release, SOF setup and Host-A initialization.
The post-reset port sample was `0x80` (connected Full-Speed). The
trace is firmware observation, not a measurement of electrical timing.

ENUM8 returned eight bytes and learned EP0 size 64. The following
POSEIDON attempt read `0x21` and aborted, so ENUM18 and IDENTIFY correctly
refused to proceed in that attempt. Following hardware RESET, revision
returned to `0x20`. ENUM18 then returned the full 18-byte descriptor,
set address 1, and confirmed the first eight descriptor bytes at the new
address. IDENTIFY completed the string and interface descriptor reads.
The reason for the single `0x21` read is unresolved.

## Initial physical test results

The table records the initial sessions. The later
[confirmed mouse input follow-up](#confirmed-mouse-input-follow-up)
passed the large Report Protocol capture and verified physical input.

| Test | Status | Evidence |
| --- | --- | --- |
| Due identification and serial access | PASS | Programming Port COM3, VID/PID 2341:003D, Windows exclusive open succeeded. |
| Upload and flash verification | PASS | BOSSA verified 108,736 bytes and exited 0. |
| Startup, HELP and parser | PASS | Startup prompt, full 19-command list on retry, unknown-command rejection, and structured register replies. First-command loss after open and partial startup capture noted above. |
| SL811HS revision and controller RESET | PASS | Revision 0x20 before initial setup and again after reset. |
| Controller initialization, USB reset sequence and Full-Speed classification | PASS | Register trace and successful descriptor exchange. One reinitialization aborted at revision 0x21. |
| Low-Speed classification | NOT RUN | No Low-Speed device exercised. |
| ENUM8 | PASS | Eight descriptor bytes, one IN NAK recovered. |
| ENUM18 and IDENTIFY | PASS | Initial prerequisite failures followed by a passing hardware-reset retry and full descriptor inventory. |
| MOUSEIRQ Boot setup and polling | PASS | Configuration and Boot Protocol accepted. 5,896 interrupt polls returned NAK, zero controller errors. Input delivery assessed separately. |
| MOUSEIRQ2 GET_PROTOCOL and SET_IDLE | PASS | Boot Protocol value 0 and 20 ms idle ACK. 1,964 interrupt polls returned NAK, zero controller errors. |
| Boot GET_REPORT(Input ID 0) | FAIL | Endpoint-zero data stage STALL, test continued. |
| MOUSEFEATURES descriptor and Report Protocol | PASS | All 163 bytes decoded; Report Protocol value 1 and SET_IDLE ACK. 1,965 interrupt polls returned NAK, zero controller errors. |
| MOUSEINPUTS compact staged diagnostic | PASS | Six polling windows completed, 11,791 NAK and zero controller errors. Control GET_REPORT returned 32 vendor bytes for ID BA. IDs 01, 02 and 03 stalled. No interrupt input was captured. |
| MOUSEINPUTS2 large staged diagnostic | FAIL | SET_CONFIGURATION stopped on a device disconnect indication (INTSTATUS 0xF1, HOSTSTATUS TIMEOUT). Capture never started. |
| Physical button, movement and wheel response | NOT RUN | No operator action was confirmed during observation windows. No interrupt data reports arrived. |
| BOT discovery, CBW/CSW and SCSI INQUIRY | PASS | Full-Speed General UDisk ABCD:1234, configuration 1, interface 0, Bulk OUT 0x01 and IN 0x81. Maximum LUN 0. INQUIRY diagnostics passed with CSW status 0x00. |
| TUR and READ CAPACITY(10) | PASS | Medium ready. Last LBA 15,728,639 and logical block size 512 bytes. |
| REQUEST SENSE on storage | NOT RUN | TUR succeeded and no tested SCSI command needed a sense request. |
| READ(10) and repeatability | PASS | Two LBA 0 commands returned 512 bytes each. The hex dumps matched byte-for-byte, SHA-256 `443a9b24ec194df548256f3df5e2d5a1d1f2b7a49b483c545eef5f4b2912751f`. No independent reference image was compared. |
| RDSK scan and no-RDB handling | PASS | Both diagnostics read blocks 0–15 without finding a valid RDSK. |
| PART/FSHD traversal of valid RDB | NOT RUN | The UDisk supplied no valid RDSK to follow. |
| MSDWRITE, MSDWRITETEST and other media writes | NOT RUN | Excluded by the test scope. |
| Raw register writes, mount, format, or partition operations | NOT RUN | Excluded by the test scope. |

No Linux block device was mounted. The UDisk was exercised through the
SL811HS fixture and the firmware shell only. Source review established
that the selected storage handlers use INQUIRY, TUR, READ CAPACITY,
READ(10) and, on a suitable failure branch, REQUEST SENSE. They do not
dispatch to the write-verify handler.

## Commands and logs

- Windows preflight used `Get-CimInstance Win32_SerialPort` filtered
  to COM3 and a passive exclusive 115200 8N1 open with DTR and RTS disabled.
  The preflight passive window received no bytes.
- Fresh build used `pio run` with a temporary `PLATFORMIO_BUILD_DIR`.
  Windows upload used the verified Arduino BOSSA 1.6.1-arduino archive
  and a 1200-baud COM3 open/close (DTR transition), followed by
  `bossac.exe --port=COM3 -U false -e -w -v -b firmware.bin -R`.
- Commands sent by session are listed below. The setup traces include
  firmware-issued controller register writes. No shell raw-write command
  was sent.
- [Complete upload and serial capture](physical-bench-test-2026-11-serial.txt)
  contains all sessions, including parser retries, register traces,
  HID output and storage dumps. It preserves the serial text and
  normalizes CR/LF and progress carriage returns for readability.

```text
basic: HELP | BENCH_UNKNOWN | R 0E
init: HELP | RESET | POSEIDON
enum: HELP | HELP | ENUM8 | POSEIDON | ENUM18 | IDENTIFY
enum-retry: HELP | RESET | R 0E | POSEIDON | ENUM18 | IDENTIFY
hid: HELP | MOUSEIRQ | MOUSEIRQ2 | MOUSEFEATURES
staged-hid: HELP | MOUSEINPUTS | MOUSEINPUTS2
port-recheck: HELP | RESET | POSEIDON
reattach: HELP | ENUM18 | IDENTIFY
storage: HELP | MSDINQUIRY | MSDREAD | MSDRDB | MSDRDBINFO
sector-repeat: HELP | MSDREAD
```

Startup capture after BOSSA reset:

```text
====11HS INTERACTIVE TESTER
================================
Ready. Type HELP.
```

## Differences from the historical bench record

The [historical bench record](test-results.md#historical-prototype-bench-results)
reports Full-Speed enumeration, HID input and BOT/SCSI work, including a
512-byte write/read-back test. This run reproduced Full-Speed enumeration,
HID control and descriptor exchanges, and read-only BOT/SCSI traffic.
The initial run did not reproduce physical mouse reports. One large capture stopped
on disconnect, and one reinitialization observed revision 0x21 before
a successful hardware-reset retry. The read-only scope excluded write
verification. A device with a valid RDB and a known Low-Speed device
were not tested.


## HID follow-up

The fixture reported Full-Speed. The first ENUM18 attempt received eight
consecutive NAKs on the initial IN transaction and stopped. A second
hardware RESET and POSEIDON sequence allowed ENUM18 to pass.
The attached VID/PID was `ABCD:1234`, matching the previously identified
General UDisk. The Trust/YICHIP receiver `145F:02CF` was not detected.
The HID follow-up capture was **NOT RUN**. Only HELP, RESET, POSEIDON
and ENUM18 were sent in this follow-up.


## Confirmed mouse input follow-up

After the owner connected the mouse receiver, RESET, POSEIDON and ENUM18
confirmed Full-Speed Trust/YICHIP `145F:02CF`. MOUSEIRQ again completed
Boot setup and polling with zero input reports. MOUSEINPUTS2 then completed
all six Report Protocol capture stages.

| Test | Status | Evidence |
| --- | --- | --- |
| Large staged HID capture | PASS | 11,752 polls, 2,253 ACKed mouse reports, 9,499 NAKs, zero controller errors and zero drops. All 2,253 raw records were captured in the log. |
| X/Y, left/right/middle and wheel | PASS | Nonzero X/Y and wheel samples and press/release edges for buttons 1, 2 and 3 were observed. |
| Boot Protocol input with confirmed stimulus | NOT RUN | Boot polling ran, but no Boot input report was observed. Motion and button delivery were verified in Report Protocol. |
| Side buttons and DPI change | NOT RUN | No side-button edges were observed and the session did not independently validate a DPI change. |

Commands sent: HELP, RESET, POSEIDON, ENUM18, HELP, MOUSEIRQ and
MOUSEINPUTS2. No storage or raw register-write commands were sent.
The full enumeration and capture were appended to the serial evidence.
