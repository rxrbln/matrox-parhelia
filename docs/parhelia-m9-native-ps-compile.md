<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 offline D3D pixel-shader compile: chain, boundary and native streams

This note drives the saved Windows `MxMD.dll` D3D pixel-shader compiler on the
host, with no GPU, to convert small `ps_2_0` token streams toward native
two-dword slots.  It answers where the compile entry chain is, what minimal
state it needs, exactly how far a token-only compile gets, and how the result
relates to the measured M9148 fragment-program facts in
`parhelia-m9-native-fragment-program-re.md`.

Labels: **RUN** = executed offline through the pinned `MxMD.dll`
(SHA-256 `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`);
**STATIC** = read from that image with objdump; **MEASURED** = an M9148 result
from the cited hardware note.  Addresses are `MxMD.dll` VAs (imagebase
`0x10000`).

## 1. The D3D pixel-shader compile chain (STATIC, RUN-confirmed)

The self-contained pixel entry is `0x10e590` (`ret 0x18`); the twin vertex
entry is `0x10e4a0`.  `0x10e590(eax = token blob, arg0..arg5)` builds its own
compiler context, parses the D3DSIO tokens, and runs the pixel frontend and
optimizer twice (flags `0x40` then `0x4a`) before the DDI finalizer `0x10ec20`.
The harness drives the same steps individually so the context is inspectable:

| step | routine | role |
|---|---:|---|
| context ctor | `0x190850` | zero a 0x624-byte context; `esi = ctx`, returns `esi` |
| token parse | `0x12a4c0` | decode version + body D3DSIO tokens into the instruction container (reader vtable `0x1f4aa4`, per-token decoder `0x1515b0`, opcode jump table `0x152878`) |
| pixel frontend | `0x1922a0` | `(ctx, container, caps, cfg, mode)`; builds the graph, calls `0x194d70` per record |
| optimizer | `0x1958a0` | ~15 analysis/transform passes, then whole-program backend |
| backend | `0x1d1d40` | lowers optimized instructions and drives the emitter |
| emitter / packer | `0x1db510` / `0x1db840` | pack each instruction into the output-stream object (vtable `0x1f50bc`, slot count `+0x04`, slot pointer `+0x08`) |

Minimal state the compile needs (STATIC, RUN-confirmed): only the global
allocator at `ds:0x20d45c` (`vtable[6]=alloc`, `[7]=free`) and two small config
objects for the frontend — a caps object whose byte `+0x120` holds
partial-precision/centroid flags, and a const-bank object read through method
`vtable[+4]` and a `+0x18/+0x1c` vector (both may be zero).  No device, surface,
or handle table is dereferenced for the compile itself.

The input blob is the Matrox reader form `{version, length_dwords, <D3DSIO
body>, END}`: the header cursor gives `end = base + length*4`, the body is
standard D3DSIO decoded by `0x1515b0`.  `ps_2_0` version dword is `0xFFFF0200`.
The assembler is embedded in `tools/m9-native-ps-compile.py`; the exact token
blobs are pinned in `captures/m9-native-ps-compile-20260915/tokens.json`.

## 2. Harness and isolation

`tools/m9-native-ps-compile.c` (built `-m32 -O0 -no-pie`) maps the pinned image
at its preferred base, installs a recording allocator, and calls the four
routines above with audited inputs.  `tools/m9-native-ps-compile.py` verifies
the SHA-256, assembles the shaders, runs the helper, and pins the per-shader
diagnostics with `--write` / `--check`
(`captures/m9-native-ps-compile-20260915/results.json`).  Multilib i386
(`gcc -m32`, non-PIE) is available on the host and required.

## 3. RUN result: the emission boundary

Every shader parses and passes the pixel frontend, then the optimizer eliminates
the whole program and the backend emits nothing.  Pinned, deterministic
(`results.json`):

