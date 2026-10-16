# Public texture and depth FAA shader joins

Five genuine normal sampled-primary CPU frames now retain actual compiler and
program evidence. The caller supplies public DP2 state; the allocator, handle
publisher, state compilers and command producers run unchanged. No compiler
enum13, private compiled state, shader result or GPU reply is assigned.

| Caller case | Actual program bytes / slots | Preprocess and raster entry byte offsets | Final compiled04 | Merged F0 / EF / D7 |
| --- | --- | --- | --- | --- |
| Diffuse baseline | 240 / 30 | 32, 0 | `62af9` | `c4008600/f8004055/20005ca3` |
| Fixed-function texture | 328 / 41 | 48, 104, 144, 0 | `62af9` | same |
| Depth enabled | 248 / 31 | 32, 0 | `62ae1` | `c4008600/d8004055/20005ca3` |
| Public ps_2_0 texture | 312 / 39 | 32, 88, 128, 0 | `62af9` | same as baseline |
| Fixed-function texture with depth | 336 / 42 | 48, 104, 144, 0 | `62ae1` | `c4008600/d8004055/20005ca3` |

The texture resource is a genuine 64x64 profile12 color allocation. Actual
`7a770` publishes its handle into the context's existing resource table, and
public texture-stage packet25 binds that returned handle. The caller supplies
XYZRHW/diffuse/UV vertices with FVF144 and stride28. The programmable case
uses public CREATEPIXELSHADER54 and SETPIXELSHADER56, with standard ps_2_0
DCL t0, DCL 2D s0, TEXLD and MOV oC0 tokens. The declarations matter: the
initial declaration-free texld request returned CPU success but launched no
draw, so it is excluded from the proof.

The depth case copies the actual FAA companion's profile49 (class4, hardware
format3d, 64 bits) into a genuine sampled 320x240 allocation. Its returned
handle is bound by public SETRENDERTARGET41, with ZENABLE and ZWRITEENABLE1
and ZFUNC4. This finite depth choice does not establish arbitrary Gallium
depth formats or successful depth pixels.

The combined case uses both genuine returned handles in one original M9128
public DRAW52 request. Its packet order is RENDERSTATE8, VIEWPORT28,
SETRENDERTARGET41, TEXTURESTAGESTATE25, FVF47, STREAMSOURCE50, DRAW52.
The live texture handle is bound at texture stage0, and the live depth handle
is paired with the normal sampled primary. The three caller vertices are
XYZRHW/diffuse/UV with FVF144 and stride28. The original compiler selects
the same four program entry offsets as the texture-only case and the same
`62ae1` lowerer input and final EF as the depth-only case. Its 336 CPU-written
program bytes have SHA256
`0fd3998609b7f644e79cde278615332d9b2c7ac4f994b5b384227dd7fcb3973f`.
The first48 bytes match texture-only, but the combined program is eight bytes
longer and differs at later VS slots. The combined program is a distinct
compiler result.

Actual lowerer `181fa0` reads its nested compiler input through argument+20.
The depth case changes the genuine final compiled04 from62af9 to62ae1,
selecting AA mode6 instead of7. At `fae93..faf84`, the actual source fields
produce the final raster state:

* F0 = AA shadow+b4 (`8000`) | program shadow+e0 (`600`) |
  texture shadow+7c (`c4000000`).
* EF = AA shadow+b8 (`e0004055`, or depth `c0004055`) |
  program shadow+e4 (`18000000`) | texture shadow+80 (`0`).
* D7 = texture shadow+84 (`20005ca3`).

The retained program vectors are joined through actual kind13/14 operands,
the existing vendor CPU mapper, all matching CPU-published PTEs, byte offsets
and known CPU-written spans. The resident database's 115160 known bytes keep
SHA256 `af36245806515bd7b0e38a986586d33611a84ed76dae845a42ce8776ba89f174`
in all five cases. Every program has different bytes; the depth program
preserves the first32 FS bytes but changes VS bytes. Texture adds genuine
preprocessing heads and a gather before rasterization. A single captured
240-byte program or fixed two-launch plan is therefore insufficient for
these public shaders.

Run `tools/check-m9-faa-texture-depth.py` against
`docs/traces/m9-vendor-faa-texture-depth-20261006.json`. The compact trace
retains actual public packets, returned resource handles, compiler events,
source merge fields, program vectors/checksums and PTE joins. It omits the
large full-frame chronological CPU-write inventory, preserved in the raw
capture when generating the trace.

