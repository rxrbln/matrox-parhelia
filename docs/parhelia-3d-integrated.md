# Parhelia 3D: the integrated model

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

One authoritative description of how a 3D command reaches the Parhelia-512
(0x0528) engine, consolidating eight sub-agent reverse-engineering documents
against what was proven on hardware on 2026-09-05/06. **Where a document and
the hardware disagree, the hardware wins and the document is marked.**

Tags: **[HW]** measured on the board, **[BIN]** read from a vendor binary,
**[INF]** inferred. Sources: `parhelia-3d-submission-audit.md`,
`-vertex-fetch-and-memory-classes.md`, `-warp-isa-complete.md`,
`-pixel-pipeline-complete.md`, `-engine-and-context-init-complete.md`,
`-draw-builders-kernel.md`, `-warp-programs-decoded.md`,
`-vendor-strings-and-constants.md`.

## 1. The exit criterion, met

A triangle was rendered from our own DMA submission on 2026-09-06: our vertex
bytes, our colour attribute, replayed through our ring. Verified by a
render-target diff beginning at exactly `0x2f654f8` — the vendor triangle's
first pixel — with the pixels reading `ffffff00` (our yellow) where the
vendor's read `ff00ff00`. Recipe: `tools/same-boot-replay.sh` with
`EXTRA="--ringvtx <file>"`. [HW]

Still supplied by the vendor at that point: the register state (captured on
the same boot), the WARP programs (bound by entry into the live client's
heap), and engine/context init (X plus a live GL client). §7 lists the order
in which those are being replaced.

## 2. Command submission — settled

| element | rule | evidence |
|---|---|---|
| doorbell | `0x404 = start & ~3`, then `0x408 = end \| class`; **one kick per packet** (`mtx1937`), entries advance `0x20` | [HW][BIN] |
| packet header | `(opcode<<24) \| (count<<17) \| ((totalDwords-2) & 0xffff)` | [HW][BIN] verified against the captured ring |
| opcode 0x00 | register list: `[hdr][tag][data…][tag][data…]`; a tag packs four indices LSB-first, `reg = 0x1000 + idx*4`, `0x20` PAD, `0x21` END | [HW][BIN] |
| header-less form | bare `[tag][data…]` with no header — what the primary ring carries between calls, and what the 2D fill uses | [HW] |
| secondary call | `0x20292820, 0, start\|st, end\|et, 0` → PAD, `0x10a0` SECADDRESS, `0x10a4` SECEND, PAD | [HW][BIN] |
| **call framing select** | the call's **START low bits pick the framing of the section**: `\|2` = header-ful opcode packets (the kernel pushbuffer, 234 of 243 calls), `\|0` = header-less bare tags (the X driver's 2D buffer, 9 calls). END is always `\|3` | **[HW]** |
| draw | `0x97080001 start end` — `0x17` + bit 31 (GL path, from `ctx+0x42c`), `[22:17]` = dwords per vertex − 1, operands are plain byte addresses, `end` exclusive | [HW][BIN] |
| address classes | address bits [1:0] = bus-cycle class: 0 local/VRAM, 2 AGP, 3 PCI; on this PCI-X board every non-local pointer is class 3, raw bus-physical, nothing registered | [BIN][HW] |

**Corrections this supersedes.** The audit's "opcodes must live in a secondary
buffer" and my own later "the secondary-call model is disproved" were *both*
wrong: the vendor uses calls, and the start tag selects the framing.
Relocating a header-less section as `|2` makes the engine parse `0x21afddc0`
as an opcode header and lock up — that was the stall at packet 37/55. [HW]

## 3. What the ring actually carries per draw

The primary ring is **not** just calls. Between them it carries, header-less,
the entire `mtx398` static block — texture units, `0x1300 = 0x002010cc`,
combiner slots `0x80..0x8a`, raster state, `0x132c = 0x10001000`, `0x12f4`,
`0x12b8`, `0x12b0` — plus four `0x707` syncs and three fence records. Replaying
only the secondary sections omits all of it. [HW]

## 4. DWGSYNC

DWGCTL (`0x1300`) low nibble 6/7 = a transient soft-trap **sync** opcode, never
cached (`mtx2434`: `((v&0xf)-6) <= 1` skips the shadow). Scope bits: `0x400`
pipe drain, `0x100`/`0x200` fence render-target surface A/B, `0x800` AA. So
`0x407` = drain only, `0x307` = fence A+B, **`0x707` = drain + both fences** —
the flush before a `0x12b0` surface rebind, ordering only, not needed to draw.
`0x12bc` protocol: tags step by 8; `tag|1` arms block-until-retire, the even
write is the value that lands in `0x41c` (read `& ~7`); nothing needs
acknowledging between syncs. [BIN][HW]

## 5. Vertex fetch

`0x1000+i*4` = per-stream base **| class**; `0x1040..0x104c` = strides as u16
pairs. On the **copy path** (`ctx+0x1cc == 0`, which is what a GL client hits)
`0x1000 = 2` and `0x1040 = 4` are constant placeholders and the real per-vertex
size travels in draw-header bits `[22:17]`; immediately before each `0x17`
packet the kernel re-emits `0x1000 = [[chan+0x58]+0x34]`, the pushbuffer's
class word (the captured `3`). `0x1064` = first usable index, `0x1060` = number
of vertices addressable. `0x10b4 = 0` then N packed attribute descriptors to
`0x10b8`. `0x10b0` primitive word = `mtx734[prim] | mtx3178[prim]<<12 | bit17`,
triangles `0x0002305c`. [BIN][HW]

