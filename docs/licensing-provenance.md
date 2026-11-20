# SL811_command licensing provenance

## Finding

The imported firmware explicitly says it follows supplied Poseidon source.
The four named routines and their detailed register behavior match Jason S.
McMullan's BSD-2-Clause SL811HS driver. This establishes a strong candidate
source and source-informed development. The import does not establish the
exact revision consulted, whether the firmware author typed from the source
or from a trace, or that any complete function body was copied. The strongest
evidence of a narrowly adapted implementation is the port-scan disconnect
branch and the transfer-issue SOF threshold and comparison. The reset
matches mainly concern hardware operations and their selected order.

This report assesses technical evidence. It does not decide whether a
particular fragment is copyrightable or whether a notice is legally required.

## Sources and revisions

| Source | Pin and relevance |
| --- | --- |
| Local firmware | SL811_command initial import `1c748053ba281410ac09d9b0b20e89a6df90a601`, November 20, 2026. `src/main.cpp` SHA-256 `2f9df0689087c7d7ab0014bb7a4767dcf8de47d8e1d2f0721f82acf61f51e91a`. Line numbers below refer to this snapshot. |
| Jason S. McMullan, poseidon-sl811hs | [v0.1.0 source](https://github.com/ezrec/poseidon-sl811hs/blob/0fec4f8d9ebcd515fbcbe92e7e9b2dc166bf5e6c/src/sl811hs.c) at `0fec4f8d9ebcd515fbcbe92e7e9b2dc166bf5e6c`. The file last changed at `f2d6e8d1256df0e270c3df5b8b47e6c51335560b` and is identical at those two commits. The four routines, including the SOF calculation, appear in the August 2, 2013 [initial source commit](https://github.com/ezrec/poseidon-sl811hs/blob/1164b90f86705487b8c2fe5ecd2dc42026d9ce98/src/sl811hs.c). The [upstream BSD-2-Clause text](https://github.com/ezrec/poseidon-sl811hs/blob/0fec4f8d9ebcd515fbcbe92e7e9b2dc166bf5e6c/LICENSE.txt) credits McMullan in 2013 and has SHA-256 `0fdba67944d3408b87e5f6f9e0df3fa3e14b9069755cf19a6ccd32c2a314b82c`. |
| Cypress SL811HS datasheet | [38-08008 Rev. *J](https://www.infineon.com/assets/row/public/documents/30/57/infineon-sl811hs-embedded-usb-host-slave-controller-additionaltechnicalinformation-en.pdf), August 1, 2017, host register tables and SOF setup example on pp. 6–14. |
| Cypress SL811HS errata | [38-17030 Rev. *A](https://www.infineon.com/assets/row/public/documents/24/52/infineon-errata-for-sl811hs-embedded-usb-host-slave-controller-erratasheet-en.pdf), January 7, 2011, items 1, 2 and 6. Item 2 covers Full-Speed SYNC to SOF and software deferral for Low-Speed. |
| USB-IF USB 2.0 base specification | April 27, 2000 base specification, §§7.1.7.5, 8.3–8.5 and 11.5.1. Read `usb_20.pdf` from the [June 3, 2025 USB-IF bundle](https://www.usb.org/document-library/usb-20-specification). PDF SHA-256 `d39698a33486c399124af92bd02e4f978fd9a836b5cf4e52e6e4633eb1d89f61`. |
| Original Poseidon 2.2 | Inspected the [archived distribution](https://ftp.fau.de/mirrors/aminet/driver/other/PoseidonMain.lha), SHA-256 `2b9030b932754d7916224e55310a8f81f2b8ceeb2d14947dcea5123c013d5be6`. `poseidon.library` identifies itself as 2.2 (14.12.03), SHA-256 `fd175aadd781009eb72cc524032ac3877e9487f77e94f63ffbd5626235d91fcf`. `hub.class` SHA-256 `fff6686a07ba32ffbfb3a3d39270a7fbc32bc8687d2a413f576ef737ac0c52ae` matches the aRCiUSB binary findings. The December 2003 English guide and readme describe the separated stack and hardware-driver interface. The archive lists Algor, Highway and Subway HCDs and supplies no named SL811HS driver or HCI source header. The library version strings, archive inventory and manuals establish the historical interface context. They do not reveal SL811HS register timing or firmware authorship. |
| Additional Poseidon HCI documentation | [Byte-identical HCI autodoc at the public reference revision](https://github.com/BlitterStudio/poseidon-usb/blob/bc56025c33345739963e65ce8253f747be9ab6bd/src/docs/usbhardware.doc) from the reviewed local checkout `04117009d950113d6af2fb688831b10cf39b298e` and [MorphOS SDK HCI header v2.2](https://morphos-team.net/sdk/includes/devices/usbhardware.html), April 26, 2007. The later references describe reset requests, root-hub emulation, transfers and bandwidth responsibilities at the HCI boundary. Its CMD_RESET entry describes a return to operational state after about 50 ms. They do not prescribe the SL811HS register sequence or the SOF heuristic. The 2007 header version is distinct from the 2003 library release. |
| Linux comparison | [Linux v6.6 SL811HS HCD](https://github.com/torvalds/linux/blob/v6.6/drivers/usb/host/sl811-hcd.c), GPL-2.0, downloaded source SHA-256 `9fe800044bfc1124aa2b91e49b2f5911da544e095873653c82e7ad889bba8b99`. Lines 347–375 implement a different SOF calculation. Lines 1313–1327 disable interrupts and hold SE0 for 50 ms. Used only to test whether particular implementation choices recur in another driver. The comparison does not imply that Arcicorsa consulted Linux or that the two drivers' histories are independent. |

## Classification and comparison

**A** is a controller requirement or register semantic. **B** is a USB or
Poseidon HCI requirement. **C** is a conventional technique or discretionary
implementation choice. **D** is a concrete candidate for retained expression,
supported by corresponding source lines beyond the necessary behavior.
Functional choices can be useful evidence of source consultation without
establishing that their implementation is protected expression.

### ResetHW

| Element | Classification and evidence |
| --- | --- |
| Read the revision register and interpret the result | **A.** Register 0x0E reads the silicon revision. Rev. 1.5 encodes 2 in the high nibble. **C.** Rejecting every full byte except 0x20 is a support policy, not a universal controller requirement. Local 342–352, upstream 1025–1030. |
| Initialize Control 2 | **A.** A write to 0x0F is required at least once after power-up, and the datasheet explicitly supplies 0xAE for Full-Speed host mode with the 1 ms SOF reload high bits. **C.** Writing this value before the reset sequence is a design choice. Local 354–356, upstream 1035–1036. |
| Clear status, mask interrupts, wait 40 ms | **A.** Status uses W1C, and the enable register gates the IRQ output without clearing latched status. **C.** The broad 0xFF clear, the exact clear/mask order, and the 40 ms pause are not prescribed by the reviewed materials. This is the most distinguishing functional correspondence in this stage. Local 356–359, upstream 1036–1041. |

**Recommendation 2, moderate-to-high confidence.** Record the matching
initialization recipe as a source reference. The fixed register calls and
delay parallel upstream behavior, but the local log calls, revision-error
path and absence of the upstream state fields leave insufficient evidence
to mark any expression in this stage as BSD-derived.

### ResetUSB

| Element | Classification and evidence |
| --- | --- |
| Assert and release SE0 | **A/B.** Control 1's reset setting drives SE0. USB §7.1.7.5 requires at least 50 ms overall reset on a root port. Holding it continuously for 50 ms implements that requirement. **C.** Masking IRQs around reset is a conventional precaution also seen in Linux. The extra mask write after ResetHW, the zero-valued release write and the point at which the port is sampled are software choices. Local 361–369, upstream 956–966 and 985–989. |
| Restart the host channels | **A.** Configure the required registers and SOF timer before arming the channel. A/B bank offsets are fixed. **B.** SOF is a USB mechanism. **C.** The contiguous base/length/PID/address/arm programming, connected-only A/B setup, unused B initialization, A/B bases 16/0 and final mask 0x23 are a specific recipe, not a USB or HCI requirement. 0x10 is the first buffer address. Zero is a placeholder for the inactive B channel, not a general packet-buffer allocation. Local 408–433 and constants 248–249, upstream 989–1010 and 2126–2138. |

**Recommendation 2, moderate confidence.** Attribute the observed sequence
as a reference. The repeated mask and inactive-B defaults strengthen the
inference of consultation. The matching literal writes implement a
register recipe and do not establish retained expression. The firmware
does not reproduce the upstream cancellation loop, toggle reset or
root-hub state updates.

### PortScan

| Element | Classification and evidence |
| --- | --- |
| Sample live connection and speed | **A.** Host interrupt status supplies W1C flags, a detect bit that is high for no device in this mode, and D+ status for differentiating an attached Full-Speed device from Low-Speed. **C.** Clearing all flags before sampling is a software choice. Local 371–377, upstream 901–906. |
| On disconnect, clear detect, read it back and conditionally clear all flags again | **D candidate.** Local 379–386, especially 382–386, and upstream 906–916, especially 914–916, implement the same conditional fallback in the same branch. W1C explains how to clear a flag, but does not require this three-step fallback. The local comment identifies the exact upstream branch. The read and test are split in the local code, with different names and trace calls. This is evidence of a narrow structural adaptation. It is not a finding of verbatim copying. |
| Configure speed, reload SOF and enable generation | **A/B.** The speed and polarity bits, 1 ms SOF reload values and timer enable are documented. **C.** Placement in the connected branch and use of separate Control 1/2 accumulators are natural choices. Both implementations write Control 2 before SOF low, unlike the low-then-high order shown in the datasheet example. This match adds evidence of a shared recipe without showing copied wording. Local 388–405, upstream 917–938. |

**Recommendation 4, moderate confidence.** Retain BSD attribution
precautionarily for the fallback at local 382–386. The source evidence
supports the structural match. It does not establish the copyright status
of that short branch. The rest of the scan can be described as
source-informed hardware programming without assigning BSD terms to the
entire routine.

### XferIssue

| Element | Classification and evidence |
| --- | --- |
| Stage OUT data and program the bank before the arm write | **A/B.** The controller requires valid buffer data and transfer parameters before launch, and USB defines packet identifiers and data toggles. **C.** The exact base/length/PID/address order is conventional, also follows increasing register addresses, and can use auto-increment in upstream. The local implementation uses separate indexed writes, including indexed OUT-buffer writes. Cypress errata item 6 documents a reason to avoid auto-increment. Local 590–600 and 617–619, upstream 430–443 and 456–457. |
| Decide whether the packet fits before SOF | **A/B.** Full-Speed frames are 1 ms. Reading 0x0F yields the remaining counter in units of 64 clocks at 12 MHz, and Cypress errata item 2 calls for deferring a Full-Speed packet that cannot fit. Payload scaling by one unit per eight bytes follows from eight bit times per payload byte and 64 clocks per counter unit. **D candidate.** Local 602–605 and 612–613 match upstream 447–452 in using the shifted payload length plus a fixed three-unit allowance and an inclusive comparison to set SYNC to SOF. The reviewed Cypress documents do not specify the allowance, rounding, or exact comparison. Linux uses a different estimate. The local comment identifies the upstream workaround. The matching two-step formulation is the strongest evidence of adaptation in this review. |
| Activate and observe completion | **A.** The ARM/ENABLE bits start the prepared transfer and DONE/status report its result. **C.** The Arduino implementation's per-transfer polling, timeout and residual checks are a separate design from upstream's active queue and interrupt handler. Local 526–558, 617–643, upstream 456–492. |

**Recommendation 3 for attribution treatment, moderate confidence.** Treat
the formulation at local 602–605 and 612–613 as narrowly adapted and retain
the BSD notice. Confidence is high that the specific heuristic was retained
and moderate that the evidence supports treating its short expression as
upstream-derived. The numerical relationship and hardware requirement
remain functional facts. This recommendation does not extend to the OUT
buffer loop, general register programming, activation or polling.

### Exact D comparison anchors

- **Disconnect fallback.** Upstream 914–916 writes DETECT to interrupt status,
  tests a fresh register read masked with DETECT, and writes 0xFF conditionally.
  Local 382–386 makes the same writes and test, with the read assigned to
  `verify` at 383 and the test at 385. This fallback remains distinguishable
  from the datasheet rule for W1C. See [upstream lines 914–916](https://github.com/ezrec/poseidon-sl811hs/blob/0fec4f8d9ebcd515fbcbe92e7e9b2dc166bf5e6c/src/sl811hs.c#L914-L916)
  and [local line 382](../src/main.cpp#L382).
- **SOF threshold.** Upstream 450 computes `(xfer->len >> 3) + 3` and
  451–452 compares remaining time inclusively. Local 605 computes
  `(length >> 3) + 3` and 612–613 applies the inclusive test to a stored read.
  The three-unit margin and inclusive comparison distinguish the formula
  from simple payload scaling. See [upstream lines 447–453](https://github.com/ezrec/poseidon-sl811hs/blob/0fec4f8d9ebcd515fbcbe92e7e9b2dc166bf5e6c/src/sl811hs.c#L447-L453)
  and [local line 602](../src/main.cpp#L602).

## Historical and release evidence

- The local Git history contains one import commit. Its firmware hash
  equals the working copy. There is no pre-import edit history showing
  how the four stages were written.
- The comments at 230–231 and 459 name supplied Poseidon source, and
  later comments identify exact branches and the transfer procedure.
  Together with matching optional details, they support direct
  consultation of a matching implementation. No supplied source
  attachment or record identifies its exact revision.
- The same imported file contains legacy diagnostic paths that use a 60 ms reset and program SOF differently (local 169–224). This provides a local contrast to the Poseidon-labelled recipe. It does not date either path.
- The original Due BIN and ELF remain in the build directory. The
  documented clean WSL build reproduced the BIN byte-for-byte. The
  ELF changed only in symbol metadata for two toolchain paths.
  This supports continuity with the imported firmware, without dating
  the first implementation or distinguishing a translation from a
  recreation of a trace. See [test results](test-results.md).
- Bench results document operation, while the USB and Poseidon documents
  explain interface requirements. None supplies contemporaneous evidence
  of a clean-room process. The observed evidence is consistent with an
  Arduino implementation informed by upstream behavior, with the narrow
  adapted fragments identified above.

## Proposed scope for owner approval

Keep the official PolyForm Noncommercial 1.0.0 text unchanged and apply it
only to original Arcicorsa material. Credit the four behavioral references
in the third-party inventory. Preserve the upstream BSD-2-Clause notice,
conditions and disclaimer for the identified SOF fragment and, as a
precaution, the disconnect fallback. Do not designate an entire function,
the file, or ordinary hardware constants as BSD-derived on this evidence.
Keep hardware licensing separate and retain the source-only release policy
pending linked-library and LGPL review.

Before adoption, seek the original supplied file, contemporaneous notes
or an author recollection that can distinguish direct transcription from
implementation from a trace. If stronger evidence locates copied
expression elsewhere, adjust the marked scope and preserve the actual
upstream terms. The notice proposals are for owner review.
