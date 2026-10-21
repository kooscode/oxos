# DMCA Compliance & Clean-Room Architecture

***

## Clean-Room Architecture & Execution Method

To keep our kernel implementation independent of the original's code, this project was developed with a **Clean-Room Reverse Engineering Pipeline**: the analysis of the proprietary binary and the writing of the open-source code are separated by a wall, and the only things allowed to cross it are written behavioural specifications and the encoder register data of §5.

The work was iterative: each feature (a kernel subsystem, a driver, a milestone such as "Halo reaches its main menu") went through its own cycle of analysis, specification, implementation and verification, and features were added one after another. The wall applies in every cycle.

```
┌────────────────────────────────────────────────────────────────┐
│        SINGLE AI AGENT: ONE CYCLE PER FEATURE, REPEATED        │
│                                                                │
│  [PHASE 1: THE REVERSE ENGINEER]                               │
│  Input: retail kernel and BIOS images (Ghidra disassembly and  │
│         decompilation, objdump); the retail kernel running in  │
│         xemu (black-box); public datasheets and open source.   │
│  Action: Map what the system does, mechanically.               │
│                                                                │
│                               ▼                                │
│                                                                │
│  [THE WALL: INTEROPERABILITY SPECIFICATION]                    │
│  Crosses: plain-text behavioural specs (spec/stock,            │
│           spec/cerbios), and the video encoder register        │
│           tables as flat data (§5). Nothing else.              │
│  *ZERO source, machine or disassembled code past this wall*    │
│                                                                │
│                               ▼                                │
│                                                                │
│  [PHASE 2: THE SOFTWARE ENGINEER]                              │
│  Input: the specifications (plus public docs and open source). │
│  Output: 1. Detailed documentation of the new software:        │
│             design specs (spec/oxos-internal), READMEs,        │
│             test descriptions and code comments.               │
│          2. 100% original C/C++ source code, tests and         │
│             tools, written strictly from those specifications. │
│                                                                │
│                               ▼                                │
│                                                                │
│  [VERIFICATION: BLACK-BOX COMPARISON]                          │
│  Run titles on both kernels in xemu and compare behaviour.     │
│  A difference goes back to Phase 1: the spec is corrected      │
│  from observations, then code refactored from corrected spec.  │
└────────────────────────────────────────────────────────────────┘
```

### 1. Phase 1: Reverse Engineering & Specification Generation
* **The Input:** The original, compiled Xbox kernels (`xboxkrnl.exe` 1.0.4627.1 and 1.0.5838.1, unpacked from retail BIOS images) analyzed with disassembly and decompilation tools (Ghidra, objdump). The Cerbios kernel was analyzed the same way for cross-checks and for features to add later. The retail kernel was also observed running in the xemu emulator, as a black box (§4). Public material was used alongside: chip datasheets (Conexant CX25870/871, Focus FS453/4), and open-source projects under their own licences (XboxDev/cromwell, nxdk, nouveau, xemu).
* **The Action:** The AI agent acted as a reverse engineer to map out the system's mechanical behaviors: kernel exports and their observable behavior, data formats, hardware initialization routines and video encoder I2C registers. The BIOS images were unpacked (their boot loader decrypted) only to reach the kernel for analysis. The boot chain itself was not reimplemented from them: the OXOS BIOS boots with the open X-code sequence from XboxDev/cromwell and contains no Microsoft keys or boot-ROM data.
* **The Constraint:** The agent was prohibited from retaining, reusing, or copy-pasting raw assembly language, decompiled C structures, internal variable names, or creative formatting from the original file. Its sole output was a functional specification documenting *what* the system does mechanically (`spec/stock/` and `spec/cerbios/` in this repository).

### 2. The Wall: How It Is Kept
Both phases were carried out by one AI agent with explicit instructions abide keep by strict rules on seperation and what may cross it and on what each phase may work from and to never stray from these rules. Frequent validations of clean-room state and review by AI agent to ensure clean-room approach is maintained.
1. **Only specifications cross.** Disassembly and decompiler output exist only inside the analysis tool's working session. They are never committed, and never pasted into a specification, a design document, the code or its comments.
2. **Specifications describe behaviour, not code.** They state inputs, outputs, side effects, data formats, the order of hardware accesses, and constants that the hardware or the ABI dictates. Addresses of the original's routines appear only as evidence pointers, so each statement can be checked. No code, decompiled structures or internal names are included.
3. **Code is written from the committed specification.** When the implementation needs something the specification does not say, the work goes back to Phase 1 and the specification is extended first. Code is never written from the disassembly.
4. **Verification is black-box (§4).** Its results are written into the specifications as behaviour, never as copied code or data.
5. **Never in the repository:** Microsoft or Cerbios images, unpacked kernels, decompiler output, or the scratch scripts that read the images. Cerbios's per-game patch data is never copied: OXOS derives its own fixes by testing.