Vertex data must live **outside** the executed command range (the vendor keeps
it in the gap between two calls): a float such as `20.0f = 0x41a00000` inside
the range parses as a `0x41` block-load header and locks the engine. [HW]

## 6. WARP

Programs are **not** in an on-chip store for a GL context: they live in a
1 MiB class-3 system-memory heap (`mtx2565`, 682 slots × 0x600) and the entry
value encodes a bus address, `entry = (bus/3) & ~3 | 3`, instruction index
`= (bus − slotbase)/12`. **Verified**: the four per-draw entries decode to
index 96 in their slots, and row 96 is the first non-zero row in each. [HW]
This refutes the init document's "such a value cannot be an entry".

Consequence: a captured draw image is valid only on the boot it was captured
on. Capture and replay on the same boot with the context alive, or relocate
the entry dwords to our own copy.

**ISA** (from the vs1.x compiler in `MTXPARD.dll`, `.text 0x191490`): 96-bit
dual-issue bundles — primary op `w2[30:23]` writing port B, secondary op
`w1[12:9]` writing port A; 2-bit register-file codes on every operand; one
shared swizzle for src1/src2 with owner `w2[22]`, src0 swizzle `w2[21:10]`;
relative addressing `w1[29:27]`/`w1[13]`. Outputs: **o0 = window-space
position**, o1/o2 = colours, o3+ = texcoords, o11 = clip position. [BIN][HW]

The per-vertex program (live slot `16c06200`) is a full transform even for
screen-space input: `add T0←v0`; 4× `dp4 r2←T0·c[a0.x+3..0] | mov o3..o6←v7..v10`;
`add o1←v5`; `smul r5←r2,r2.xyz1 | mov o11←r2`; `add o2←v6`; then a viewport
tail with `c[b1+0]`/`c[b1+8]` producing `o0`. `tools/parhelia-warpasm.py`
regenerates those 13 words bit-for-bit. Base registers `b0..b3 = e0/160/80/800`
and `a0 = 0..3` are installed by the kind-1 setup program. [HW]

`ctl33/ctl35/ctl0d/ctl27/ctl2b/ctl05`, `sop2/sop8`, `br`, `smul` are **not**
emitted by the D3D compiler and remain undecoded; the three setup programs
that use them exist verbatim in `mtx_parhelia.o` `.data` at `0x1643c0`,
`0x1646a0`, `0x1641e0`, with a 346-instruction corpus in `MTXPARM.sys`
`0x627b8`. No binary names any WARP mnemonic as text. [BIN]

## 7. Open, in the order they are being closed

1. Bind **our** generated program instead of the vendor's heap entry (the
   assembler already matches bit-for-bit; needs our program in a class-3
   buffer plus the kind-1 base-register packet).
2. Emit **our** register state instead of the captured sections
   (`parhelia-pixel-pipeline-complete.md` §4: flat and Gouraud are the *same*
   state — the hardware always interpolates; textured adds the unit block).
3. Engine and context init without X — including the WARP program upload via
   `0x918`/`0x91c`, which no capture covers.
4. Undecode-by-corpus of the `ctl*`/`sop*` ops (§6).

## 8. Contradictions resolved

| claim | source | verdict |
|---|---|---|
| opcodes only reachable via a secondary buffer | audit | **partly wrong**: calls are used, but the start tag selects framing; the primary ring also executes header-ful streams directly [HW] |
| the secondary-call model is disproved | my own earlier note | **wrong** — it failed because both operands were tagged `\|2` [HW] |
| `0x1130` bit 9 = WARP bypass | old docs, my tool | **refuted**: the vendor force-ORs `0x200` on every draw [BIN] |
| `reg = 0x1000 + slot*4` (→ `0x12a0`, `0x1058`, `0x1020`, `0x103c`, `0x101c`) | six docs | **refuted**: `0x12a0` is named in no vendor binary [BIN] |
| `0x12b0 = 0x3ff0f800` | draw-sequence, audit | transcription error for `0x3fff8000` [BIN][HW] |
| `0x300` arm formula | three docs | `0x300` is 16-bit: `(v & 0xffff) \| 0x140` [HW] |
| `0x8fc` selects DMA mode | engine-init doc, our `engine_init()` | it is the **IRQ mask** [BIN] |
| `0x400` is the ring limit | init agent | **contested**: under a live vendor context `0x400 = 0x1b466000` and the fence records write into `0x1b466020`, i.e. a status/write-back page [HW]. Treat as unresolved; program both consistently |
| `0x1100 = 0x08a57183` is instruction data, not an entry | init agent | **refuted** by the index-96 decode above [HW] |
| f2b = 1 on every arithmetic instruction | ISA doc | wrong: the stage/clipper class runs `f2b=0` [HW] |
| stage entries at indices 15/19 | my own note | wrong slot base; they are **111/115** [HW] |
| `0x1040` carries the byte stride (0x14) | memory-classes agent | wrong for the copy path: it is the placeholder `4` [BIN][HW] |
| the kernel never emits a block-load packet | session summary | wrong: `mtx1342` is reached per draw via `mtx693` [BIN] |

## 9. Method lessons that cost the most time

- A pixel count at a guessed address proves nothing. Judge a draw by a
  **full-VRAM diff with a changed input** (colour or position); a same-input
  diff changes nothing even when the draw works.
- A captured image is valid only on its own boot (§6).
- Pre-flight every dword range before the doorbell; keep data outside it.
- Never kill a live GL client, and never leave X frozen for more than a
  minute — both take the machine down (`parhelia-no-lockup-guardrails`).
