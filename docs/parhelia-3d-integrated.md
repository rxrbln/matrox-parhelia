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

## 6b. Measured since this document was first written

**GL state to command bits** — a synthetic variant matrix
(`tools/gltest.c`, `tools/capture-gl-matrix.sh`, `tools/diff-gl-variants.py`)
measured the encodings directly; full table in
`parhelia-gl-state-attribution.md`. All seven captured primitives match
`0x10b0 = mtx734[prim] | topology<<12 | 0x60000`, and the primitive **class**
is replicated in three places: `0x10f0`'s low nibble, `0x1130[25:24]` and
`0x1300`'s low nibble. Cull is `0x1130` bit 0 (not `0x12fc`), flat shading
`0x1130` bit 12, depth test `0x12c8` bit 0, blend `0x12e8`, alpha test
`0x12e4`, scissor max `0x132c = (h<<16)|w`. Point size is `c[0x172].w` =
half the size; polygon offset is `c[0x178]`; lights are 7-vec4 blocks from
`c[0x0E0]`. [HW]

**The steady state is four packets.** Between consecutive draws the vendor
emits only `0x10b0`, `0x1000 = 3`, `0x1040 = 4` and the draw — byte-identical
in 931 of 956 measured gaps. Everything else in a frame (529 writes across 104
registers, ten block loads, the priming pseudo-draws, X's 2D blits, the
flushes and the fence) is context or redundant. [CAP]

**Block loads are consumed through the vertex-fetch frontend.** The
`[count][first][1][0]` sub-header is the `v0` the constant-loader microprogram
reads, so a one-attribute pipe must be programmed immediately before them
(`0x1000=0, 0x1040=4, 0x10b0=0x0002305c, 0x10b4=0, 0x10b8=0x83c00000`) and the
real vertex format restored after. Omitting this is what stalled our generated
stream at its first block load. [BIN][HW]

**Gouraud interpolates in hardware.** A per-vertex-colour client's own frame
shows a genuine gradient. An earlier three-colour draw of ours came out
uniform because it replayed `gltri`'s state, and `gltri` uses one flat colour —
so nothing in that capture configures interpolation. With all three vertices
the same colour, flat and smooth are indistinguishable, which is why every
single-colour test had looked correct. [HW] See
`parhelia-gouraud-interpolates.md`.

**Coordinate space when replaying.** A replayed stream carries the captured
client's projection, so vertices must be supplied in ITS window coordinates
(0..320 x 0..240 for `gltri`), not NDC. NDC vertices are clipped away entirely
and draw nothing. [HW]

**The render target is per client, not merely per boot.** `gltri` renders at
~`0x2f654f8`; a different client at ~`0xee7c60`. A diff aimed at the wrong one
reports a silent zero. Derive it per client (`tools/prep-draw.sh`). [HW]

### Measured 2026-09-06

**Vertex format — ground truth, not inference.** With the GL client frozen, the
draw packet's operand address was read back from `/dev/mem`. For a 3-vertex
`GL_TRIANGLES` draw with header `0x97080001` (5 dwords/vertex):

```
bf333333 bf19999a 3f800000 00000000 00000000   -0.7 -0.6  1 0 0
00000000 3f19999a 00000000 3f800000 00000000    0.0  0.6  0 1 0
3f333333 bf19999a 00000000 00000000 3f800000    0.7 -0.6  0 0 1
```

The layout is **x, y, r, g, b** floats. This one read refuted three separate
suspects at once (draw stride, triangle winding, coordinate space). The vertex
bytes sit immediately BEFORE the secondary command section in the same class-3
buffer (`16a20010..16a2004c`, section starts `16a2004e`). [HW]

**The primitive table is complete — all ten GL primitives.** [HW]

| GL primitive | `0x10b0` | class | DWG nibble |
|---|---|---|---|
| `POINTS` | `0x00060020` | 0 | `0xd` |
| `LINES` | `0x0006103d` | 1 | `0x5` |
| `LINE_STRIP` | `0x0006203d` | 1 | `0x5` |
| `LINE_LOOP` | `0x0006203d` | 1 | `0x5` |
| `TRIANGLES` | `0x0006305c` | 2 | `0x4` |
| `TRIANGLE_STRIP` | `0x0006405c` | 2 | `0x4` |
| `TRIANGLE_FAN` | `0x0006505c` | 2 | `0x4` |
| `POLYGON` | `0x0006505c` | 2 | `0x4` |
| `QUADS` | `0x0006605c` | 2 | `0x4` |
| `QUAD_STRIP` | `0x0006705c` | 2 | `0x4` |

