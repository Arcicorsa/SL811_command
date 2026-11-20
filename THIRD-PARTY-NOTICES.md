# Third-party notices and pending checks

The original project license applies only to the first-party portions
listed in NOTICE. Third-party rights and notices remain independent.

- **poseidon-sl811hs v0.1.0 (BSD-2-Clause).** Jason S. McMullan,
  copyright 2013. The matching reference is commit
  `0fec4f8d9ebcd515fbcbe92e7e9b2dc166bf5e6c`. The exact revision originally
  supplied is unknown. In `src/main.cpp` at initial import `1c74805`,
  BSD attribution applies to the PortScan disconnect fallback at lines
  382–386 as a precaution and to the XferIssue Full-Speed SOF threshold
  and comparison at lines 602–605 and 612–613. ResetHW and ResetUSB
  are credited as behavioral references only. Arcicorsa's original
  portions remain under PolyForm Noncommercial 1.0.0. Preserve the
  complete upstream copyright notice, conditions and disclaimer with
  source redistributions and reproduce them in the accompanying
  materials for any future binary redistribution.
  [Pinned source](https://github.com/ezrec/poseidon-sl811hs/blob/0fec4f8d9ebcd515fbcbe92e7e9b2dc166bf5e6c/src/sl811hs.c)
  · [Complete BSD-2-Clause notice](third-party/poseidon-sl811hs-LICENSE.txt)
  · [Provenance report](docs/licensing-provenance.md)
- **PlatformIO Core 6.2.0 scaffolding (Apache-2.0).** The tracked
  `include/README`, `lib/README` and `test/README` match the templates
  in `project/commands/init.py`. The comment header in `platformio.ini`
  matches `project/config.py`. The five upstream ignore rules directly
  below the modification notice in `.gitignore`, and the rendered
  `.vscode/extensions.json`, match the VS Code
  integration templates. These matches were verified after line-ending
  normalization. The upstream notice is:

      Copyright (c) 2014-present PlatformIO <contact@platformio.org>

  The upstream license is preserved in
  [third-party/PlatformIO-Core-LICENSE.txt](third-party/PlatformIO-Core-LICENSE.txt).
  Later project-specific changes to `platformio.ini` and `.gitignore`
  are identified in NOTICE.
  [PlatformIO Core templates](https://github.com/platformio/platformio-core/tree/v6.2.0/platformio/project)
- **Arduino SAM 1.6.12.** Source headers include LGPL-2.1-or-later
  notices in Print.cpp and the Due variant, separate permissive
  notices in the USB core, and Atmel terms on libsam sources.
  Verify the precise notice for each shipped component, including any
  linked libsam archive members and startup objects.
  [Arduino SAM upstream](https://github.com/arduino/ArduinoCore-sam)
- **CMSIS 4.5.0 and CMSIS Atmel 1.2.2.** The Arm Cortex-M3 header and
  prebuilt CMSIS libraries have Arm BSD-style notices despite the
  framework-cmsis package labeling itself Apache-2.0. Atmel device
  headers carry their own text. Use the actual notices for files and
  objects that are redistributed.
  [Atmel CMSIS upstream](https://github.com/arduino/ArduinoModule-CMSIS-Atmel)
- **Build tools and runtimes.** Atmel SAM 9.0.0 is Apache-2.0 and the SCons package 4.41101.0
  is MIT. Arm GCC 7.2.1 and the rest of the installed toolchain retain
  their upstream terms. Before binary distribution, identify the
  linked libgcc, C library, startup objects and archives using the
  exact build's link map. Verify the applicable runtime exceptions and
  copyright notices for those components.
  [Atmel SAM platform](https://github.com/platformio/platform-atmelsam/tree/v9.0.0)
  [GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html)

The initial release is source-only. Distribute neither Arduino BIN nor
ELF until a review of the actual linked build confirms applicable LGPL
obligations, including the
terms for user modification and reverse engineering to debug
modifications, complete corresponding library source, and a compliant
relinking route. Package any required third-party notices and license
texts verbatim when redistribution requires them.
[LGPL 2.1, section 6](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html)