| shader | parse | records | frontend | out_link | in_link | endpoints | optimizer | program |
|---|---|---|---|---|---|---|---|---|
| `mov oC0,c0` | ok | 1 | ok | 0 | 0 | 0 | `0` | none |
| `mov oC0,v0` | ok | 1 | ok | 0 | 0 | 0 | `0` | none |
| `add r0,v0,c0; mov oC0,r0` | ok | 2 | ok | 0 | 0 | 0 | `0` | none |
| `texld r0,t0,s0; mov oC0,r0` | ok | 1 | ok | 0 | 0 | 0 | `0` | none |
| `texld;mul;mov oC0` | ok | 2 | ok | 0 | 0 | 0 | `0` | none |
| `texkill t0` | ok | 1 | ok | 0 | 0 | 0 | `0` | none |
| `mul r0,v0,c0; mov oC0,r0` | reject | 0 | ok | 0 | 0 | 0 | `0` | none |

**Root cause (RUN/MEASURED, read from the live context):** the input/output
varying link tables at `ctx+0x24/+0x28` and `ctx+0x34/+0x38` are empty and the
`0xfff8` endpoint counter `ctx+0x5a8` stays zero.  With no output link, converter
`0x1a34e0` maps the `oC0` write to the absent-descriptor sentinel `0xf0000000`,
so the optimizer's dead-code elimination drops it: the whole-program backend
`0x1d1d40` receives an empty instruction list (`0x1d2452` count = 0) and returns
0.  This is exactly the external varying/output linkage that
`parhelia-m9-program-stage-entry-re.md` shows the GL/D3D linker supplies (opcode
`0x59`/`0x67` records, remap table at `+0x5bbc`); a bare token compile does not
carry it.

Seeding one output-link entry (`M9_PS_SEED`, writing `ctx+0x34/+0x38`) is
accepted by the frontend (`out_link` becomes 1) but does not by itself set the
endpoint counter or restore emission: the frontend still does not convert `oC0`
to the special output class.  So the remaining input is the pixel-shader output
declaration/render-target state the frontend keys on, not merely a link entry.
No result was fabricated to cover this gap.

Two secondary facts: `MUL` (`0x15171b`) enforces operand-class checks
(`[esi+0x50]&0x1c0 == 0x40`) that a plain `mul r0,v0,c0` fails, so it is rejected
at parse while `ADD` (generic handler `0x152869`) is accepted; and the assembler
uses the standard D3DSIO register-type split, verified against the parser's own
nibble decode.

## 4. Native encoding per operation (RUN corpus) + measured output

Because the whole-program emit is gated on linkage, the exact native slots for
each operation come from the per-instruction lowerer corpus in
`parhelia-m9-native-lower-runtime.md` — the **same** backend
(`0x1d1d40`/`0x1db510`/`0x1db840`), run per instruction on synthetic
descriptors [RUN].  A driver backend concatenates these with the measured
fragment output MOV and terminator.  The arithmetic bodies (identity-swizzled,
full-mask, class-0 result) are:

```text
MOV  : ffc0001c a2803f1f ; ffc0811c a2803f1f                 (2 slots)
ADD  : e004001d b05a800f ; ffff801f 0000000f (x2) ;
       dfc4001c ac800d00 ; ffc4011c ac800d00                 (6 slots)
MUL  : e004001d b05a800f ; ffff801f 0000000f (x2) ;
       c23f801c ac200100 ; e23f811c ac200100                 (6 slots)
TEXLD: ffff8000 0000000f ; ffc0001c a2803f0f ; ffc0011c a2803f0f
       (3 slots, zeroed resource metadata; real unit/dimension unresolved)
CMP  : e004001d b05a800f ; ffff801f 0000000f (x2) ;
       e004001d b05a803f ; ffff801f 0000000f ;
       c205005c af400d10 ; e205815c af400d10                 (8 slots)
```

The **measured** fragment color output and terminator (M9148,
`parhelia-m9-native-fragment-program-re.md`) that terminate a real fragment
program are:

```text
output MOV : W0 = 0xffc00000 | c<<15 | 0x2d<<8 | 4<<5 | op0   ; op0 1d -> R,G ; 1e -> B,A
             W1 = 0xa2803f1f  (source selector c)             ; a280331f | e<<14 (constant pair e)
terminator : ffff801c a01fc00f
```

These are **not** the same as the corpus's class-0 output (`W0[7:5]` routing 0)
or the compiler's synthetic terminal `fffc7e1d a2803f0f` (STATIC, endpoint
selector `0x7e`).  The fragment stage uses output routing class 4 with physical
destination selector `0x2d`, and a distinct stop slot.  Reconciling those is the
first thing an offline full compile would settle (see 7).