`LINE_LOOP` reuses the `LINE_STRIP` word (the ICD appends the closing vertex)
and `POLYGON` reuses `TRIANGLE_FAN` (decomposed to a fan) — there is no
distinct hardware encoding for either.

**Surface descriptors immediately before the draw.** Decoding the span in
execution order, the last writes before the draw's DWGCTL `0x00a60ec4` are:

```
slot0  0x1220 = 02000000   0x1224 = 32020780     <- low 16 bits = 0x780 = 1920 px pitch
slot1  0x1228 = 01000000   0x122c = 36020780
slot2  0x1230 = 00000000   0x1234 = 32020000
slot3  0x1238 = 00000000   0x123c = 32020000     <- pitch 0: not a usable target
```

Two things follow. DWGCTL bits[11:10] = 3 would select slot 3, which has pitch
0 — so **that slot decode is wrong** and the destination is one of slots 0/1.
And the base values are **not raw VRAM byte offsets**: reading VRAM at
`0x02000000` finds the client's own frame absent (all zeros). The base/cfg
encoding is the open blocker; until it is known, no draw can be verified.
Format looks like `cfg = [flags:8][fmt:8][pitch_px:16]`. [HW]

**A replay only draws while the capturing context is ALIVE.** The span
references the capturing client's WARP entries, block file, vertex buffers and
surface descriptors. If X or the client has exited, the replay still consumes
every packet, reports idle, and the 2D instrument still measures 120000 px —
while rasterising nothing. There is no error signal. Assert `pgrep -x X` and
the client before treating any zero-pixel result as a negative. X and the held
client must be started with `setsid ... < /dev/null &` or they take a SIGHUP
when the ssh session that started them exits. [HW]

### Gouraud triangle from our own vertices — 2026-09-06 [HW]

Proven by a controlled colour rotation, not by a pixel count:

| vertices supplied | apex `0x2fa1020` | bottom row `0x3092020` |
|---|---|---|
| apex green | `ff02fa02` (green) | `ff1c34af` blue-dominant |
| colours rotated, apex red | `fffb0201` (red) | `ff35ae1c` green-dominant |

The readback followed our vertex colours in both places and the bottom row
interpolates smoothly (`1c34af -> 1b34b0 -> 1a34b1 -> 1834b3`). Ours are the
vertices and the primitive; the state and WARP program are still the captured
vendor context.

**Locating the render target** — the step that unblocked verification. Render a
uniquely coloured triangle from the client (`GLTEST_RGB=0xff00ff`), then search
in capped 32 MB slices (`--findcolor ff00ff --scanbase B --vramscan 0x2000000`).
Found 16128 px, exactly the count predicted from the decoded 320x240 viewport.
Never sweep the whole BAR in one pass (guardrail: it has half-killed the machine
three times).

### Surface addressing — merged and cross-checked [BIN][HW]

Three agents reported on this and two of them were wrong in ways that only the
measured triangle could settle. Resolved:

- **Base is a plain byte address**: `base = allocator_offset | class_tag`, tag
  in bits[1:0] (0 local, 2 AGP, 3 PCI/system). No shift, no MMU. All agents
  agree; `MmSurface::AttachToBlock` is a single OR.
- **The 3D render target is TILED**: 64x32 pixel tiles, 8192 bytes per tile.

  ```
  addr(x,y) = base + (y>>5)*(W/64)*8192 + (x>>6)*8192 + ((y&31)*64 + (x&63))*4
  ```

  Verified against the measured triangle: first/last non-black pixels
  `0x2fa1020..0x3095f7c`, span 1003356 B. A linear stride cannot fit it (7680
  gives 130.65 rows, 8192 gives 122.48, both need ~144). The tiled formula with
  apex (136,48) gives base `0x2f60000`, which is **tile-aligned** — no linear
  model produces an aligned base. Corroborated in the binaries by the 32 px
  height granularity and 0x2000 alignment for types 2/3.
- **`next_pow2(width)*bpp/8` is the HOST APERTURE (CPU) pitch, not the engine
  layout.** An earlier report offered it as the scanline stride and a second
  report repeated it; both are superseded. It is why a flat BAR copy of the
  render target to the linear scanout comes out scrambled into blocks.
