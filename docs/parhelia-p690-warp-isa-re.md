Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# P690 WARP vertex ISA — encoding, upload path and a decode of every program we run

This document is the *driver-facing* WARP reference: the subset of the
instruction set that is backed either by a program this project runs on real
P690 hardware, or by a bit-exact re-encoding of such a program.  It
consolidates and re-grounds `parhelia-warp-isa-complete.md` (which reverses
the vendor D3D assembler statically) and `parhelia-vertex-isa.md`.

> **A fact other drivers and agents will hit.**  The engine serves a
> previously executed WARP entry from an **instruction cache**: re-uploading
> different code to an address a program has already run from keeps running
> the old program.  Draining the queue, waiting for the WARP gate and
> toggling the program-source selector `0x920` bit 0 all fail to clear it.
> Give every upload a fresh address.  Section 9.3.

Evidence tags used throughout:

* **PROVEN(hw)** — a program containing this construct is loaded by
  `drivers/gpu/drm/parhelia/parhelia_warp.c` and produces correct rendering
  or a verified constant readback on the target (`192.168.1.111`, P690).
* **PROVEN(enc)** — the field assignment is confirmed by re-encoding a
  PROVEN(hw) instruction bit-exactly from its mnemonic form
  (`tools/parhelia-warp-roundtrip.py`, 88/88 arithmetic instructions).
* **BIN** — read out of a vendor binary at the quoted address
  (`MTXPARD.dll`, image base 0x10000, file offset = VA − 0x10000).
* **INF** — inferred from the above, not itself executed or read.
* **OPEN** — not determined.

---

## 0. Summary

| item | status |
|---|---|
| 96-bit dual-issue bundle layout, all 24 fields | PROVEN(enc) for every field a running program sets; BIN for the rest |
| register files and their 2-bit codes | PROVEN(enc) |
| swizzle encoding incl. the 0.0/1.0 literal codes | PROVEN(enc) |
| primary opcodes 0x71 add, 0x75 mul, 0x76 mad, 0x77 dp4, 0x78 dp3, 0x62 max, 0x89 persp-scale | PROVEN(hw) for add/mul + secondary mov; the rest PROVEN(enc) + INF semantics |
| secondary opcodes 1 mov, 9 mova | PROVEN(hw) mov, PROVEN(enc) mova |
| END / NOP / wra templates | PROVEN(hw) |
| program store, entry token, load sequence | PROVEN(hw) |
| constant upload via the loader program | PROVEN(hw) — the kernel reads all 11 vectors back |
| branch / control class (0x23, 0x46, imm32) | PROVEN(hw) inside the loader only; not re-encodable yet |
| the C encoder and program builder | PROVEN(enc): reproduces six kernel programs dword for dword |
| relative addressing (`a0`, base registers b0..b3) | PROVEN(hw) inside the loader; base register *values* OPEN |

---

## 1. The instruction store and the entry token  [PROVEN(hw)]

The WARP instruction store is **not** on-chip memory that the driver fills
through a port.  On the P690 the engine fetches instructions over PCI from a
host DMA buffer:

* `parhelia_warp_init()` allocates one coherent DMA buffer of
  `PARHELIA_WARP_STORE_INSTRUCTIONS * 12 + 383` bytes and aligns its bus
  address up to a multiple of **384** (`(384 - bus % 384) % 384`; the vendor
  allocator uses a division, not a power-of-two mask).
* One instruction is **12 bytes** = three little-endian dwords `w0 w1 w2`.
* `parhelia_write(pdev, 0x0920, read(0x0920) | 1)` switches instruction
  fetch to the owned class-3 (PCI bus master) heap.
* An **entry token** is
  `((bus / 12) * 4 + instruction_index * 4) | 3`
  (`parhelia_warp_entry()`).  It is an absolute instruction ordinal in
  12-byte units, shifted left by two, with the low two bits carrying the
  memory class (3 = bus master).  It is written to `0x1100`
  (`P3D_R_WARP_ENTRY`, reglist index 0x40) and `0x1104` (index 0x41).

Consequences that matter for a driver:

* The store size is a driver policy choice, not a hardware limit — a second
  DMA allocation is just as valid an instruction source as the first.  This
  is what the user-program heap of §9 uses.
* Because the entry is a *bus address* derived token, a client that can write
  register index 0x40 freely can point the vertex engine at arbitrary system
  memory.  See §9.3.

### 1.1 Store gate

Before any store write the driver waits for the WARP gate
(`warp_gate_ready()`): `0x08f0 & 0x03020000 == 0x00020000`,
`0x0908 & 0xfff7ffbf == 0x63a78001`, `0x0924 & 0xfff == 0x100`.  Writing the
store while the engine is executing from it is what produces the
"WARP store busy" failure after an engine lock-up.  [PROVEN(hw)]

### 1.2 Layout the kernel uses

`PARHELIA_WARP_STORE_INSTRUCTIONS = 2816`.  Instructions 0..1655 hold the
nine resident vendor images (`parhelia_firmware.h`); the driver's own
programs start at 1760 and are spaced 128 instructions apart:

| entry (instr) | program | length |
|---|---|---|
| 1760 | passthrough (or vertex-probe with `vertex_probe=1`) | 5 (6) |
| 1888 | textured | 5 |
| 1896 | textured multitexture | 7 |
| 2016 | transform | 11 |
| 2144 | lighting | 16 |
| 2272 | replace | 6 |
| 2400 | constant loader | 14 |
| 2528 | constant probe (only with `warp_probe=1`) | 3 |

---

## 2. Instruction encoding  [PROVEN(enc) unless noted]

One bundle = a **primary** operation (opcode `w2[30:23]`, up to three
sources, writes destination port **B**) and an optional **secondary**
operation (4-bit opcode `w1[12:9]`, reads src2, writes destination port
**A**).  Both execute in the same 96-bit word.

