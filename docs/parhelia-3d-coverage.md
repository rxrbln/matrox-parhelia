# Parhelia 3D feature coverage map

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

The canonical index of how much of the 3D pipeline is mapped, ordered simplest
to most complex.  Scope is the Parhelia family (0x0527/0x0528, LX 0x2537/0x2538,
P690 0x2539).  **The M9 (0x0540) is a different render engine and is out of
scope here** -- do not carry findings across.

Grades:

- **SOLVED** — encoding known AND confirmed against a hardware test or capture
- **DECODED** — derived from vendor binaries or captures, never confirmed on hardware
- **PARTIAL** — some fields known, named gaps remain
- **UNKNOWN** — not investigated

Last revised 2026-09-05.

## Submission and memory

| area | grade | notes |
|---|---|---|
| Kernel ioctl ABI | SOLVED | nr=0 GET_MEMORY_INFO, nr=31 buffer create, nr=33 (0x4621) doorbell; confirmed on hardware |
| Region model, 7 mappings | SOLVED | control block, command record stream, vertex ring, command ring, texture heap |
| mmap cookie publication | SOLVED | in-band at control page +0x28, second slot +0x48; 7/7 cookies resolvable in both buffer-diff captures |
| DMA command ring | SOLVED | GFP_DMA32 **system memory**; `0x404=start`, `0x408=end\|3`. A ring in the framebuffer BAR overruns and locks the engine — see `parhelia-vram-ring-hazard` |
| Block-load packet framing | SOLVED [HW] | `0x41000000 \| (count<<17) \| (count*4+3)`; fetch pointer stops exactly at end. Confirmed on **both** 0x0528 and P690 |
| Generic packet header shape | DECODED | `[31:23]`=opcode, `[22:17]`=16-byte-group count, `[15:0]`=dword count, both as (n−1) |
| Fences / sync | PARTIAL | pair 0x12BC write-tag / 0x41C read-completion; `0x450` bit22 = indexed FIFO empty. Not exercised end-to-end |
| IRQ | DECODED | 0x8f4 status, 0x8f8 write-1-clear, 0x8fc mask |
| Engine reset | **UNKNOWN — and there is no known path** | vendor never strobes PCI config 0xE0; a locked engine survives rmmod and X restart. Only a reboot clears it |

## State delivery

| area | grade | notes |
|---|---|---|
| Two namespaces | SOLVED | region-2 GL **state slots** via `[key][count]` records vs. DMA-ring **registers** at `0x1000 + index*4`. A state slot does **not** name a register |
| Slot -> shadow -> hardware chain | SOLVED | full map in `parhelia-slot-translation.md`: dispatcher, 172-entry table, three delivery mechanisms, register layouts and value encodings |
| Record stream framing | SOLVED | `parhelia-state-slot-map.md`; every `reg = 0x1000 + slot*4` address derived from a region-2 slot is refuted |
| Key 0x05 / key 0x0c attribution | SOLVED | blend factors = key 0x05 0x09..0x0c (key 0x0c 0x13/0x14 write them in pairs); polygon mode = key 0x0c 0x08. See §7.2 of the slot map |
| Block file layout (384 entries) | PARTIAL | `0x000..0x0df` matrix palette, `0x0e0..0x15f` WARP source registers (`r = N − 0xE0`), `0x160..0x17f` fixed-function setup. Only the middle window is established |
| Lazy emission | SOLVED | state is emitted in validation order, not call order, and unchanged values are never re-emitted. Any differential sweep must account for this |

## Raster / fixed function

| area | grade | notes |
|---|---|---|
| Render targets, pixel formats | DECODED | surface slots 0x1220..0x123c |
| Texture storage, incl. rectangular | SOLVED [HW] | linear, proven; 32×16 occupies exactly 32*16*4, dense |
| Sampler registers | SOLVED | `0x11c0`-`0x11e8` + `0x1200`-`0x120c` and nothing else; field layouts in `parhelia-texture-path.md` |
| Texture upload path | SOLVED | staging ring + kernel memcpy; opcode 0x0a record, 0x48 bytes |
| Texture formats / filtering / wrap / multitexture | DECODED | from `CDPSTextureEngine::UpdateHardware` |
| Mipmapping | DECODED | |
| Non-power-of-two | SPLIT | storage SOLVED [HW]; sampling capability DECODED (TextureCaps POW2 clear, static only); the GL stack **rejects** NPOT outright as a GL-1.3 artefact; `np2Conditional` unreconciled |
| Depth / stencil | DECODED | slot run 0x35..0x3b contiguous; value enums confirmed |
| Blending, alpha test, logic op | DECODED | value-level enums confirmed; constant blend colour = opcode 0x35 record |
| Colour mask | DECODED | bit0=R..bit3=A; 0 blanks output |
| Culling / front face / polygon mode | PARTIAL | 2-bit fields confirmed; "cull off" is recorded as 1 in one place and 3 in another — **unreconciled** |
| Scissor | DECODED | 0x1328/0x132c. The old 0x2121a1a0 "guard-band scissor" reading is **retracted** (packed colour, dead code) |
| Guard band | DECODED | **±8160.0f**, three sources across two binaries, block index 0x174. Distinct from 0x12f4 (31.0/15.0) |
| Clipping, 6 user planes | PARTIAL | planes written to block 0x16c..0x171; which registers the shader reads is **unresolved** (see below) |
| Fog | DECODED | **hardware** — RasterCaps sets FOGVERTEX/FOGTABLE/FOGRANGE/WFOG/ZFOG. The CPU routine is a fallback |
| Polygon offset | DECODED | **hardware** — DEPTHBIAS + SLOPESCALEDEPTHBIAS; block register 0x178 |
| Anti-aliasing / multisample | DECODED | |
| Point size / line width | DECODED | CPU path is forced by **policy**: `caps[0x1c]`/`caps[0x1d]` hard-coded 0. Says nothing about the silicon |
| GL_POINT_SIZE_MAX | SOLVED | slot 0xa6 / reg 0x1298 = 2048.0f. Was mislabelled "guard band / viewport scale" — retracted |
| Occlusion query | **absent — closed** | `parhelia-occlusion-query-re.md`. No counter register exists: the complete MMIO read set of both Windows D3D HALs (27 offsets, 12 of them in the 3D block and all identity read-modify-writes) and of the Linux kernel HAL (14 offsets, **none** in `0x1000..0x13ff`) is enumerated, and no value is accumulated or differenced anywhere. Both D3D drivers refuse queries three ways: `GETD3DQUERYCOUNT`/`GETD3DQUERY` → `DDERR_CURRENTLYNOTAVAIL` (`MTXPARD 0x3e705`), DP2 `CREATEQUERY`/`ISSUEQUERY` are skip stubs (`0x16a4c0`), `D3dGetDriverState` is an unconditional `E_NOTIMPL` (`0xff990`). No GL ICD for the family — Linux **or** Windows — has a single query entry point, while `MSerX/MxMGLX.dll` (`DEV_0540` only) in the same installer has all eight |