- **DWGCTL bits[11:9] select nothing on the 3D path.** They are part of the
  unconditional 3D constant: `0x860e00 | 0x200000 | 0xc4 == 0x00a60ec4`
  exactly. The 2D map (`[11:10]` dst, `[9:8]` src) is real for opcodes 0-3/0xC
  and misleading for opcode 4. The "destination is slot 3" reading is refuted.
- **The 3D destination is slot 0 (colour) and slot 2 (depth), bound by the
  KERNEL on the PRIMARY path**, via header `0x8d8c8988` followed by
  `[colour_base, colour_cfg, depth_base, depth_cfg]`. It is never in a client
  secondary section — which is why a walk of every captured secondary section
  found no value near the real target, and why the span's `slot0 = 0x02000000`
  (the desktop surface) is not where the draw lands. The kernel's write comes
  after the client's and wins.

cfg word: `[13:0]` width px · `[18:16]` type (0 linear buffer, 1 texture,
2 render target, 3 primary/displayable, 4/5 tiled) · `23` linear/tiled ·
`[29:24]` format · `[31:30]` tiling. `0x32020780` = A8R8G8B8 type 2 w1920;
`0x30030780` = the primary/desktop.

Slot mapping is `slot N = 0x1220 + N*8`, confirmed against raw packet bytes
(`SurfDesc2d::Init` pairs `slot<<10` with header `0x21218988`, LSB-first =
indices `0x88,0x89` = `0x1220/0x1224`). A claimed 0<->3 reversal was checked
and **withdrawn by its author**.

Engine offset space is flat: `[0, 0x08000000)` local VRAM, above that
non-local, bits[1:0] = bus class.

### Engine and context init [BIN]

- **`0x400` is the write-back/status page base**, not a ring limit — it holds
  the physical address of a 0xae0-byte shared buffer, and the fence lands at
  `phys+0x20`. That resolves the long-standing `0x1b466000`/`0x1b466020`
  contradiction. There is no ring: submission is a start/end span.
- **`0x8fc` is the interrupt mask** (confirms the earlier correction).
- **WARP upload**: `0x918` = instruction index, `0x91c` = data port, and
  **`0x920` bit 0 selects the on-chip store vs a system-memory heap** — which
  reconciles the two conflicting readings we carried. Two agents agree.
- **The block file has no default image.** All 384 vec4 are zeroed at context
  create and only 14 slots are written; `c[0..3]` (the MVP) is **not** among
  them, so a cold context transforms every vertex to the origin. Anything
  standalone must load the MVP itself.
- Methodology: `mtx_drv.so` has an obfuscated `.symtab` but its `.dynsym`
  carries 1039 real mangled C++ names — use `nm -D`, not `nm`.

### Block (constant) load packet — exact encoding [HW]

```
header     = (0x41 << 24) | (nslots << 17) | (3 + nslots*4)
descriptor = [ nslots, firstSlot, 1, 0 ]
data       = nslots * vec4
totalDwords = 5 + nslots*4
```

Verified against all eight loads in the captured span (1, 2, 4, 5 and 8 slots:
headers `41020007`, `4104000b`, `41080013`, `410a0017`, `41100023`). Two of the
ten `0x41` packets are not loads but preformatted blobs; blob 9's descriptor
`(0xE0, 0x160, 0x80, 0x800)` is the WARP constant-base bind.

Implemented as `3dtri --viewport X0 Y0 W H`, which rewrites the `c[0x160]`
(scale) / `c[0x161]` (offset) pair in the span's own load. Measured effect on a
fixed NDC triangle: 320x240 -> 15140 px, 160x120 -> 3083 px, 80x60 -> 208 px.
The count follows our values, so 3D state is under our control and not merely
inherited. It does not scale exactly quadratically - `c[0x161].w` is the
viewport height and the WARP program writes it into `o0.w`, so changing it also
moves the clip behaviour.

**Walk sections by their own framing when patching.** The secondary image is a
concatenation of sections whose call start tag selects header-ful (`|2`) or
bare-tag (`|0`) framing; scanning it as one packet stream desyncs and finds
nothing.

### Primitives rendered from our own vertex data [HW]

Nine of the ten GL primitives rasterise from vertices we supply (NDC,
`x,y,r,g,b`), no stalls, engine idle throughout:

| primitive | px | primitive | px |
|---|---|---|---|
| points | **0** | tristrip | 26696 |
| lines | 58 | trifan | 17007 |
| linestrip | 170 | quads | 31323 |
| lineloop | 170 | quadstrip | 35840 |
| triangles | 15140 | polygon | 23539 |

`lineloop` matches `linestrip` exactly, confirming they are the same hardware
primitive. Points draw nothing even though `c[0x172].w = 0.5` (point size 1.0)
is already set, so the cause is elsewhere - likely the DWGCTL opcode nibble
(0xd for points vs 0x4 for triangles) or a different WARP program.

### Render target tiling — SOLVED [HW]

```
tile  = (y>>5)*TPR + (x>>6)        TPR = surfaceWidth/64 (30 at 1920), 8192 B/tile
tx = x & 63, ty = y & 31
index bit 0,1,2  = tx0,tx1,tx2   |  bit 3 = ty1  |  bit 4,5 = tx3,tx4
index bit 6      = ty0           |  bit 7 = tx5 XOR (tile_row & 1)
index bit 8,9,10 = ty2,ty3,ty4
addr = base + tile*8192 + index*4
```

Verified at **0.59/255 mean colour error with zero pixels outside** the
triangle. The `tile_row & 1` term is what makes every second band of tiles land
32 px off when omitted - the "interlaced" corruption seen on screen.

Method note: a calibration quad spanning only 2x2 tiles produced a
self-consistent but wrong fit twice. Two techniques settled it: **pairwise bit
analysis** (pixel pairs whose addresses differ in one bit; median position
delta - immune to the distribution of pixels, unlike per-bit means) and a
**per-tile right/wrong split**, which showed errors were 0% or 100% per tile
and confined to even tile rows.

With the correct decode the Gouraud triangle is pixel-accurate; **1004 px (6%)
are genuinely missing in the lower right** (tiles 153/154), against a vendor
draw of the same vertices that covers 16128 of 16144.

### Our own command stream — DONE [HW]

`tools/gen-stream.py` emits a ring/sec/reloc triple that `3dtri --ringreplay`
executes unchanged; `tools/verify-draw.py` de-tiles the result and compares
coverage and per-pixel colour against exact barycentric interpolation. Every
layer below was verified to render **pixel-identically to the vendor stream**:

```
pixels 15140 (ideal 16144, missing 1004) | outside 0 | colour err mean 0.59 | PASS
```

| layer | status |
|---|---|
| ring, secondary calls, packet framing | generated |
| all 525 register writes | generated |
| all 9 block-file loads | generated (byte-identical to the vendor's) |
| draw packet | generated |
| vertices + primitive | ours |
| WARP vertex program | **ours** (11 bundles, own entry) |
| the 2 preformatted blobs | **dropped - measured unnecessary** |

**34 packets, no vendor bytes**, against the vendor's 154 (X's 2D traffic is
dropped entirely).

Traps that cost runs:
- the WARP entry `4*(bus/12)|3` only round-trips from a **12-byte aligned**
  image; a page-aligned DMA buffer is not one (4096 % 12 == 4);
- our program addresses the viewport **absolutely at c8/c9** while the vendor's
  reads it through a base register at `c[0x160]`, so an extra block load must
  copy those two vec4 across - without it the program binds, executes cleanly
  and draws nothing;
- the opcode-0 header is `totalDwords - 2`, not `totalDwords - 1`.

Caveat: the blobs are droppable because the live vendor **context** has already
established that state. A cold context may need them. Engine/context init
remains the outstanding dependency.

## 7. Open, in the order they are being closed

0. **Decode the surface base/cfg encoding** (`0x1220..0x123c`) and the real
   DWGCTL destination-slot field. This now gates everything: a draw whose
   output location is unknown cannot be judged.