| bits | name | meaning |
|---|---|---|
| `w0[3:0]` | wmaskA | port-A write mask, x y z w |
| `w0[7:4]` | dstA | port-A destination index |
| `w0[9:8]` | fileA | port-A destination file |
| `w0[13:10]` | wmaskB | port-B write mask |
| `w0[17:14]` | dstB | port-B destination index |
| `w0[19:18]` | fileB | port-B destination file |
| `w0[23:20]` | src2 | src2 index (also the secondary op's source) |
| `w0[24]` | neg2 | negate src2 |
| `w0[26:25]` | file2 | src2 file |
| `w0[30:27]` | src1 | src1 index |
| `w0[31]` | neg1 | negate src1 |
| `w1[1:0]` | file1 | src1 file |
| `w1[5:2]` | src0 | src0 index |
| `w1[6]` | neg0 | negate src0 |
| `w1[8:7]` | file0 | src0 file |
| `w1[12:9]` | sop | secondary opcode (0 = none) |
| `w1[13]` | idxsel | relative index source: 1 = `a0` (latch T1), 0 = base register `w1[26:25]` |
| `w1[16:14]` | exthi | high 3 bits of the one extended operand index |
| `w1[19:17]` | extsel | which operand is extended (0/1/2 = src0/1/2); 2 when unused |
| `w1[21:20]` | — | never set by any running program; BIN kernel patch kind 0x13 sets 1 in two vendor fragments — OPEN |
| `w1[24:22]` | f1g | port-B/src1 relative and extension control; **1** in every running arithmetic instruction |
| `w1[26:25]` | relcomp | `a0` component, or base-register select when `idxsel = 0` |
| `w1[29:27]` | f1i | relative operand select: 4/5/6 = src0/1/2, 7 = port-A dst; 0 when nothing is relative |
| `{w2[9:0], w1[31:30]}` | swzShared | 12-bit swizzle belonging to src1 (`w2[22]=0`) or src2 (`w2[22]=1`) |
| `w2[21:10]` | swz0 | src0 swizzle |
| `w2[22]` | owner | shared-swizzle owner: 0 = src1, 1 = src2 |
| `w2[30:23]` | op | primary opcode |
| `w2[31]` | f2b | 1 on arithmetic, 0 on NOP/END/control and on the loader's control-class instructions |
| `{w2[21:0], w1[31:22]}` | imm32 | immediate, control-class opcodes only |

**Swizzle**: three bits per component, component *c* at `[3c+2:3c]`.
Codes `0..3 = x y z w`, `4 = 0.0`, `5 = 1.0`, `7 = identity (own component)`,
`6 = OPEN`.  The identity quad is `0 | 1<<3 | 2<<6 | 3<<9` = **0x688**, the
all-zero quad `4*0o1111` = **0x924** and the all-one quad **0xb6d**.
[PROVEN(enc)]

The 0.0 and 1.0 swizzle codes are how running programs materialise literals
**without spending a constant slot**: `r1.0000` and `r1.1111` in the kernel
programs are an arbitrary register read with swizzle `0x924` (all 4) and
`0xb6d` (all 5).  The register index is irrelevant.  [PROVEN(hw)]

### 2.1 File codes

| code | source file | destination file |
|---|---|---|
| 0 | input `v#` (vertex fetch) | latch/address `T#` (`T1` = `a0`) |
| 1 | temporary `r#` | temporary `r#` |
| 2 | latch `T#` | output `o#` |
| 3 | constant `c#` | constant `c#` |

Note the asymmetry: file code 2 means *latch* when read and *output* when
written; file code 0 means *input* when read and *latch* when written.
[PROVEN(enc), consistent with BIN tables 0x1dd16c/0x1dd194]

### 2.2 Templates

```
wra  (arithmetic no-op, carrier for a secondary op)
     00080200 00440000 b01a21a2     ; op 0x60, f2b=1, both files 2, masks 0
END  00000200 00040000 14c00000
NOP  00000200 00040000 00000000
```
Every running program ends with `END` followed by one `wra` word whose
`f2b` is 0 (`00080200 00440000 301a21a2`) — see §6.  [PROVEN(hw)]

---

## 3. Register files as the running programs use them

### 3.1 Inputs `v#` — vertex fetch slots  [PROVEN(hw)]

| reg | contents in our programs | set by |
|---|---|---|
| `v0` | position (x, y, z, w) | attribute descriptor 0 |
| `v5` | diffuse colour | descriptor 1 |
| `v7` | texture coordinate set 0 | descriptor 2, `0x1110`/`0x1114` |
| `v8` | texture coordinate set 1 | descriptor 3, `0x1118` |

This is the classic D3D fixed-function numbering (`v0` position, `v3`
normal, `v5` diffuse, `v6` specular, `v7..v14` texcoords).  The lighting
program reads the normal from `v5`, i.e. it reuses the colour slot; that
program has not run on hardware.

### 3.2 Outputs `o#`  [PROVEN(hw) for 0, 1, 3, 4, 11]

| reg | meaning |
|---|---|
| `o0` | screen-space position — the setup unit's input |
| `o1` | colour 0 |
| `o2` | colour 1 (specular) — BIN only |
| `o3..o10` | texture coordinate sets 0..7; `o10.w` = fog — `o3`, `o4` PROVEN(hw) |
| `o11` | clip-space position copy; every running program writes it |

`o0` carries **screen space**, not clip space: the transform program
performs the perspective divide and viewport scale itself (§5.6), and the
passthrough programs are fed already-transformed window coordinates by
Gallium's draw module.  Writing clip space to `o0` has never been tried.
[PROVEN(hw) for the screen-space form; the clip-space question stays OPEN]

`o4` must be written whenever texture-coordinate set 1 is advertised: the
hardware otherwise removes set 1 from `0x1110` before the draw.
[PROVEN(hw), kernel comment on `warp_textured_multitex`]

### 3.3 Temporaries `r#`, latches `T#`, constants `c#`

`r0..r15` direct, `r16..r127` through `exthi`.  `T0..T15` latches; `T1`
doubles as the relative-address register `a0` (written by secondary op 9,
`mova`).

**T2 is special.**  Draws depend on WARP temporary T2.  After power-on it
holds garbage — every draw is rejected and the fence never retires.  After
a warm reboot it holds a stale value and the engine locks up.  The
passthrough, multitexture and replace programs therefore contain
`mul T2, v5, r1.0000` (T2 ← 0).  The *textured* program must **not** write
it: with the T2 write the first vertex-alpha draw of the Q3 demo locked the
engine every time (and it also ran 28.9 instead of 26.4 fps, so T2 probably
feeds an LOD or perspective term).  Mesa issues one degenerate passthrough
draw at context creation (`ph_prime_warp()`) to prime it.  Any new program
must follow the same rule: prime T2 elsewhere, do not write it in a
program that samples a texture.  [PROVEN(hw), kernel commits `aa7afa1`,
`0c28239`]

Constants `c0..c15` direct, `c16..c127` through `exthi`, `c128..c255`
through base register 2.  The constant file *is* the engine block-state
file (`parhelia-block-state-map.md`): `c0x160/0x161` hold the raster block's
own copy of the viewport pair, `c0x172..0x17e` the setup constants.  A
program that writes file 3 therefore writes engine state, which is why the
user-program validator of §9 rejects constant destinations outright.

---

## 4. Opcodes

### 4.1 Primary, `w2[30:23]`

| op | n | mnemonic | semantics | evidence |
|---|---|---|---|---|
| 0x60 | 0 | `wra` | arithmetic no-op, secondary carrier | PROVEN(hw) |
| 0x61 | 2 | `min` | componentwise | BIN |
| 0x62 | 2 | `max` | componentwise | BIN; used by `warp_lighting` |
| 0x63 | 2 | `slt` | | BIN |
| 0x64, 0x65, 0x67, 0x68 | 2 | compare family | | OPEN |
| 0x66 | 2 | `sge` | | BIN |
| 0x69 | 1 | | | OPEN |
| 0x6d | 1 | primary `mov` | | INF |
| 0x71 | 2 | `add` | componentwise | **PROVEN(hw)** |
| 0x72 | 2 | add/sub variant | | OPEN |
| 0x73 | 2 | `sub` | | INF |
| 0x75 | 2 | `mul` | componentwise | **PROVEN(hw)** |
| 0x76 | 3 | `mad` | `s0*s1+s2` | PROVEN(enc); INF semantics |
| 0x77 | 2 | `dp4` | 4-component dot, broadcast | PROVEN(enc) |
| 0x78 | 2 | `dp3` | 3-component dot, broadcast | PROVEN(enc) |
| 0x79 | 2 | dp4 + clip flag | | INF |
| 0x7a | 2 | dp4 variant | | OPEN |
| 0x89 | 2 | `smul` — **`dst.c = s1.c * (1 / s0.w)`** | perspective divide | PROVEN(enc); semantics INF (strong, §5.6) |
| 0x8a | 2 | `s1.c * rsq(s0.w)` — normalise | | INF |
| 0x8b..0x8d | 1 | scalar transcendentals on `.w` | | INF |
| 0x91 | 1 | `exp` (D3D `expp` layout) | | BIN |
| 0x92 | 1 | `log` (D3D `logp` layout) | | BIN |
| 0x93 | 2 | `pow` | | BIN |
| 0x94 | 2 | `lit` | | INF |
| < 0x60 | — | control class (NOP 0x00, branch 0x23/0x46, END 0x29, `getc`, `subi`, …) with `imm32` and `f2b = 0` | | PROVEN(hw) in the loader |

### 4.2 Secondary, `w1[12:9]` — reads src2, writes port A

| op | mnemonic | evidence |
|---|---|---|
| 0 | none | |
| 1 | `mov` | **PROVEN(hw)** (`mov o11, v0`, `mov o3, v7`) |
| 4 | `frc` | BIN |
| 5 | `floor` | BIN |
| 7 | clamped `mov`, used for `o10.w` fog | INF |
| 8 | `getidx` — writes the index-register quad, no vector source | PROVEN(hw) in the loader |
| 9 | `mova` — write `a0` (= latch T1) | PROVEN(hw) in the loader |
| 0xc | `rcp` (`.w`) | BIN |
| 0xd | `rsq` (`.w`) | BIN |
| 0xe | `lit` | BIN |
| 2, 3, 6, 0xa, 0xb | | OPEN |

A primary op with three sources (`mad`) occupies src2 and therefore cannot
be paired with a secondary op.  [BIN 0x192f22, and no running program does
it]

---

## 5. Every program the driver runs, decoded

Disassembly produced by `tools/parhelia-warpdis2.py`; each arithmetic word
re-encodes bit-exactly from the printed mnemonic
(`tools/parhelia-warp-roundtrip.py`).

### 5.1 `warp_passthrough` — entry 1760  [PROVEN(hw)]

```
00083e00 00440000 b89a2249   add o0, v0, v0.0000          ; o0 = v0 (window space)
0800bebf 00440215 ba9a2249   mul T2, v5, r1.0000 | mov o11, v0
08087e00 00440015 b89a2249   add o1, v5, r1.0000          ; o1 = v5
00000200 00040000 14c00000   end
00080200 00440000 301a21a2   wra                          ; f2b = 0 tail
```
`v0.0000` and `r1.0000` are the all-zero swizzle, so both adds are moves.
Two secondary moves cannot share a bundle, hence the `add ... , x.0000`
idiom for the second one.

### 5.2 `warp_textured` — entry 1888  [PROVEN(hw)]

```
00083e00 00440000 b89a2249   add o0, v0, v0.0000
08087ebf 00440215 b89a2249   add o1, v5, r1.0000 | mov o11, v0
0078023f 00440200 b01a21a2   mov o3, v7                   ; secondary op 1
00000200 00040000 14c00000   end
00080200 00440000 301a21a2   wra
```
No T2 write — deliberately (§3.3).

### 5.3 `warp_textured_multitex` — entry 1896  [PROVEN(hw)]

`warp_passthrough` plus `mov o3, v7` and `mov o4, v8`.  Writes T2.

### 5.4 `warp_replace` — entry 2272  [PROVEN(hw)]

```
00083e00 00440000 b89a2249   add o0, v0, v0.0000
0800bebf 00440215 ba9a2249   mul T2, v5, r1.0000 | mov o11, v0   ; T2 = 0
08087e00 40440109 b89a22db   add o1, T2, r1.1111                 ; o1 = 1,1,1,1
0078023f 00440200 b01a21a2   mov o3, v7
00000200 00040000 14c00000   end
00080200 00440000 301a21a2   wra
```
GL_REPLACE: the vertex colour is forced to white so the combiner's
`TEX * PREV` yields `TEX`.  This is also the clearest proof that swizzle
code 5 is the literal 1.0 and code 4 the literal 0.0: the same register
`r1` yields 0 in one instruction and 1 in the next.

### 5.5 `warp_vertex_probe` / `warp_constant_probe`  [PROVEN(hw) with `warp_probe=1`]

```
000803ff 00440200 b01a21a2   mov c15, v0      ; secondary op 1, destination file 3
00000200 00040000 14c00000   end
```
The kernel then reads `c15` back through the `0x0910`/`0x0914`/`0x0938`
constant port and compares it against the input vector, for two different
input patterns.  This is a *closed loop through the hardware*: it proves the
file-3 destination encoding, the secondary `mov`, the END convention and the
entry token in one step.

### 5.6 `warp_transform` — entry 2016  [not yet run on hardware]

```
00003e00 00440000 b89a2249   add T0, v0, v0.0000            ; T0 = v0
1804a200 00440103 bb9a21a2   dp4 r2.w, T0, c3
10049200 00440103 bb9a21a2   dp4 r2.z, T0, c2
08048a00 00440103 bb9a21a2   dp4 r2.y, T0, c1
00048600 00440103 bb9a21a2   dp4 r2.x, T0, c0               ; r2 = MVP * v0 (clip)
0e987c1f 00440215 b89a2249   add o1, v5, r1.0000 | mov T1, c9
12257ebf 00440289 c49a22a2   smul r5, r2, r2.xyz1 | mov o11, r2
2c105e00 004401a1 bb6a2222   mad T1.xyz, c8.xyz1, r5, T1.xyz0
08083e00 00440105 b89a2249   add o0, T1, r1.0000
00000200 00040000 14c00000   end
00080200 00440000 301a21a2   wra
```
This is the semantic anchor for opcode 0x89.  `r5 = smul(r2, r2.xyz1)` has
to be `(x/w, y/w, z/w, 1/w)` for the following
`T1.xyz = c8.xyz * r5.xyz + c9.xyz` to be the viewport transform with the
documented constants `c8 = (W/2, -H/2, (zf-zn)/2, 1)` and
`c9 = (x0+W/2, y0+H/2, (zn+zf)/2, H)`.  `T1.w` is untouched by the `mad`
and therefore reaches `o0.w` as `c9.w = H`.  [INF, strong]

The matrix is read **row-wise transposed**: `c[r] = (M[r], M[r+4], M[r+8],
M[r+12])` for a column-major GL matrix, i.e. `dp4 r2.x, v0, c0` computes
row 0 of `MVP * v`.

Note that the program writes `T2` nowhere but writes `T0` and `T1`; the
T2 rule of §3.3 applies to it unchanged and is currently satisfied only
because Mesa primes T2 with a passthrough draw.

### 5.7 `warp_lighting` — entry 2144  [not yet run on hardware]

`warp_transform` with a directional-light block inserted before the
viewport tail:

```
2004fe00 00440017 bc1a21a2   dp3 r3, v5, c4          ; N.L   (normal in v5)
0804fe00 0044008d b11a2249   max r3, r3, r1.0000     ; max(0, N.L)
1a10be00 00440195 bb5a2249   mad T2, c5, r3, r1.0000 ; diffuse * N.L
0c20be00 40440199 bb1a22db   mad T2, c6, r1.1111, T2 ; + ambient
08087e00 00440109 b89a2249   add o1, T2, r1.0000
```
`c4` light direction, `c5` material, `c6` ambient.  Gouraud lighting: the
setup unit interpolates `o1`.

### 5.8 `warp_constant_loader` — entry 2400  [PROVEN(hw)]

The only program with control flow.  It is bound in *loader* mode
(`0x10f0 = 0x20000028`) and consumes one vec4 per iteration through the
attribute port; `v0 = (count, first, 1, 0)`, `v1..` the payload.

```
0008016f 30443000 301a21a2   getidx r6, a0+0          ; save a0
00700600 00040000 19800000   getc c7, v0.x            ; scalar count
00080011 404f1200 305a2092   mova T1.x, v0.yyyy       ; a0.x = first
00080012 804f1200 305a2124   mova T1.y, v0.zzzz       ; a0.y = 1
00740200 0044001c 21000000   subi c7, c7, 1           ; sets flags
00018200 01440000 11c00000   br cc6, done
00000200 00040000 00000000   nop                      ; branch delay slot
0008030f 019d2200 301a21a2   mov c[a0.x], v[a0.y]
00180013 20443000 301a21a2   getidx T1.xy, a0+1       ; advance both indices
00000200 fe840000 11ffffff   br copy
00000200 00040000 00000000   nop
0268001f 004f1200 301a21a2   mova T1, r6             ; restore a0
00000200 00040000 14c00000   end
00000200 00040000 00000000   nop
```

Observations that the driver depends on:

* every branch has a **delay slot** filled with `nop`;
* the control-class instructions carry `f2b = 0`;
* `br` encodes a *signed* relative displacement in `imm32`;
* the loader is driven by 0x41 block packets whose first dword is
  `0x41<count*2><first…>`-shaped; `parhelia_warp_state.h` holds the exact
  shapes the kernel uses, and `parhelia_validate_constant_group()` the ones
  userspace may send.
* The kernel verifies all 11 uploaded vectors by reading them back through
  the constant port, so this whole path is closed-loop PROVEN(hw).

---

## 6. Entry, termination and binding conventions  [PROVEN(hw)]

* A program **starts** at the instruction the entry token names; there is no
  header word.
* It **ends** with `END` (`00000200 00040000 14c00000`).  Every driver
  program then stores one extra word — `wra` with `f2b = 0`
  (`00080200 00440000 301a21a2`) — after END.  The vendor images do the
  same.  Treat it as required padding: the fetch unit reads bundles in
  pairs.
* Binding is a register packet, not a store write:
  `0x10f0` (WARP_MODE) then `0x1100`/`0x1104` (entry).  `p3d_bindpacket()`
  writes the mode twice, then the entry, then the mode again — the
  duplication is in every vendor capture and removing it was not tried.
* Draw class modes: `0x20000002` / `0x20000028` (loader) /
  `0x3e403c3c`-shaped stage words; see `parhelia_warp_state.h`.

---

## 7. Constant upload  [PROVEN(hw)]

Two routes reach the constant file:

1. **The loader program** (§5.8) driven by 0x41 block packets.  Payload
   sub-header `(count, first, 1, 0)` followed by `count` vec4s.  This is the
   route Mesa's `p3d_emit_constant_group()` uses and the only one available
   to userspace.
2. **The `0x0910`/`0x0914`/`0x0938` port**, kernel only, used for readback
   (`warp_read_constant()`: per component write
   `0x0910 = (3 << 24) | (comp << 16) | slot`, `0x0938 = 0xfffffffc`, then
   **four** settling reads of `0x0914`).

Before any of this works, `a0` and the base registers `b0..b3` must be
latched by the kind-1 program (`mtx251`, entry `PARHELIA_WARP_BASE_ENTRY`);
otherwise the loader's loop bound is whatever the previous context left and
the engine stops retiring.  The kernel does this once at init.

---

## 8. Round-trip verification

`tools/parhelia-warp-roundtrip.py` extracts every `static const u32
warp_*[]` array from `parhelia_warp.c`, disassembles each instruction with
`parhelia-warpdis2.py` and re-assembles the printed mnemonic with
`parhelia-warpasm.py`, asserting the three dwords come back identical.

Result: **88 arithmetic instructions match bit-exactly, 0 mismatches**, 10
words skipped.  Every skipped word is in `warp_constant_loader`: two
branches, their delay-slot `nop`s, the `getidx`/`mova` index setup and the
relative `mov c[a0.x], v[a0.y]`.  `END`, `NOP` and both `wra` templates are
compared, not skipped.  The tool exits non-zero on any mismatch, so it can
be run as a regression check whenever `parhelia_warp.c` changes.

---

## 9. The user-program upload ABI

### 9.1 Store

A second coherent DMA heap, `PARHELIA_WARP_USER_INSTRUCTIONS` instructions
long, divided into fixed slots of `PARHELIA_WARP_USER_SLOT_INSTRUCTIONS`.
Each slot is owned by one `drm_file` and released when it closes.  Entry
tokens are computed exactly as in §1.

### 9.2 What the validator accepts

A render-node client's program executes on the vertex engine with full
access to the constant/block-state file, so the validator is a whitelist:

* the program must contain `END` **and at least one word after it**: the
  fetch unit reads bundles in pairs, so a program ending exactly at the slot
  boundary would execute whatever the previous tenant left.  The rest of the
  slot is cleared on every upload for the same reason;
* primary opcode must be one of the arithmetic opcodes of §4.1 with
  `f2b = 1`, or the exact `END`/`NOP`/`wra` templates.  **The control class
  (`op < 0x60`) is rejected**: no branches, hence no loops, hence no way to
  stall the engine from a program;
* secondary opcode must be 0, 1, 4, 5, 7, 0xc, 0xd or 0xe.  `mova` (9) and
  `getidx` (8) are rejected because they set up relative addressing;
* **destination file 3 (constants) is rejected on both ports** — that file
  is engine state, including surface base addresses;
* **relative addressing is rejected**: `w1[29:27]` (f1i) must be 0 or 2 and
  `w1[13]` must be 0, so every index is a literal the validator can bound;
* `w1[21:20]` must be 0 (meaning OPEN, §2);
* index bounds after `exthi`: `v# <= 15`, `r# <= 31`, `T# <= 15`,
  `o# <= 11`, `c# <= 255`;
* `exthi`/`extsel` must be consistent: `extsel <= 2` selects the one
  extended operand, otherwise `exthi` must be 0.

Constant *reads* are allowed.  They can observe another context's
constants, which is the same exposure the shared engine state already has;
they cannot be used to fetch memory.

### 9.3 The engine caches instructions per entry  [PROVEN(hw)]

Re-uploading different code to an address a program has already executed
from keeps running the **old** program.  Measured with
`parhelia-drm-test --warp-program-probe`: uploading the passthrough program,
drawing, then re-uploading the same slot with one swizzle changed
(`add o1, v5.yzxw` instead of `add o1, v5`) still drew red; the same changed
code uploaded to a fresh slot drew the blue it should.  Draining the queue
and waiting for the WARP gate does not help, and neither does toggling the
program-source selector `0x920` bit 0 around the write.

**This bites anyone editing a kernel WARP program.**  The resident heap is a
`dmam_alloc_coherent()` allocation, and a module reload can land it at the
same bus address, which makes the entry tokens identical.  Change a program,
rebuild, reload without rebooting, and the engine may keep executing the
previous image - an experiment that looks like the edit had no effect, or
like the hardware behaves inconsistently between runs.  Reboot between
program edits, or check that the entry the kernel prints has changed.

The vendor avoids the problem structurally: its heap is carved into 682
single-use slots of 128 instructions.  This driver does the same in
miniature - a slot holds two program images and an upload always lands on
the one the slot did not last use, so `WARP_PROGRAM` returns a different
entry every time and callers must use the value it returns.  Both images
stay accepted by the submit validator, because a client may still have draws
queued against the previous entry.

### 9.4 The encoder in the driver

`reference/p3d_warp.c` implements the encoder of section 2 and a builder for
the fixed-function shapes.  `warp_build_selftest` in `parhelia_drm_test.c`
requires it to reproduce six of the programs above **dword for dword**:
passthrough, textured, textured multitexture, replace, transform and
transform-with-lighting.  All six match, so the field assignment, the opcode
values, the literal swizzle codes and the shared-swizzle ownership rule are
confirmed by construction, not only by reading.

`parhelia_vs_compile.c` is the NIR front end on top of it.

### 9.5 Entry-register enforcement

Independently of the upload path, `parhelia_validate_reglist()` now rejects
writes to reglist index 0x40 / 0x41 (`0x1100` / `0x1104`) whose value is not
an entry this kernel handed out — a resident/driver program entry, one of
the caller's own user slots, or the two literal binding values 0 and 2.
Without that check a render-node client could point the instruction fetch at
an arbitrary class-3 bus address, i.e. at arbitrary system memory, and have
the resulting "program" write the surface-base block.

---

## 10. Hardware results

`parhelia-drm-test --warp-program-probe /dev/dri/card0` on the P690:

| check | result |
|---|---|
| draw with the kernel's passthrough entry | red, as expected |
| draw with a userspace copy of it uploaded through the ioctl | red |
| re-upload with a rotated colour swizzle | blue |
| draw with a program `p3d_warp_build()` generated | red |
| six malformed programs (constant destination, branch, relative addressing, output index out of range, missing END, END without its padding word) | all rejected with -EINVAL |
| an entry the kernel never handed out | -EACCES |
| a released slot's entry | -EACCES |

The probe renders into a `PARHELIA_CREATE_RENDER_NATIVE_COLOR` allocation
and reads it back through `DOWNLOAD_RENDER`.  It first used a dumb buffer,
which the 3-D engine does not write on this kernel - the clear appeared but
no triangle did.  `--p690-multitex-warp-probe` still fails for exactly that
reason and needs the same conversion.

## 11. The NIR vertex compiler and the compiled-program draw path

`parhelia_vs_compile.c` compiles a GL vertex shader into a program the
WARP engine runs, and `ph_draw_tcl()` binds it.  This section is the
driver-facing contract; the shapes that run on hardware are in §11.5.

### 11.1 What the driver routes to a compiled program

Two hardware vertex routes now exist and they do not overlap:

1. **the kernel's resident transform programs** (§5.6) for the
   fixed-function shape `ph_vs_analyze()` recognises — position is a matrix
   product, every other output copies an attribute.  Unchanged.
2. **a compiled program** for a shader that is *not* that shape, behind
   `PARHELIA_HW_VS=1`.  The route is default-off until the GL comparison of
   §11.5 has run on hardware: half the opcodes it needs have BIN or INF
   evidence only, and a default-on path whose semantics are merely inferred
   is how an inferred field becomes a user-visible defect.  The
   position still has to be the matrix product, because the CPU evaluates it
   per vertex to decide whether the geometry fits the hardware clip volume
   (`ph_tcl_vertices_fit()`; the engine does not clip and the vendor never
   asked it to survive `w <= 0`).  Everything else - colour, the texture
   coordinate sets, fog - is compiled from its expression tree.

`ph_vs_analyze(nir, vs, relaxed = true)` is the same matcher with every
failure outside the position store demoted to "not a matrix term", which is
what makes the position requirement separable from the rest of the shader.

The matrix convention both routes share: constant-buffer vec4 *k*,
component *i*, scales input component *k* into output component *i*.  That
is GLSL's column-major `mat4 * vec4` and the fixed-function vertex program's
`MUL/MAD` chain; a shader written as four `dp4`s against rows would need the
transpose and is refused by the matcher, so `ph_tcl_matrix()` never sees the
other layout.

### 11.2 The expression set

Accepted NIR operations and the bundles they become:

| NIR | WARP | note |
|---|---|---|
| `mov`, `fneg` | folded into the operand | negate is an operand modifier |
| `fabs` | `max x, -x` | the engine has no absolute modifier |
| `fadd`, `fmul` | `add` 0x71, `mul` 0x75 | |
| `fadd(fmul(a,b), c)` | `mad` 0x76 | single-use multiply only; this is what keeps a matrix product at four bundles, since the vertex NIR options do not fuse |
| `fmax` | `max` 0x62 | PROVEN(hw) here: scene 3 uses it for `abs` and is bit-identical |
| `fmin` | — | **refuted**: 0x61 is not a componentwise minimum (§11.6), so `clamp()` and `min()` keep the draw module |
| `fdot2`, `fdot3`, `fdot4` | `dp3` 0x78 (z forced to the literal 0 for `fdot2`), `dp4` 0x77 | |
| `frcp`, `fdiv` | `smul` 0x89 against the literal 1.0 | the divisor must be one broadcast component, because 0x89 reads `s0.w` |
| `frsq` | 0x8a | same shape |
| `ffloor`, `ffract` | secondary 5 and 4 on a `wra` bundle | one source, port A |
| `vec2`/`vec3`/`vec4` | one temporary, one masked write per component; a component that is a single operation over leaves is written directly under its mask | |

A varying whose expression is one operation wide is computed straight into
its output register instead of into a temporary that is then copied, so the
copy only remains for a plain attribute read or a deeper expression tree.

Everything else is refused and the draw keeps Gallium's draw module.  There
are no branches: the upload validator rejects the control class (§9.2), so a
compiled program cannot loop.

Two structural rules the encoder forces and the compiler has to respect:
src1 and src2 share one swizzle field, and a bundle carries one
index-extension field.  A second offender is moved into a temporary with a
secondary `mov` first (`ph_vs_legalise()`).

### 11.3 Constants

The constant file is the engine's block-state file, so a program may read it
but never write it.  A compiled program's block is laid out by the compiler
and published by the ordinary constant loader (§7, route 1):

```
c0                              .. the shader's constant-buffer vec4s
c[constants.vec4s]              .. the literals the compiler spelled out
c[max(8, vec4s + literals)]     .. viewport scale  (W/2, -H/2, (zf-zn)/2, 1)
c[max(8, vec4s + literals) + 1] .. viewport offset (x0+W/2, y0+H/2, (zn+zf)/2, H)
```

A component that is only 0.0 or 1.0 costs nothing - swizzle codes 4 and 5
materialise both (§2) - so only genuinely arbitrary values consume a slot.
The viewport pair keeps the proven c8/c9 slots whenever the shader's own
constants leave room, and moves behind them when they do not; the raster
block's own copy at `c[0x160]` is emitted unchanged.

`p3d_state.vs_const` / `vs_const_vec4s` / `vp_slot` carry this to
`p3d_emit_constant_group()`.  One block load carries at most 63 vectors,
which is the driver's constant limit.

### 11.4 T2, the program store and the draw

* **T2 is never written by a compiled program.**  A transforming draw may
  sample a texture, and a textured program that writes T2 locks the engine
  (§3.3).  Mesa primes T2 once per context with `ph_prime_warp()`, exactly
  as the resident transform programs rely on.
* One program per shader, uploaded lazily on the first draw that would use
  it through `DRM_IOCTL_PARHELIA_WARP_PROGRAM`, released when the shader is
  deleted.  The entry the ioctl returns is the only one bound - the engine
  serves an already-executed entry from an instruction cache (§9.3).
* A slot holds `PARHELIA_PARAM_WARP_PROGRAM_INSTRUCTIONS` instructions and a
  file may hold `PARHELIA_PARAM_WARP_PROGRAM_SLOTS` of them (8), so at most
  eight distinct compiled shaders are resident at once; an upload that cannot
  be satisfied falls back to the draw module for that shader.  Instruction
  fetch is host DMA, so the image size is a driver policy choice: it was 32
  bundles when only the probe used the path and is now **128**, which is also
  the compiler's own `PH_VS_MAX_INSTRUCTIONS`.  The whole user heap is
  32 slots x 256 bundles x 12 bytes = 96 KiB of coherent memory.
* The vertex stream is the fixed-function transforming one, unchanged:
  position at dwords 0..2 (`v0`, `w` defaulting to 1.0), colour at 3..5
  (`v5`), coordinate set 0 at 6..7 (`v7`), set 1 at 8..9 (`v8`).  The
  shader's position input takes `v0` - the clip-volume bound reads those
  three dwords - and its remaining inputs take `v5`, `v7`, `v8` in attribute
  order.  A program that reads a slot the draw does not declare, or that does
  not write `o3`/`o4` when a coordinate set is advertised (§3.2), is refused
  for that draw.
* A program that writes no colour writes `o1 = 1,1,1,1` itself, so the
  fixed-function fragment stage always has an interpolated colour.

### 11.5 Verification

`parhelia-drm-test --selftest` compiles eight shader shapes with
`nir_builder` and checks each generated program against a reimplementation of
the kernel's upload validator, so an illegal program fails on the host rather
than at the ioctl.  It pins the constant layout, the folded matrix product
(four bundles for both the `dp4` and the GLSL column form) and the two refusal
cases (an operation with no opcode, a program that does not fit its slot).

It then **executes** one program: an evaluator over the field map of §2 and
the opcode semantics of §4 runs the bundles and compares `o0`, `o11`, `o1`
and `o3` against the shader's algebra computed directly.  A mis-encoded
swizzle, a source in the wrong operand slot or a wrong register allocation is
therefore a host failure rather than wrong pixels on the target; what the
evaluator cannot check is whether the *hardware* agrees with the documented
semantics of the opcodes that are only BIN or INF, which is what the GL
comparison below is for.

`tools/parhelia-gl-vs-test.c` renders twelve scenes through GLSL 1.20 vertex
shaders and `tools/parhelia-gl-vs-compare.sh` runs it twice - once with
`PARHELIA_HW_VS=1`, once with `PARHELIA_HW_VS=0` - and compares the dumps.

The scenes are **ordered by how well the opcodes they need are established**,
so the first scene that differs names the construct that disagrees with this
document rather than leaving a whole shader to bisect:

| scene | adds | evidence before this test |
|---|---|---|
| 0 | transform and a colour copy: `mul`, `mad`, `add`, `smul`, the constant block, the entry binding, the stream | all PROVEN(hw) in the resident programs |
| 1 | `mad` absorbing a literal multiply and add | PROVEN(enc), semantics from the viewport tail |
| 2 | `min` 0x61 and `max` 0x62 through `clamp()` | BIN |
| 3 | `dp3` 0x78, `max` as absolute value, a vec4 of scalars | PROVEN(enc) |
| 4 | `smul` 0x89 reciprocal of an arbitrary scalar | INF, strong |
| 5 | 0x8a through `inversesqrt()` | INF |
| 6 | secondary `frc`/`floor` through `fract()`/`floor()` | BIN |
| 7 | `o3` and a bound texture | PROVEN(hw) for the resident programs |
| 8 | a refused shader: the draw module must still own it | - |
| 9, 10 | a perspective cube, so `w` varies per vertex | |
| 11 | fixed function again in the same context | |

Each scene is flushed to the dump as it completes and its name is printed
before it draws, and a scene range can be passed to both the test and the
compare script, so a scene that locks the engine neither loses the earlier
results nor has to be guessed at.

### 11.6 Hardware results

Measured on the P690 (`192.168.1.111`) with the 128-bundle module of §11.4
loaded, X at depth 16, `PARHELIA_HW_VS=1`, and the engine primed by an
untextured draw first.

`parhelia-gl-vs-compare.sh` renders all twelve scenes twice and diffs:

| check | result |
|---|---|
| scene 0, the plumbing: upload, constant block at c0, viewport tail, entry binding, stream, one swizzled move | **0/65536 differing pixels**, identical hash |
| scenes 1, 3, 4, 5, 6, 7, 9, 10, 11 | 0/65536 differing pixels each |
| scene 2, `clamp()` | **36864/65536 differing pixels, worst delta 255** |
| scene 8, the refused shader | 0-8 pixels, varying run to run on the draw-module path in both runs; readback noise, not a vertex difference |
| every scene after `fmin` was removed | 0/65536 differing pixels |

**0x61 is not a componentwise minimum.**  Scene 2 was the only scene that
differed, and it differed on every pixel of the quad; scene 3 uses 0x62 for
`abs` as `max(x, -x)` and is bit-identical, so `max` is confirmed and the
refutation is specific to `min`.  The opcode is no longer emitted - a guessed
re-encoding is how an inferred field becomes a defect that surfaces in
someone's game hours later - and `clamp()`/`min()` keep the draw module.

Everything else the compiler emits is now PROVEN(hw): `mul`, `add`, the
folded `mad`, `max`, `dp3`, `dp4`, `smul` both in the viewport tail and as a
reciprocal of an arbitrary scalar, 0x8a through `inversesqrt()`, and the
secondary `frc`/`floor`.  The perspective cube scenes prove the divide and
the viewport tail with `w` varying per vertex.

Per-draw routing, counted from the bound entry in `PARHELIA_TRACE=1`:

| run | draws on compiled programs | draws on resident programs |
|---|---|---|
| `PARHELIA_HW_VS=1` | **8**, each its own user-slot entry | 4 |
| `PARHELIA_HW_VS=0` | 0 | 12 |

The four that fall back are the two refused shaders and the two draws that
arrive after the file's eight program slots are all taken - this test links a
fresh program object per scene, so it reaches
`PARHELIA_PARAM_WARP_PROGRAM_SLOTS` and the rest fall back.  That path is
therefore exercised too, and it renders identically.

The upload ABI with the enlarged image passes `--warp-program-probe`
(kernel, uploaded, rotated-swizzle and builder-generated draws all correct,
zero validator misses, alien and released entries `-EACCES`), and
`parhelia-gl-tcl-compare.sh` is 13/13 scenes at 0 differing pixels, so the
fixed-function transform route is undisturbed.

ioquake3 is unaffected by construction and by measurement: its stats line
reads `31.0 hardware-transform draws (0.0 compiled vertex programs)`, because
the game uses fixed-function GL and never presents a shader to the compiler.

**The route still defaults off.**  The one GLSL client not covered above is
the X server itself: glamor's vertex shaders are expected to be refused
because their position is not a matrix product, but that has not been
observed on hardware, and Xorg is the wrong place to discover otherwise.
Verifying glamor with the route on is what the default flip waits for.

## 12. Still open

* The value of base registers `b1..b3` and how they are programmed.
* Whether `o0` may carry clip space (would remove the divide/viewport tail).
* `w1[21:20]`, swizzle code 6, primary 0x64/0x65/0x67/0x68/0x69/0x72/0x7a,
  secondary 2/3/6/0xa/0xb.
* The exact ordering of 0x8b..0x8d (rcp / rsq / ?).
* Why the textured program locks up when it writes T2, i.e. what T2 really
  feeds.
* Re-encoding the control class in the assembler (`getidx`, `f2b = 0`
  arithmetic, relative `mov`), needed only if a driver-generated program
  ever needs a loop.

## 12. T2 is the setup unit's clip-space position (MEASURED 2026-10-03)

`parhelia-drm-test --colour-alpha-probe` (Mesa p690-raster `1ee794a`):
`COLOUR_ALPHA_T2="r g b [a]" COLOUR_ALPHA_T2N=6` draws two covering triangles
through the kernel pass-through, whose `mul T2, v5, r1.0000` leaves **T2 =
v5, the vertex colour** (the instruction does not zero it).  Then
`COLOUR_ALPHA_QUAD=0 COLOUR_ALPHA_PROG=1` draws a quad through the T2-free
RGBA program, which inherits that T2.  The quad draws if and only if the
inherited T2 lies inside the homogeneous clip volume:

| T2 written (r,g,b,a) | quad |
|---|---|
| (0,0,0,1) from an RGB colour (0,0,0), (1,1,1,1), (0,0,0,0.01), (0,0,0,2), (0,1,0,1) | drawn |
| (0,1,0,0), (0,0,0,0), (0,0,0,-1), (0,1,0,0.5), (1,0,0,0.5), (0.5,0.5,0.5,0.25), (2,0,0,1) | nothing |

So T2 is the clip-space position the setup stage trivially rejects against
(`-w <= x,y,z <= w`, `w > 0`).  This explains, in one rule:

* the RGBA pass-through rejecting a triangle whose colour has a channel above
  its alpha or alpha 0 (section 5.2's finding);
* T2-free programs (the RGBA program, the textured programs, the transform
  programs) drawing nothing whenever the last T2-writing draw - of any
  client, in any process - left an outside value (an RGBA colour with a
  channel above its alpha, or alpha 0, through the kernel pass-through);
  a later pass-through draw with an inside colour restores it.

  The power-on vertex-count failure (Mesa's 7-dword RGBA quads losing their
  second triangle after `parhelia-up.sh`) is **not** this: in that state the
  probe's own T2-free quads, large and small (`COLOUR_ALPHA_QXY="75 85"`),
  drew both triangles, and Mesa priming T2 with four distinct covering
  triangles did not help.  A native-target probe quad
  (`COLOUR_ALPHA_NATIVE=1`) cleared it - still open;
* vendor setup programs computing clip codes on T2 (`mtx1066`:
  `slt T2., T2.xxxx, -T2.yyyy` ...).

Never read a WARP input the vertex stream does not fetch: the probe below
stopped the engine.

Not yet working: writing an inside T2 from the RGBA program itself.  `add T2,
v0.0000, v0.0001`, `add T2, v5.0000, v5.0001`, `mul T2, v0.000w, r1.0000`,
`mul T2, v0.0001, r1.0000/r1.1111` all left the quad dropped (the
constant-swizzle digits do not give (0,0,0,1) in that slot).  `mul T2, v6,
r1.0000` (an input the stream does not fetch) **stopped the engine** (watchdog
power cycle 02:56) - never read an unfetched input.
