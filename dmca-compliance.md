# DMCA Compliance & Clean-Room Architecture

***

## Clean-Room Architecture & Execution Method

To keep our kernel implementation independent of the original's code, this project was developed with a **Clean-Room Reverse Engineering Pipeline**: the analysis of the proprietary binary and the writing of the open-source code are separated by a wall, and the only things allowed to cross it are written behavioural specifications, the encoder and NV2A calibration register data of §5, and the 16-byte certificate key of §6.

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
│           and NV2A calibration tables as flat data (§5), and   │
│           the 16-byte certificate key (§6). Nothing else.      │
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
* **The Action:** The AI agent acted as a reverse engineer to map out the system's mechanical behaviors: kernel exports and their observable behavior, data formats, hardware initialization routines and video encoder I2C registers. The BIOS images were unpacked (their boot loader decrypted) only to reach the kernel for analysis. The boot chain itself was not reimplemented from them: the OXOS BIOS boots with the open X-code sequence from XboxDev/cromwell and contains no Microsoft boot-ROM data. The only Microsoft key it carries is the certificate key of §6, passed to the kernel for save-game and System Link interoperability.
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
* **The Result:** A 100% original, functional and documented codebase that implements identical interoperable behavior using its own logic flow, variable naming conventions, and programming expressions. Every source file in this repository is original. The BIOS image the build produces also contains XboxDev/cromwell's X-code sequence and EEPROM key states (GPL-2.0), which the build fetches from that project, and the 16-byte certificate key of §6.