### 3. Phase 2: Independent Clean Re-Implementation
* **The Boundary:** The agent treats the Phase 1 specifications as its *only* source of truth about the original kernel, alongside the public documentation and open-source references named above.
* **The Documentation:** Most of Phase 2 is documentation written in the project's own words. Before code is written, the agent writes a design document for each feature in `spec/oxos-internal/`: what OXOS does, the choices it makes, and where it deviates from the reference and why. Features with no reference counterpart are designed there from scratch: the boot splash, the kernel call tracer, the debug serial port, 48-bit disk addressing and exFAT storage. The READMEs, the test descriptions and the code comments describe the new software, and cite the behavioural specifications and design documents they implement, never the original binary.
* **The Action:** Operating as a clean system architect, the agent wrote entirely new C/C++ kernel code, tests and tools to satisfy the requirements laid out in the specifications and design documents.
* **The Result:** A 100% original, functional and documented codebase that implements identical interoperable behavior using its own logic flow, variable naming conventions, and programming expressions. Every source file in this repository is original. The BIOS image the build produces also contains XboxDev/cromwell's X-code sequence and EEPROM key states (GPL-2.0), which the build fetches from that project.

### 4. Verification: Black-Box Comparison
OXOS was checked against the retail kernel by running the same software on both in xemu, and comparing only what can be observed from outside: whether a title boots and plays, function results, the free memory a title starts with, the controller test, the display modes titles ask for (logged with debugger breakpoints at the kernel's entry points), and the state of the emulated hardware. Register values read from the running retail kernel are used only to see whether OXOS's values agree or differ; differences are recorded as behaviour in the specification, and the values themselves are not copied. Every difference found this way went back through Phase 1 before the code changed.

### 5. Integration of Video Encoder Lookup Tables (LUTs)
Hardware-level configuration constants (the I2C register values required to initialize the Conexant, Focus, or Xcalibur video chips) were extracted as flat data from the kernel in the Complex 4627 BIOS image and from the 5838 BIOS image. They are recorded as plain english description of register maps and function (register, value, functional purpose) in `spec/stock/18-conexant-encoder.md`, `19-focus-encoder.md` and `20-xcalibur-encoder.md`. Phase 2 derives functional hardware constants from those described encoder maps for the sole purpose of achieving an interoperable working video output on original physical hardware (the kernel's switch to them is in progress at the time of writing).

***

## Statutory & Judicial Precedents

The methodologies used in this project are fully protected under United States copyright law, the Digital Millennium Copyright Act (DMCA), and established judicial precedents governing software interoperability.

### 1. Statutory Protections: The DMCA Interoperability Exception
The hardware analysis performed during this project falls under **17 U.S.C. § 1201(f)(1)** (Reverse Engineering Exception of the Digital Millennium Copyright Act):

> *"Notwithstanding the provisions of subsection (a)(1)(A), a person who has lawfully obtained the right to use a copy of a computer program may circumvent a technological measure that effectively controls access to a particular portion of that program for the sole purpose of identifying and analyzing those elements of the program that are necessary to achieve interoperability of an independently created computer program with other programs, and that have not previously been readily available to the person engaging in the circumvention, to the extent any such acts of identification and analysis do not constitute infringement under this title."*

Section 1201(f)(3) further allows the information obtained this way to be made available to others *"solely for the purpose of enabling interoperability of an independently created computer program with other programs"*, to the extent doing so does not infringe copyright or violate other law.

The analysis of the kernel (including unpacking it from the BIOS images) and of the video encoder configuration was done solely to achieve hardware and software interoperability: allowing an independently created kernel to run original software on original hardware.

### 2. Judicial Precedent: Sega Enterprises Ltd. v. Accolade, Inc. (9th Cir. 1992)
In *Sega Enterprises Ltd. v. Accolade, Inc.*, 977 F.2d 1510 (9th Cir. 1992), the court ruled that disassembling a copyrighted binary to extract functional data requirements strictly necessary for hardware compatibility is a legally protected **Fair Use**. The court held:

> *"Where disassembly is the only way to gain access to the ideas and functional elements embodied in a copyrighted computer program and where there is a legitimate reason for seeking such access, disassembly is a fair use of the copyrighted work, as a matter of law."*

The video encoder register values are a technical necessity to interface with the physical motherboard hardware. Denying a developer the right to use these functional constants would grant the copyright holder an illegal monopoly over compatible hardware interfaces.

### 3. Judicial Precedent: Sony Computer Entertainment, Inc. v. Connectix Corp. (9th Cir. 2000)
In *Sony Computer Entertainment, Inc. v. Connectix Corp.*, 203 F.3d 596 (9th Cir. 2000), the court solidified the legality of reverse-engineering a proprietary hardware BIOS to build an interoperable, scratch-written equivalent. The court affirmed that copying a BIOS into memory during intermediate steps to observe its functional behavior, in order to write a non-infringing alternative environment, constitutes Fair Use.

### 4. The Merger Doctrine & Non-Copyrightability of Data
Under **17 U.S.C. § 102(b)**, copyright protection does not extend to any *"idea, procedure, process, system, method of operation, concept, principle, or discovery."* 

The video encoder tables are flat numeric configurations dictated by the encoder chips' register design, the television standards they must produce (NTSC, PAL, EIA-770, SMPTE), and the console's board wiring. Each value is a hardware setting with a function (a clock divider, a sync width, a colour gain, an output routing), not a creative choice, and the range of values that produces a correct picture on this hardware is narrow. Where the ways of expressing a functional idea are this limited, the *expression* merges with the *functional idea*: under the legal **Merger Doctrine**, when functional requirements dictate the format of data, that data is not protected by copyright.