## 5. Fragment register class -> native selector

| class | compiler descriptor (STATIC) | low selector (STATIC) | fragment hardware (MEASURED) |
|---|---|---|---|
| `r` temporary | `2*i` | `2*i` | **unknown** — class-0 dest `0x00/0x01` locked up the engine |
| `v` input / varying | `0x10000000 + 2*p` | `2*p` | provoking-vertex color `c=0x44` (x,y) / `0x45` (z,w); general varying selectors unknown |
| `c` constant | `0x20000000 + 2*i` | gated `W1[20:14]` | 8-byte raster constant entries `8d..8e`; `e = pair index`, `W1[11:10]=00` |
| `s` sampler | `0x70000000 + i` | `0x70 + i` | four full fragment sampler blocks, indices `0x70+unit` |
| `o` output | `0xffd8 + 2p` / special `0xfff8` | `0x58 + 2p` / `0x78` | color output routing **class 4**, destination selector **`0x2d`**, op0 `1d`=R,G / `1e`=B,A |
| terminal | endpoint `0x7e` (`W0[14:8]`) | `0x7e` | stop slot `ffff801c a01fc00f` |

Confidence: the compiler-side descriptors and selectors are STATIC/RUN and
exact for the synthetic-descriptor corpus.  The fragment-hardware column is
MEASURED but incomplete: the temporary selector range, general varying/texcoord
selectors, and the map from the compiler output class (`0x58/0x78`) to the
measured fragment output selector (`0x2d`) are all unresolved.  The offline
compiler is the intended instrument to resolve them, once the output-linkage
input in section 3 is supplied.

## 6. D3D versus GL

The Linux GL driver `m9x_drv.so` reaches the same backend family
(`parhelia-m9-native-shader-fragments.md`, `-gl2-compiler-control-re.md`): the
Iris/GLSL frontend `0x1a40b0` and the D3D frontends `0x191130`/`0x1922a0` both
feed optimizer `0x1958a0` and backend `0x1d1d40`.  Differences that matter here:
GL seeds its stage-final output as class 12 (`gl_FragColor` at MxMGLX
`0x107275c5`) through the varying link table, which is exactly the linkage a bare
D3D token compile lacks; and GL texture modes are one-hot arguments 3..6 of the
shared lowerer `0x1bda50` (B/P/L/DD), whereas D3D derives them from the `TEX`
token bits.  The native slot encoding is common to both.

## 7. Hardware tests to run first (safest order)

All are lower risk than a blind fragment write (class-0 destinations already
locked up the engine, MEASURED).  Prefer, in order:

1. **Vendor capture, no risk:** dump the bound `c2` image and `8d..8e` constant
   spans under vendor GL for a constant-color, a `uniform*color`, and a
   `texture2D` fragment shader (`tools/m9-vendor-vram-dump.py`).  This yields the
   real compiled fragment slots and pins the output-selector map (`0x58/0x78`
   versus `0x2d`) and the actual terminator — closing section 3's linkage gap
   from observed data rather than by forcing the offline compiler.
2. **Safe-destination MOV sweep (MEASURED harness):** with the verified
   two-pass triangle, replace one output slot by a MOV whose destination is a
   candidate temporary selector near `0x2d` and read it back on scanout; find the
   fragment temporary range before any two-source op.
3. **Terminator check:** confirm whether a compiled fragment program stops on
   `ffff801c a01fc00f` (measured vendor form) or on the compiler endpoint
   `fffc7e1d a2803f0f`, by appending each after a known-good output slot.
4. Only then attempt driver-emitted `MUL`/`ADD`/`TEXLD` fragment bodies, built
   from the section 4 corpus with a proven safe temporary and the verified
   output/terminator.

## Reproduction

```sh
# host, from the repository root
python3 tools/m9-native-ps-compile.py          # print per-shader diagnostics
python3 tools/m9-native-ps-compile.py --check   # verify the pinned results
```

The helper is rebuilt each run from `tools/m9-native-ps-compile.c`; it requires
`gcc -m32` (i386 multilib) and the pinned `MxMD.dll`.