The combined case is reproducible with the original M9128 `MxMD.dll` in the
established emulator, using `tools/m9-vendor-faa-texture-depth.py --case
texture-depth`, `--runtime-tools tools/faaemu`, `--bootstrap-tools tools/m9emu`,
and the recorded `pci-inventory/inventory.json`, `pci-resources.txt`, and
`dc-banks-idle-e4611b75.csv` inputs under
`captures/m9128-device-inputs-20261006/`. The compact trace records those
input hashes and the vendor image hash. Captured handles and GPU addresses
identify the original owners for source checking; they are not native values.

## Controlled public-state differentials

The original M9128 emulator repeated the combined request with controlled
public state values. All seven runs use the same genuine normal
sampled primary, allocated 64x64 profile12 texture, 320x240 profile49 depth
target, three XYZ/RHW/UV vertices, source input hashes, and four launcher
routes. Each CPU draw returned successfully; no GPU execution or completion
was supplied.

| Public state change | Compiler `+04` | Program bytes | Program result | Final EF |
| --- | --- | --- | --- | --- |
| COLOROP4 (MODULATE), ZENABLE1, ZFUNC4, ZWRITE1 | `62ae1` | 336 | baseline | `d8004055` |
| COLOROP7 (ADD) only | `62ae1` | 336 | FS slots2 and4 change, 11 bytes total | `d8004055` |
| COLOROP5 (MODULATE2X) only | `62ae1` | 352 | FS grows by two slots; preprocess entries shift | `d8004055` |
| COLOROP7, COLORARG1/2 swapped | `62ae1` | 336 | ADD operands swap in slots2 and4 | `d8004055` |
| ZFUNC8 (ALWAYS) only | `62ae1` | 336 | byte-identical | `d8004055` |
| ZWRITE0 only | `62ae1` | 336 | byte-identical | `d8004055` |
| ZENABLE0, depth still bound | `62af9` | 328 | byte-identical to texture-only | `f8004055` |

All except MODULATE2X retain entry offsets `48,104,144,0`; that case uses
`64,120,160,0`. COLOROP4→7 changes only
the public stage0 state1 payload and the CPU-written program, with SHA256
`848a74cc622014616e36671b08874cc9abcf9a83656a80cc208a7207a7cf0b8d`
for the ADD program. It changes no triangle command-list write. ZFUNC4→8
and ZWRITE1→0 each change only the public RENDERSTATE8 payload and one
source command-list write at original PC `fbe8c`, respectively
`38038f→38039f` and `38038f→38038d`. ZENABLE1→0 changes seven source
command-list writes and selects the texture-only program and compiled word
even though the depth resource remains bound. These are CPU source outputs,
not decoded depth pixels or a general depth control formula.

The repository's M9 two-dword disassembler and the existing Mesa native FS
encoder identify the two changed instructions as register-register multiply
(`op2=01`) versus register-register add (`op2=04`). In this fixed program,
`op0=1d` and class4 write the first scalar of each destination pair. The
actual encoded operand selectors are:

| Stage | FS slot | Operation | Destination `d` | First source `a` | Second source |
| --- | ---: | --- | ---: | ---: | --- |
| MODULATE | 2 | MUL, `op2=01` | `08` | `02` | `b=06` |
| MODULATE | 4 | MUL, `op2=01` | `2d` | `00` | `b=1a` |
| ADD | 2 | ADD, `op2=04` | `08` | `02` | `c=06` |
| ADD | 4 | ADD, `op2=04` | `2d` | `00` | `c=1a` |
| ADD, arguments swapped | 2 | ADD, `op2=04` | `08` | `06` | `c=02` |
| ADD, arguments swapped | 4 | ADD, `op2=04` | `2d` | `1a` | `c=00` |

The swapped public COLORARG1/2 request changes exactly these source fields:
`02/06` and `00/1a` trade places, while the two destination selectors and
four entry routes stay fixed. It changes no triangle command-list write.
MODULATE2X confirms that a nearby valid COLOROP changes more than an opcode:
its FS prefix gains two slots, earlier interpolation destinations and later
temporary selectors change, and all three preprocessing entries advance16
bytes. This evidence supports the decoded MUL/ADD operation and operand
routing for these captured states; it is not an encoder for a live shader.

### Two public texture-coordinate selections

The original M9128 path also accepts a dual-coordinate FVF `0x244`, stride36
triangle. Each vertex supplies the original UV pair and a distinct second UV
pair. The only change within each pair of COLOROP4, 5 or 7 captures is public
stage0 state11 `TEXCOORDINDEX=0` versus `1`. Source `0x105648` stores this
field and compiler `0x17f91d` consumes it as the coordinate record index;
this is a live public state, not a caller-assigned private compiler mode.