1. Bind **our** generated program instead of the vendor's heap entry. The
   program and the binding recipe are now RE'd: vendor Gouraud program =
   96 bundles applying MVP, perspective divide and viewport (`o0` leaves in
   **window pixels**, `o1` = colour passthrough); our 11-bundle replacement is
   generated and round-trips. Bind = `0x1374` strobe, `0x10f0` MODE
   `0x20000002` twice, `0x1100` = entry, `0x10f8`. `entry = 4*(bus/12) | 3`,
   and the buffer must be **12-byte aligned** (a page-aligned buffer is not).
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
| `0x1130` bit 9 = WARP bypass | old docs, my tool | **refuted** as a bypass, but the `\| 0x200` is **conditional**, not forced: `mtx3400` @0x4433c sets it only on a device-flag branch, and the 0528 capture has bit 9 **clear** (`0x0200200c`) — see `parhelia-fragment-gating-re.md` §4.1b [BIN][CAP] |
| `reg = 0x1000 + slot*4` (→ `0x12a0`, `0x1058`, `0x1020`, `0x103c`, `0x101c`) | six docs | **refuted**: `0x12a0` is named in no vendor binary [BIN] |
| `0x12b0 = 0x3ff0f800` | draw-sequence, audit | transcription error for `0x3fff8000` [BIN][HW] |
| `0x300` arm formula | three docs | `0x300` is 16-bit: `(v & 0xffff) \| 0x140` [HW] |
| `0x8fc` selects DMA mode | engine-init doc, our `engine_init()` | it is the **IRQ mask** [BIN] |
| `0x400` is the ring limit | init agent | **contested**: under a live vendor context `0x400 = 0x1b466000` and the fence records write into `0x1b466020`, i.e. a status/write-back page [HW]. Treat as unresolved; program both consistently |
| `0x1100 = 0x08a57183` is instruction data, not an entry | init agent | **refuted** by the index-96 decode above [HW] |
| f2b = 1 on every arithmetic instruction | ISA doc | wrong: the stage/clipper class runs `f2b=0` [HW] |
| stage entries at indices 15/19 | my own note | wrong slot base; they are **111/115** [HW] |
| surface scanline stride is `next_pow2(width)*bpp/8` | two agents | **superseded**: that is the host-aperture CPU pitch; the engine stores the 3D target in 64x32 px / 8192 B tiles, proven by an exact fit to the measured triangle and a tile-aligned base [HW] |
| 3D destination is surface slot 3 (DWGCTL bits[11:10]) | fragment-gating agent, my own decode | **refuted**: `0x860e00\|0x200000\|0xc4 == 0x00a60ec4`, so bits[11:9] are constant on the 3D path; destination is slot 0 colour / slot 2 depth, bound by the kernel on the primary path [BIN][HW] |
| `0x1040` carries the byte stride (0x14) | memory-classes agent | wrong for the copy path: it is the placeholder `4` [BIN][HW] |
| the kernel never emits a block-load packet | session summary | wrong: `mtx1342` is reached per draw via `mtx693` [BIN] |

## 9. Method lessons that cost the most time

- **A VRAM diff includes our own clear.** A diff that exactly matches a
  triangle-sized footprint may be the clear *erasing* the previous frame; the
  pixel VALUES are the evidence, never the diff alone. This produced one false
  positive in this project.
- **Check that a binary is non-empty before trusting a null result.** A crash
  during a build left a 0-byte client that ran, printed nothing and rendered
  nothing; two capture attempts were spent before the file size was checked.
- **Bench one variant per run and persist each result** before starting the
  next; the box crashes often enough that batches lose everything.
- A pixel count at a guessed address proves nothing. Judge a draw by a
  **full-VRAM diff with a changed input** (colour or position); a same-input
  diff changes nothing even when the draw works.
- A captured image is valid only on its own boot (§6).
- Pre-flight every dword range before the doorbell; keep data outside it.
- Never kill a live GL client, and never leave X frozen for more than a
  minute — both take the machine down (`parhelia-no-lockup-guardrails`).
- **Match the instrument to the signal.** `--findcolor ff0000` reports "none"
  for a *correct* Gouraud triangle: only a corner pixel is pure. Judge a
  gradient by its values across the footprint, not by exact-colour search.
- **Verify the flag you think you are using actually does the thing.**
  `--replayrt` only sets the diff/report base; the surface is patched by
  `--rtbase`/`--rtcfg`/`--rtslot`. Several runs "re-pointed" nothing while I
  read the result as a hardware negative. Grep the tool's own confirmation line
  rather than assuming.
- **A cached address is a per-boot fact.** `rt.txt` held a render target from
  an earlier boot, so every diff read zero. Stamp such caches with
  `/proc/sys/kernel/random/boot_id` and invalidate on mismatch.
- **A sweep that runs a binary the script never builds reports a clean zero.**
  `prep-draw.sh` swept with `gltri` while building only `gltest`.
- **Resume the client, not just X.** Scripts that `kill -STOP $GP $XP` and then
  `kill -CONT $XP` leave the GL client stopped; the vendor kernel driver waits
  on it and the machine stops responding.