### 4. Verification: Black-Box Comparison
OXOS was checked against the retail kernel by running the same software on both in xemu, and comparing only what can be observed from outside: whether a title boots and plays, function results, the free memory a title starts with, the controller test, the display modes titles ask for (logged with debugger breakpoints at the kernel's entry points), and the state of the emulated hardware. Register values read from the running retail kernel are used only to see whether OXOS's values agree or differ; differences are recorded as behaviour in the specification, and the values themselves are not copied. Every difference found this way went back through Phase 1 before the code changed.

### 5. Integration of Video Encoder and NV2A Calibration Lookup Tables (LUTs)
Hardware-level configuration constants (the I2C register values required to initialize the Conexant, Focus, or Xcalibur video chips, and the NV2A memory pad drive/slew/delay calibration table the boot loader interpolates between) were extracted as flat data from the kernel in the Complex 4627 BIOS image and from the 5838 BIOS image. They are recorded as plain english description of register maps and function (register, value, functional purpose) in `spec/stock/18-conexant-encoder.md`, `19-focus-encoder.md` and `20-xcalibur-encoder.md`. Phase 2 derives functional hardware constants from those described encoder maps for the sole purpose of achieving an interoperable working video output on original physical hardware (the kernel's switch to them is in progress at the time of writing).

### 6. The Certificate Key
The 16-byte certificate key (Legal Basis §1.1) is the value the retail boot loader hands to the kernel. It crosses the wall as a single flat value, together with a behavioural description of its one use: deriving each game's save-signature, alternate-signature and System Link keys (`spec/stock/05-title-launch-xbe.md` §7). The OXOS boot loader passes it to the OXOS kernel in the same place, and the kernel derives the per-game keys from it with its own HMAC-SHA1 code (`kernel/src/xe/loader.cpp`). No other key material from the boot loader, the boot ROM or the kernel is used.

***

## Legal Basis for Reverse Engineering and Interoperability in OXOS

The methodologies used in this project are grounded in United States copyright law, the Digital Millennium Copyright Act (DMCA), and established judicial precedents governing software reverse engineering, functional information, and interoperability.

OXOS is an independently developed operating system and kernel for the original Xbox. Its objective is to reproduce the functional interfaces necessary to operate the original hardware and interoperate with existing retail Xbox software, without incorporating Microsoft's copyrighted operating system or reproducing its proprietary implementation.

### 1. Statutory Protections: The DMCA Interoperability Exception
The hardware and software analysis performed during this project implicates **17 U.S.C. § 1201(f)(1)** (Reverse Engineering Exception of the Digital Millennium Copyright Act):

> *"Notwithstanding the provisions of subsection (a)(1)(A), a person who has lawfully obtained the right to use a copy of a computer program may circumvent a technological measure that effectively controls access to a particular portion of that program for the sole purpose of identifying and analyzing those elements of the program that are necessary to achieve interoperability of an independently created computer program with other programs, and that have not previously been readily available to the person engaging in the circumvention, to the extent any such acts of identification and analysis do not constitute infringement under this title."*

**Section 1201(f)(3)** further provides for making qualifying information available to others *"solely for the purpose of enabling interoperability of an independently created computer program with other programs"*, provided that the information was obtained through the activities authorized by paragraphs (1) or (2), and that its provision does not constitute copyright infringement or violate other applicable law.

The analysis of the original Xbox kernel, including its extraction and examination from BIOS images, hardware initialization sequences, NV2A graphics configuration, video encoder register tables, and functional cryptographic mechanisms, is undertaken to achieve hardware and software interoperability. This includes:
* Reproducing the functional hardware initialization requirements necessary for an independently developed kernel to operate on original Xbox motherboards.
* Identifying video encoder register configurations necessary to produce correct video output across supported television standards.
* Identifying functional cryptographic inputs and key-derivation behavior necessary to maintain compatibility with existing game-generated data and networking protocols.

#### 1.1 Cryptographic Interoperability: Save-Game and System Link Compatibility
One specific interoperability requirement concerns a fixed 16-byte cryptographic value used by retail original Xbox consoles (the certificate key).

This value is identical across retail Xbox systems. The boot loader hands it to the kernel, and its only use there is as an input to the platform's per-game key-derivation process: when a game starts, the value is combined through HMAC-SHA1 with values from that game's own header to derive game-specific keys. These derived keys serve three functional purposes:
1. **Save-game signature authentication.** Games use their derived signature key to generate and validate signatures on their own saved games and configuration data. This allows a game to recognize data created by the same title.
2. **Alternate save-game signature authentication.** Additional derived keys let a game accept data from related titles that its header names, such as other regional releases.
3. **System Link network authentication.** A separate derived key is used to authenticate and secure System Link communications between consoles running the same game.

Without the original value, OXOS derives different per-game keys from those used by retail consoles. Consequently:
* Existing user-created save files from retail consoles may be rejected as damaged when loaded under OXOS.
* Save files created under OXOS may be rejected by retail consoles.
* System Link communications between OXOS and retail consoles fail, because their independently derived LAN keys do not match.

OXOS passes the 16-byte value from its boot loader to its kernel exclusively to reproduce this existing key-derivation behavior. Providing it restores compatibility with these existing formats and protocols.

Importantly, this value does not decrypt, unlock, authorize, or execute games. It does not replace Microsoft's RSA executable-signature verification, authenticate original discs, defeat copy protection, or bypass hard-drive security mechanisms. Those functions use separate mechanisms and keys.

Its inclusion is purely an interoperability requirement. Its purpose is to allow independently developed software to exchange and mutually use existing game-generated data and participate in existing networking protocols.

The project's intended use and distribution of this information are consistent with the interoperability objectives described in § 1201(f)(1)–(3), subject to the statutory requirements concerning lawful reverse engineering, provenance, copyright infringement, and other applicable law.

### 2. Judicial Precedent: *Sega Enterprises Ltd. v. Accolade, Inc.* (9th Cir. 1992)
In *Sega Enterprises Ltd. v. Accolade, Inc.*, 977 F.2d 1510 (9th Cir. 1992), the court ruled that disassembling a copyrighted binary to extract functional information necessary for interoperability can constitute legally protected **Fair Use**. The court held:

> *"Where disassembly is the only way to gain access to the ideas and functional elements embodied in a copyrighted computer program and where there is a legitimate reason for seeking such access, disassembly is a fair use of the copyrighted work, as a matter of law."*

The video encoder register values are technical necessities for interfacing with the physical motherboard hardware. They are not expressive content but functional parameters required to initialize and operate the hardware.

Likewise, the cryptographic analysis concerns functional inputs and algorithms necessary to reproduce existing interoperability behavior, rather than the expressive content of the original operating system. The ability to reproduce save-game authentication and System Link communications is a legitimate interoperability objective, not an attempt to reproduce Microsoft's copyrighted operating system or game content.

### 3. Judicial Precedent: *Sony Computer Entertainment, Inc. v. Connectix Corp.* (9th Cir. 2000)
In *Sony Computer Entertainment, Inc. v. Connectix Corp.*, 203 F.3d 596 (9th Cir. 2000), the court affirmed that reverse-engineering a proprietary BIOS to develop an independently created, interoperable alternative can constitute **Fair Use**. The court recognized that intermediate copying during reverse engineering may be justified when necessary to understand functional requirements and develop a non-infringing alternative.

This precedent is particularly relevant to OXOS because the project involves examining original Xbox BIOS and kernel behavior to develop an independently implemented operating system. OXOS does not require Microsoft's original kernel implementation to provide compatible hardware initialization, save-game handling, or System Link functionality. Instead, it independently implements the functional requirements necessary for compatibility.

### 4. The Merger Doctrine and Non-Copyrightability of Functional Data
Under **17 U.S.C. § 102(b)**, copyright protection does not extend to *"any idea, procedure, process, system, method of operation, concept, principle, or discovery."* This distinction applies to both hardware configuration data and functional cryptographic parameters.

#### 4.1 Hardware Configuration Data
The video encoder tables are numeric configurations dictated by the encoder chips' register design, the television standards they must produce (NTSC, PAL, EIA-770, SMPTE), and the console's motherboard wiring. Each value performs a specific hardware function, such as a clock divider, sync width, colour gain, or output routing. These are functional settings rather than creative expression.

Where hardware requirements dictate the available configurations and the ways of expressing those configurations are limited, the expression may merge with the underlying functional idea. The **Merger Doctrine** therefore provides an additional basis for distinguishing functional hardware parameters from copyrightable expression.

#### 4.2 Functional Cryptographic Parameters
The same distinction applies to the 16-byte cryptographic value and its role in the original Xbox's key-derivation process. The value is a fixed binary input to a mathematical cryptographic operation. Its purpose is to produce specific functional outputs required by existing software protocols. It does not represent source code, executable instructions, game content, or creative expression. Its function is determined by the original platform's cryptographic implementation and the interoperability requirements of the programs using it.

Similarly, the HMAC-SHA1 operation and the resulting key-derivation behavior are functional processes rather than copyrightable expression under § 102(b). The objective is not to reproduce the original implementation's expressive elements, but to reproduce the necessary functional behavior so independently developed software can interoperate with existing software.

### 5. Separation from Copyrighted Content and Access-Control Mechanisms
OXOS maintains a distinction between interoperability information and mechanisms protecting copyrighted content.

The project does not rely on the interoperability information discussed here to circumvent Microsoft's RSA executable authentication, authorize unsigned executables, decrypt commercial game content, authenticate copied discs, or defeat unrelated platform security mechanisms. The cryptographic value discussed in §1.1 is used only as an input to the original platform's per-game key-derivation process. Its inclusion does not change the existing authorization requirements governing whether a game may execute.

Historical original Xbox soft-modification techniques involving specially constructed save files are also distinct from this interoperability implementation. Those techniques relied on vulnerabilities in particular games. The cryptographic value itself does not execute code or independently authorize the execution of modified software.

### 6. Project Scope and Interoperability Objective
All reverse-engineering and implementation activities described in this disclosure are directed toward developing an independently created operating system capable of interoperating with the original Xbox platform. The functional information used by OXOS serves three principal objectives:
* **Hardware interoperability:** correctly initialize and operate the original Xbox motherboard, graphics hardware, and video output circuitry.
* **Data interoperability:** allow users to exchange and continue using their existing game-generated save files and configuration data across retail Xbox systems and independently developed systems.
* **Network interoperability:** allow independently developed systems to participate in existing retail Xbox System Link networking protocols.

These objectives are consistent with the definition of interoperability in **17 U.S.C. § 1201(f)(4)**:

> *"The ability of computer programs to exchange information, and of such programs mutually to use the information which has been exchanged."*

OXOS is an independently created implementation intended to preserve the functional compatibility of an existing computing platform. The information incorporated into the project is limited to what is necessary to achieve that objective. No copyrighted game content or proprietary operating system implementation is required for these interoperability functions.

The project's reverse-engineering, implementation, and distribution activities are undertaken with the intention of remaining within the applicable protections of United States copyright law and the statutory interoperability provisions of the DMCA.