All six CPU draws return successfully. For each COLOROP, both coordinate
selections produce byte-identical complete programs, compiled04 `62ae1`,
and the same merged F0/EF/D7. The programs also match the corresponding
single-coordinate captures above: 336 bytes and entries `48,104,144,0` for
MODULATE and ADD, 352 bytes and `64,120,160,0` for MODULATE2X. Choosing
coordinate set1 changes exactly three original source command-list writes:
`f70fa:05300d88→07300d88`, `f7102:05300b36→07300b36`, and
`fbee8:28207006→28207046` for MODULATE/ADD, or
`28207776→282077b6` for MODULATE2X. The coordinate selection is therefore
represented outside the C2 program in these admitted cases. The captures
do not supply texture pixels or prove that the GPU samples the selected UVs.

The existing Mesa FS encoder explains the captured operation/control words
for the two fixed slots. Its `L_MUL_RR` base W1 is `ac200000`, its `L_ADD_RR`
base is `ac800c00`, and `w0_make` packs the decoded register selectors.
The additional `40000000` in slot2 is the known program end marker: the
encoder places it at FS slot `max(n,4)-4`, which is slot2 for the six-slot
MODULATE/ADD prefix and slot4 for the eight-slot MODULATE2X prefix. The
remaining W1 swizzle bits are `140` for the second MODULATE slot and `110`
for the second ADD slot, matching the current backend's scalar selectors.
This closes the *encoding of these observed slots once their scheduled
operands and program length are known*. It does not derive the vendor's
register allocation or program-length decision from an arbitrary live
Gallium shader. In particular, TEXCOORDINDEX changes source records while
the program remains identical, and MODULATE2X changes slot scheduling and
preprocess entry offsets. A fixed C2 byte table would also copy captured
layout instead of deriving it from retained owners.

Reproduce this matrix with `tools/m9-vendor-faa-texture-depth.py --case
texture-depth --texcoord-count 2`, COLOROP `4`, `5`, or `7`, and
`--texcoord-index 0` or `1`, using the same original M9128 runtime and
recorded device/DC inputs above. `tools/m9-vendor-faa-texcoord-compact.py`
produces `docs/traces/m9-vendor-faa-texcoord-diff-20261007.json`; run
`tools/check-m9-faa-texcoord-diff.py` with that trace and
`docs/traces/m9-vendor-faa-variant-diff-20261007.json`. The checker proves
source-owned program/PTE joins, exact public state differences, C2 equality,
entry offsets, the three command-word differences, decoded operand fields,
and marker placement. This is a bounded negative for a new FAA encoder, not
a GPU rendering result.

Reproduce the seven raw captures with `tools/m9-vendor-faa-texture-depth.py
--case texture-depth` and, for the six controls, `--color-op 7`,
`--color-op 5`, `--color-op 7 --color-arg1 0 --color-arg2 2`,
`--zfunc 8`, `--zwrite 0`, or `--zenable 0`; use the same runtime, bootstrap, PCI and DC
inputs above. `tools/m9-vendor-faa-variant-diff-compact.py` extracts the
source-bearing differential into
`docs/traces/m9-vendor-faa-variant-diff-20261007.json`. Run
`tools/check-m9-faa-variant-diff.py` against that trace and the five-case
trace to check packet deltas, returned handles, compiler words, source-owned
program bytes/PTE joins, entry routes and command-write deltas.

This still does not justify lowering Mesa's live fragment shader into a
vendor program. `m9_faa_lower_compiled04` translates a genuine vendor compiler
word into AE/EF/F0 edits; Mesa has no source-proven map from its Gallium/NIR
shader and texture state to the vendor word or to a complete instruction
stream. The decoded slots still carry context-dependent register allocation,
swizzles and control bits that the public state differential does not derive;
MODULATE2X also changes program length and entry offsets. The depth controls
have a separate command-word producer. The next source experiment must pin
the original M9128 compiler's register assignment, swizzle/control fields and
program-length decisions with independently varied arguments and adjacent
operations, then join that encoding to Mesa's live FS operations. Before any application FAA
submit, a native producer also needs two distinct retained placements and
relevant alignment changes for the entire program and command flow.

Texture contents and depth storage are not populated by a simulated GPU.
This proves CPU admission and program/state production, not rendering or
retirement. The vertices are caller XYZRHW; arbitrary application GPU vertex
transforms, native metadata pairing and texture/depth variants still require
joins before activating prepared Mesa FAA jobs.
