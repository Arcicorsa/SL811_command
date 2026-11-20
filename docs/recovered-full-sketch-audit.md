# Recovered full-sketch audit

This audit records the comparison and pre-import experiments. The firmware
is now restored byte-for-byte in `src/main.cpp`, with the original
[archived sketch](../reference/SL811HS_Poseidon_Due_MSD_WRITE_VERIFY.ino)
outside the build directory. Arcicorsa confirmed ownership of the recovered
BOT, HID, RDB and diagnostic implementation. See the [restoration build
record](test-results.md#restored-final-bench-build-2026-11-20) and
[current licensing scope](../NOTICE).

The recovered sketch retains all nine public command handlers and all 49 public
top-level functions. It adds **58 functions, ten commands and five aliases**.
Five existing functions change. The biggest compatibility change for existing
users is the serial baud rate. The biggest operational change is a command that
writes a real USB storage block without an interactive confirmation.

## Inputs and comparison method

- Baseline is [public `main` at `449f2301`](https://github.com/Arcicorsa/SL811_command/tree/449f2301d85998e22a59c8460167a54965b151f5). The fetched `src/main.cpp` matches the local tracked file byte-for-byte. It has 2,264 newline characters. Line counts that include an editor's final empty line may show 2,265.
- The local, untracked `SL811HS_Poseidon_Due_MSD_WRITE_VERIFY.ino` has 6,618 newline characters and SHA-256 `cd8a23164b45166b7648efe39665aa5e5df3805fcc3809b8c49bc450cb518708`. References to recovered line numbers below use that exact snapshot.
- The public C++ source SHA-256 is `2f9df0689087c7d7ab0014bb7a4767dcf8de47d8e1d2f0721f82acf61f51e91a`. The comparison enumerated top-level definitions, compared function text after normalizing line endings, and checked the remaining file-level diff. The functions and commands below are a source inventory. They do not imply that all diagnostics were exercised on hardware.
- Both inputs and the project's existing firmware artifacts were preserved. Build experiments ran on isolated copies without any upload.

## Feature matrix

Commands accept uppercase or lowercase spellings. The five additional aliases
are listed alongside their primary commands.

| Feature or command | Public snapshot | Recovered sketch |
| --- | --- | --- |
| `R`, `W` | Raw register reads and writes | Same command behavior and bus implementation. Raw writes can affect controller state. |
| `RESET` | Hardware pulse and state reset in `slReset` | Same. |
| `POSEIDON` | Buffered revision check, host initialization, bus reset, speed/presence scan and channel setup | Same implementation, including Full-Speed readiness gate for enumeration. |
| `ENUM8` | Partial EP0 device-descriptor exchange | Same diagnostic and fixed retry wrapper. Uses the generalized transfer primitive through an EP0 wrapper. |
| `ENUM18` | Initial eight bytes, full device descriptor, SET_ADDRESS and address 1 verification | Same sequence and state tracking. |
| `IDENTIFY` | Read strings and device/configuration/interface/endpoint descriptors | Same descriptor-only path. It does not configure the device or prove bulk or interrupt transfers. |
| `MOUSE` | Legacy Full-Speed descriptor and VID/PID test | Same legacy test, distinct from the five new HID commands. |
| `HELP` | Nine commands documented | All 19 primary commands documented, including the destructive warning. Aliases exist in dispatch. |
| General control requests | Descriptor-specific EP0 helper and legacy tests | Adds no-data control OUT and multi-packet control IN helpers. Logs SETUP, data and status phases, validates lengths and toggles, and handles short IN data. |
| General Host-A endpoints | Poseidon-style path hardcodes EP0 | Four-bit endpoint selection enables bulk and interrupt traffic. The hardware, packet ordering, DONE poll and SOF decision remain shared. |
| `MSDINQUIRY` / `MSD` | Descriptor metadata only | Discovers a Full-Speed SCSI-transparent BOT interface, configures it, queries maximum LUN and exercises a standalone CBW, 36-byte INQUIRY response and CSW path. Prints identity and status fields. |
| `MSDREAD` / `MSDSECTOR` | Absent | TEST UNIT READY, REQUEST SENSE on command failure, bounded retry for medium-change Unit Attention, READ CAPACITY(10), READ(10) of LBA 0 and hex/ASCII dump. |
| `MSDWRITE` / `MSDWRITETEST` | Absent | Destructive one-block WRITE(10), READ(10) of the same LBA, byte comparison and mismatch reporting. Details and test constraints below. |
| `MSDRDB` / `RDB` | Absent | Read-only LBA 0–15 scan, bounded by disk end. Checks RDSK signature, SummedLongs and sum-to-zero, prints geometry and list heads, and dumps the matching block. Distinguishes missing signature from an invalid checksum. |
| `MSDRDBINFO` / `RDBINFO` | Absent | Finds a valid RDSK, checks its block size, and walks and decodes PART and FSHD lists. Each walk is capped at 16 entries and checks cycles, bounds, signatures and checksums. It prints LSEG pointers without reading or loading their payloads. |
| `MOUSEIRQ` | Absent | Configures the receiver, sets Boot Protocol, attempts SET_IDLE and polls Interrupt IN for 12 seconds. Counts polls, ACKs, NAKs and errors. Captures up to 48 reports and decodes buttons, signed X/Y and an optional fourth byte. |
| `MOUSEIRQ2` | Absent | Boot Protocol plus GET_PROTOCOL, GET_REPORT(Input ID 0) and SET_IDLE(20 ms) diagnostics. Polls for four seconds, captures up to 48 reports, and prints control and polling results separately. |
| `MOUSEFEATURES` | Absent | Requests the known receiver's 163-byte HID report descriptor into a 192-byte buffer, prints raw bytes and decodes Input/Output/Feature descriptor items. Switches to Report Protocol, verifies it, attempts SET_IDLE and captures up to 24 raw Interrupt IN reports over four seconds. It does not perform a Feature-report transfer. |
| `MOUSEINPUTS` | Absent | Compact receiver-specific diagnostic. Checks Input report IDs 01, 02, 03 and BA via GET_REPORT, requests SET_IDLE(0), then runs six four-second windows for X/Y and primary buttons, side buttons, wheel, AC Pan, system/media and remaining controls. Holds up to 80 reports across the six windows and prints decoded data and counters. |
| `MOUSEINPUTS2` | Absent | Six independent four-second captures for X/Y, left, right, middle, vertical wheel and DPI/movement. Each stage stores up to 768 eight-byte mouse records and 64 special records of up to 32 bytes, plus report counts, dropped/oversize/empty records, movement statistics and button edges. DPI is a manual observation stage, not a command to program the receiver. |
| Serial monitor | 9600 baud | 115200 baud. CR is ignored, LF dispatches, and the 64-byte command buffer is unchanged. Diagnostics block the command loop while active. |
| Scope | A single connected Full-Speed device for Poseidon diagnostics | Still one device at address 1 and Host-A. No generic HID driver, hub stack, automatic mass-storage mount or general-purpose block driver is added. |

## Changes to existing implementation

| Public definition | Recovered change |
| --- | --- |
| `enumIssueChannelA` (560 → 652) | Becomes an EP0 compatibility wrapper around new `enumIssueChannelAEndpoint` (561). The register PID encoding now accepts `endpoint & 0x0F`. The OUT preload, SOF check, final arm, completion waits and ACK/sequence rules are retained. |
| `EnumTransferResult` (489) | Attempts and NAK counters widen to 32 bits. `attemptLimit` distinguishes the control retry cap from elapsed-time bulk retries. The existing eight-attempt control retry helper is unchanged. |
| `enumPrintTransferResult` (693 → 711) | Prints wide counters and either the fixed limit or a time-limited label. Existing per-transfer status reporting remains. |
| `printHelp` (2081 → 6345) and `processCommand` (2098 → 6372) | Add all ten commands and dispatch aliases. Existing handlers still dispatch to the same functions. |
| `setup` (2208 → 6562) | Only the baud rate changes, from 9600 to 115200. GPIO mapping, initialization, reset and prompt remain. |

The other 44 function definitions match after line-ending normalization.
They include parallel bus/RAM access, reset and Poseidon logging, all existing
enumeration and descriptor helpers, the legacy mouse test and the shell loop.
There are no removed definitions, commands or diagnostic paths in the source
diff. Timing and device behavior still need regression testing after migration.

## Storage and HID implementation details

- **BOT discovery and wire protocol.** Configurations are scanned using bounded
  descriptor reads up to 512 bytes, selecting alternate setting 0 with class
  08, subclass 06 and protocol 50, validated bulk IN/OUT addresses and Full-Speed
  packet sizes of 8, 16, 32 or 64 bytes. No storage VID/PID allowlist is used.
  SET_CONFIGURATION initializes independent Bulk IN and OUT toggles at DATA0.
  GET_MAX_LUN permits the documented STALL fallback and checks values up to 15.
  The test paths use LUN 0. The transport packs 31-byte CBWs, chunks the optional
  data stage, and validates 13-byte CSW length, signature, tag, status and
  residue bounds. An in-range residue inconsistent with the observed data
  produces a warning. IN can end on a short packet. NAK retries use elapsed
  time, normally 1.5 s, and a 10 s CSW retry window in the shared executor.
  Individual attempts use the inherited controller timeout, so the retry
  window is not a hard wall-clock cap. STALL, toggle mismatch, timeout or
  phase errors are diagnosed without full BOT reset recovery or endpoint
  CLEAR_FEATURE recovery. [USB-IF Bulk-Only Transport 1.0](https://www.usb.org/document-library/mass-storage-bulk-only-10)
- **SCSI coverage.** INQUIRY (12h), TEST UNIT READY (00h), REQUEST SENSE (03h),
  READ CAPACITY(10) (25h), READ(10) (28h) and WRITE(10) (2Ah). REQUEST SENSE
  recognizes fixed responses 70h/71h and descriptor responses 72h/73h,
  reports sense key and ASC/ASCQ, and dumps raw bytes. The readiness retry
  specifically covers 06/28/00, with up to two retries in the write path.
  Capacity fields and CDB LBAs use big-endian order, while BOT tag, length
  and residue fields use little-endian order. Sector helpers use buffers up
  to 4096 bytes. Multi-packet transfer of one sector does not constitute
  multi-sector SCSI I/O. READ CAPACITY(16) is not implemented.
- **RDB coverage.** PART output includes flags, BSTR name, device flags,
  DosEnvec fields, boot priority, DosType and geometry-derived LBA estimates.
  FSHD prints type/version, flags, patch fields and the first LSEG pointer.
  BLOCK geometry is inspection output and does not mount filesystems. RDB
  scans require supported longword-aligned logical sizes, and list traversal
  rejects a mismatch between the RDSK block size and SCSI block size.
  The PART geometry code's 512-byte assumption is present in a comment,
  but the LBA arithmetic does not explicitly gate on 512-byte media.
  Treat unusual geometry and non-512-byte estimates as diagnostic output
  until tested. [AmigaOS SCSI and RDB documentation](https://wiki.amigaos.net/wiki/SCSI_Device)
- **HID coverage.** The experiments use configuration 1, interface 1, EP
  0x82 and a two-millisecond poll target with up to 64 bytes per packet.
  The two staged tests reject devices other than Trust/YICHIP 145F:02CF.
  The Boot diagnostics warn on a different receiver but still use the
  fixed mapping. The descriptor display handles short/long item framing,
  usage pages and ranges, logical/physical ranges, units, collections,
  global PUSH/POP and per-ID input/output/feature lengths. The report
  decoders cover button bits, signed X/Y, wheel, AC Pan, system and consumer
  controls and raw vendor report BA. NAK is normal, and the capture paths
  retain the expected toggle across NAK. There is no vendor-specific OUT
  report in the staged tests. [USB-IF HID specifications](https://www.usb.org/hid)

## Destructive write test

`MSDWRITE` and `MSDWRITETEST` invoke the same path immediately after the
serial command is parsed (6521–6527). The handler requires verified address 1,
compatible BOT discovery, configuration, GET_MAX_LUN, readiness on LUN 0,
and a successful eight-byte READ CAPACITY(10) result. It checks a minimum
last LBA of 128 and a supported nonzero block length up to 4096 bytes.
It then overwrites exactly one block at `lastLba / 2` with a deterministic
pattern, reads it back and compares all bytes. The midpoint can contain
filesystem data or metadata. The firmware does not verify that the medium
is blank, identify it to the operator for confirmation, back up the old
block, or restore it. The capacity check does not reject the READ CAPACITY(10)
`0xFFFFFFFF` sentinel in this path. There is no explicit cache flush or
power-cycle verification. A transport failure can leave the old block
partially or fully replaced.

For bench use, isolate a single blank or disposable device with expendable
data, verify it is the intended medium, and keep all valuable media
disconnected from the fixture. Run all read-only tests first. Treat a
failed write or read-back as possible media modification. Before making
the destructive operation readily available to others, make an explicit
opt-in and target/LBA confirmation a separately approved safety change,
with a sentinel guard and honest residue reporting. Keep that change out
of the behavior-preserving import so the recovered baseline remains
reviewable. The raw register `W` diagnostic also remains a low-level
operator tool.

## Complete inventory of functions absent from public main

The entries give recovered start lines. Together they enumerate all 58
new definitions, including helpers with no serial command of their own.

### Shared Host-A and control helpers

- `enumIssueChannelAEndpoint` (561)
- `enumControlNoData` (1791)
- `enumControlIn` (1839)

### BOT discovery, wire encoding and bulk I/O

- `msdPrintLe32` (1975)
- `msdPrintAsciiField` (1987)
- `msdValidFullSpeedBulkPacketSize` (2000)
- `msdFindBotInterface` (2006)
- `msdBulkTransferWithRetry` (2186)
- `msdBulkOutBuffer` (2232)
- `msdBulkInBuffer` (2268)
- `msdGetMaxLun` (2327)
- `msdStoreLe32` (2404)
- `msdReadBe32` (2411)
- `msdPrintUint64` (2418)

### SCSI executor, sense, dump, read and write-verify

- `msdBotExecuteScsi` (2432)
- `msdSenseKeyName` (2591)
- `msdRequestSenseAndPrint` (2606)
- `msdPrintSectorHexDump` (2690)
- `poseidonMassStorageReadFirstSector` (2735)
- `poseidonMassStorageWriteVerifyTest` (2955)

### RDB scan, validation, block reads and PART/FSHD inspection

- `poseidonMassStorageFindRdb` (3194)
- `msdRdbPrintHex32` (3477)
- `msdRdbPrintFourCC` (3483)
- `msdRdbValidateBlock` (3496)
- `msdRdbReadBlock` (3544)
- `msdRdbPrintBstrName` (3604)
- `msdRdbPrintDosType` (3615)
- `msdRdbPrintPartition` (3619)
- `msdRdbPrintFileSysHeader` (3737)
- `msdRdbReadAndValidateListBlock` (3767)
- `poseidonMassStorageRdbDetails` (3798)

### Standalone BOT INQUIRY

- `poseidonMassStorageInquiry` (4049)

### Boot and control/interrupt mouse diagnostics

- `enumPrintBootMouseReport` (4233)
- `poseidonMouseInterruptTest` (4274)
- `poseidonMouseInterruptDiagnostics` (4459)

### HID descriptor decoder and Report Protocol diagnostic

- `hidReadUnsigned` (4685)
- `hidReadSigned` (4691)
- `hidPrintUsage` (4700)
- `hidPrintUsageSet` (4734)
- `hidPrintMainFlags` (4762)
- `hidParseReportDescriptor` (4772)
- `enumPrintRawHidReport` (4927)
- `poseidonMouseFeatures` (4939)

### Compact staged receiver diagnostics

- `mouseReadS16LE` (5120)
- `mouseReadS8` (5125)
- `mousePrintConsumerUsage` (5129)
- `mousePrintDetailedReport` (5159)
- `mouseGetInputReport` (5255)
- `mousePollInputStage` (5280)
- `poseidonMouseInputTests` (5356)

### Large per-stage capture, decoding and summaries

- `mouseInputs2ResetStage` (5586)
- `mouseInputs2Accumulate` (5625)
- `mouseInputs2CapturePacket` (5680)
- `mouseInputs2PollStage` (5716)
- `mouseInputs2PrintRaw` (5786)
- `mouseInputs2PrintStageData` (5820)
- `mouseInputs2PrintSummary` (5842)
- `poseidonMouseInputTests2` (5881)

The main new supporting types are `MsdBotInterface`, `HidGlobalState`,
`MouseInputs2MouseRecord`, `MouseInputs2SpecialRecord` and
`MouseInputs2Stage`. The new static `mouseInputs2Stages` array is present
even when the large capture command is idle. The constants for class
requests, BOT signatures/timeouts, scan limits, receiver mappings and
capture limits are part of the corresponding groups above.

## Provenance and licensing review

- The existing PortScan disconnect fallback stays at recovered lines
  382–386. The previously reviewed SOF logic moves into
  `enumIssueChannelAEndpoint` at recovered lines 608–611 and 618–619.
  Moving it to a general-purpose function does not extend BSD attribution
  to the function's other code or its new callers. ResetHW and ResetUSB
  remain credited as behavioral references. Keep the full existing
  BSD-2-Clause text and update the applicable file and line references at
  import. The matching Poseidon reference remains v0.1.0 at
  `0fec4f8d9ebcd515fbcbe92e7e9b2dc166bf5e6c`. The revision originally
  supplied has not been established. [Existing provenance report](licensing-provenance.md)
  · [Pinned Poseidon reference](https://github.com/ezrec/poseidon-sl811hs/blob/0fec4f8d9ebcd515fbcbe92e7e9b2dc166bf5e6c/src/sl811hs.c)
- The added endpoint-nibble parameter implements a controller register
  format. New bulk and HID paths reuse the local transfer helper and
  add bench-oriented serial polling and reporting. BOT wrapper layouts,
  SCSI opcodes, HID item fields and RDB offsets are evidence of format
  compatibility. They do not by themselves identify copied source.
  The added block has no third-party copyright banner or new external
  includes. The matching upstream repository also contains a target-side
  BOT simulator, whose CBW/CSW signatures and GET_MAX_LUN selector match
  the protocol. The recovered sketch builds and sends wrappers from the
  host side and has a separate discovery/read/write/reporting flow.
  [Pinned simulator](https://github.com/ezrec/poseidon-sl811hs/blob/0fec4f8d9ebcd515fbcbe92e7e9b2dc166bf5e6c/src/massbulk_sim.c)
  Source inspection identified no additional matching Poseidon
  function body or positive evidence of a newly incorporated third-party
  implementation. This does not establish independent authorship.
  Ask the owner to confirm provenance for the newly recovered code,
  especially the BOT transport, HID parser and RDB offsets/decoder, and
  compare any identified inputs before extending PolyForm to those portions.
- The sketch has the same Arduino and standard-library includes and
  compiles with the same pinned toolchain. Keep PlatformIO scaffold
  attribution and the third-party dependency inventory. Withhold linked
  firmware images while Arduino SAM LGPL and runtime obligations remain
  under review.

## PlatformIO feasibility

The two experiments used copies of the existing configuration with the
Atmel SAM 9.0.0 platform, Arduino SAM 1.6.12, and GCC 7.2.1 for
`env:due`. Neither experiment modified the repository's source or build
directory.

| Experiment | Result |
| --- | --- |
| Put the unmodified recovered `.ino` in `src/` as the only firmware input | Fails under PlatformIO's Arduino prototype generation. Generated declarations precede `EnumTransferResult`, `MsdBotInterface`, HID capture types and constants. |
| Compile the exact recovered bytes as temporary `src/main.cpp`, with no `.ino` in that project | Builds and links without source edits. PlatformIO reports 58,068 bytes RAM (59.1%) and 106,144 bytes flash (20.2%). The BIN is 108,736 bytes. The Arduino SAM USB core emits the previously observed misleading-indentation warning. |
| Existing public baseline | Earlier clean build reported 3,036 bytes RAM and 53,208 bytes flash. See [test results](test-results.md). |

The linked `mouseInputs2Stages` symbol alone occupies 55,032 bytes.
The write-verification path also uses two 4096-byte block arrays on the
stack. Static memory usage is not a measurement of peak stack use.
This validates the C++ import mechanically. It cannot establish
identical machine code to an unavailable historical recovered-sketch
binary, or equivalent electrical timing and device behavior. The reported
historic 512-byte write/read-back pass and HID reports are prior bench
evidence. No test was rerun on physical hardware in this audit.

## Safe migration plan

1. Preserve public `main` and its licensing history. Retain the exact
   recovered file and checksum as local reference evidence. After owner
   approval, prepare an additive restoration branch from current public
   main. Keep the import diff and any subsequent safety changes reviewable
   as separate commits, with no force-push or history replacement.
2. Use the recovered bytes as the single C++ translation unit selected by
   the PlatformIO build. Avoid compiling the public C++ and recovered
   sketch together because they both define the same entry points and
   helpers. If the original `.ino` is retained as an archive, keep it
   outside the build inputs and document which file is canonical.
   Update the monitor setting and documentation to 115200 baud, retain
   the existing wiring, and document commands and exact alias spellings.
3. Update NOTICE and source-range references after ownership review.
   Preserve the existing scoped BSD and Apache terms. Keep build
   artifacts ignored and the eventual distribution source-only.
4. Clean-build the baseline and restored versions in isolated directories.
   Preserve separate hashes and compare linker memory figures. Treat the
   large HID capture pools and nested storage buffers as explicit RAM
   and stack test cases. Hash the archived source and the imported C++
   file to prove the import itself introduced no source edits.
5. On the bench, with separate upload approval, check revision, raw
   register access, reset, `POSEIDON`, `ENUM8`, `ENUM18`, `IDENTIFY` and
   legacy `MOUSE` before exercising the restored commands. Capture
   controller/packet status, retry counters, ready/not-ready and
   disconnect behavior. Test the five HID commands on the known
   receiver, including overflowing capture pools. Use expendable or
   read-only media for INQUIRY, LBA 0 and RDB tests. Cover valid,
   absent and corrupt RDBs, short reads and link-cycle handling without
   writing media.
6. Gate a write test separately with explicit operator approval and a
   sacrificial target. Validate byte-exact read-back, error handling and
   repeatability, and label any test that actually exercises cache
   durability independently. Keep the recorded tested baseline and
   make any future opt-in, sentinel check or BOT recovery improvement
   separately reviewable before publishing a default build.

No firmware was uploaded or executed. This audit does not assert that
every recovered diagnostic was part of the historical hardware test.