## Shaders

| area | grade | notes |
|---|---|---|
| Fragment/pixel combiner ISA | SOLVED | full opcode/modifier/replicate/scale tables; decoder `tools/parhelia-fragdis.py` |
| WARP vertex ISA encoding | SOLVED | 3 dwords/instruction, field map from the vendor's own patcher table; 9-bit dstA |
| WARP source addressing | SOLVED | `{exthi(3):idx(4)}` = **7 bits, 128 registers**. The 5th bit of each source select is the **negate modifier**, not an address bit |
| WARP constant store | PARTIAL | block `0x0e0..0x15f` → `r = N − 0xE0`, six anchors + two dead-band alignments. **Collisions below r12 and for clip planes are unresolved** |
| WARP readback port | SOLVED | `0x910`/`0x938`/`0x914`; **kind=3 only** — the vendor's sole use. Other kinds take a different internal path and scanning them locks the engine |
| Vertex shader exposure | SOLVED | the unit is present and implemented in the ICD; `GL_VERTEX_SHADER_EXT` is disabled by four stores, **all writing 0**. Fenced off, not missing |
| WARP program binding | PARTIAL | 18-entry program table; a draw binds several entry points chosen from state. Mechanism known, never validated |
| Readback port as an execution detector | **UNUSABLE** | never shown to observe any operation we can provably execute; use VRAM instead |
| Block load taking effect | **OPEN** | a correctly framed block load changes nothing observable in the kind=3 space. Either it needs arming, or kind=3 reads the port-A destination file rather than the constant file |

## The critical path to a first triangle

1. **Does a block load commit, and what arms it?** Candidate: key 0x05 slot 0x12
   → `[0x1be8]` bit2, dirty `[0x1bec] |= 0x20`, believed to be the WARP
   vertex-shader mode enable. Its consumer in the HAL is not yet traced.
2. **WARP program binding** — entry points and the 18-entry table.
3. **Vertex submission** — float layout and the `0x920` stride.
4. **Replay harness executed on hardware** — the phase-2 exit criterion. Cookie
   resolution is done (7/7); execution is untested.

## Known-unreliable areas to re-derive, not trust

- Any address of the form `0x1000 + slot*4` taken from a region-2 slot id.
- The pipeline-key bit numbering in `parhelia-draw-recipe.md` (superseded).
- Anything that reads a differential sweep as call-ordered.


## Instrumentation status (2026-09-05, P690)

Use **VRAM** as the observable, not the readback port.

* **VRAM — reliable.** The 2D fill writes exactly 120000 pixels and they are
  read back correctly. A per-4KiB-page checksum diff taken around a submit
  costs 128 KiB instead of a 128 MiB copy and covers the whole aperture; exclude
  the live cursor page at `0xec0000`, and note an *absolute* colour scan finds
  the live desktop and is useless.
* **Readback port — unusable as an execution detector.** No stimulus we can
  provably execute has ever moved it. Any conclusion of the form "N registers
  changed / did not change" is a statement about the instrument.

Current WARP draw state, measured with the reliable observable: the engine
consumes the whole stream, `bmstat` leaves idle so work is genuinely dispatched,
and **0 of 32768 VRAM pages change** — the engine writes nothing anywhere.


## Update 2026-09-05: what the parallel RE pass settled

* **Record framing** -- `{key, count}` header, `size = 8 + count*itemsize`,
  validated to 100% on two captures and against the client's own declared
  length on four more. `parhelia-record-stream.md`.
* **Slot to hardware** -- the full chain, with concrete register layouts for
  depth/stencil, blend, alpha, colour mask, scissor and cull, plus the value
  encodings. `parhelia-slot-translation.md`.
* **Texture path** -- sampler register window, field layouts including LOD bias
  and clamps, the staging-ring upload, and mipmaps as an explicit per-level
  table. `parhelia-texture-path.md`.
* **Buffer-create ABI and its failure modes** -- one input field, a live PDEV
  handle, and three separate reasons allocation fails on this box.
  `parhelia-kernel-abi.md`.
* **Two vertex-delivery modes** -- index mode (`0x8f000001`, needs a bound
  array) and address mode (`0x17000001 | flags | stride<<17`, self-contained).
  Every from-scratch attempt here had used index mode with nothing bound.

**Texture state has no register path.** Texture environment, enables and
env colour are compiled into a WARP program; an open driver must generate it.
That is the largest single piece of work implied by this pass.
