Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# P690 WARP vertex ISA — encoding, upload path and a decode of every program we run

This document is the *driver-facing* WARP reference: the subset of the
instruction set that is backed either by a program this project runs on real
P690 hardware, or by a bit-exact re-encoding of such a program.  It
consolidates and re-grounds `parhelia-warp-isa-complete.md` (which reverses
the vendor D3D assembler statically) and `parhelia-vertex-isa.md`.

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

* the program must contain `END` within its word count, and nothing after
  the `END`+1 padding word is executed;
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

### 9.3 Entry-register enforcement

Independently of the upload path, `parhelia_validate_reglist()` now rejects
writes to reglist index 0x40 / 0x41 (`0x1100` / `0x1104`) whose value is not
an entry this kernel handed out — a resident/driver program entry, one of
the caller's own user slots, or the two literal binding values 0 and 2.
Without that check a render-node client could point the instruction fetch at
an arbitrary class-3 bus address, i.e. at arbitrary system memory, and have
the resulting "program" write the surface-base block.

---

## 10. Still open

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
