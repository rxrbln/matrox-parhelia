# Matrox M9 native shader fragments

This note records the native microcode database recovered statically from the
saved Windows `MxMD.dll` and cross-checked against Linux `m9x_drv.so`.  It is a
companion to `parhelia-m9-3d-static-re.md`.  Addresses and lengths below are
binary evidence.  The backend encoder fixes several operation and operand-field
boundaries.  Compiler lowering also fixes operand direction, result-component
order, and several source-operation anchors, although most native operation
mnemonics remain unknown.

Caller analysis identifies this table as the fixed-function vertex-program and
vertex-declaration conversion generator.  It is not a pixel-shader library.

## Proven representation

* A native instruction slot is 8 bytes/two little-endian dwords.
* One selector-`68`, ports-`60..67` upload line is 32 bytes/eight dwords and
  carries four native slots.
* Linker `0x13bf80` pads the final stream to 32 bytes, so its last one to three
  slots can be padding.
* The `0xc00` bank boundary is 3,072 bytes: 384 native slots or 96 upload lines.
* All fixed-fragment lengths are divisible by eight.  The standalone fragment
  at VA `0x1ecdd8` is exactly one instruction:

```text
{ 0xfffc0fff, 0x8bffffc0 }
```

Windows function `0x1454a0` builds 57 vector slots, numbered `0x00..0x38`, at
stride `0x10`.  Helper `0xadf00` appends `{read_only_data_pointer, byte_length}`
descriptors.  Function `0x120910` flattens the entire `+0x000..+0x380` table.
Linux 1.15 exports the equivalent initializer as `m9x30489` at `0x4462d0`.

The Windows fixed data covers 48 unique contiguous blobs at
`0x1e91f8..0x1ef780`, totaling `0x6588` bytes or 3,249 instruction slots.  The
SHA-256 of that contiguous block is
`0191f28356ce9d99de6c859ecb54e54ac66e9b8dd2a5823aff75f757b4095883`.

## Slot manifest

Entries are `Windows_VA/byte_length`, with hexadecimal lengths.  `A` and `B`
mark mutually exclusive prefixes; `optional` preserves branches whose condition
has not been named safely.

```text
00: 1e91f8/a8 1e9358/240 1eabf0/128 1eca48/b8
01: 1ecf20/58 1ecd58/80 1ecdd8/8 1e91f8/a8 1e9358/240 1eabf0/128 1eca48/b8
02: 1ece08/d8 1ecbb8/100 1e91f8/a8 1e9358/240 1eabf0/128 1eccb8/a0 1eca48/b8
03: 1ece08/d8 1ecbb8/100 1ecf20/58 1ecd58/80 1ecdd8/8 1e91f8/a8 1e9358/240 1eabf0/128 1eccb8/a0 1eca48/b8
04: 1e91f8/a8 1e9598/440 1eabf0/128 1eca48/b8
05: 1e91f8/a8 1ea3b0/568 1eabf0/128 1eca48/b8
06: 1ecf20/58 1ecd58/80 1ecdd8/8 1e91f8/a8 1ea3b0/568 1eabf0/128 1eca48/b8
07: 1ece08/d8 1ecbb8/100 1e91f8/a8 1ea3b0/568 1eabf0/128 1eccb8/a0 1eca48/b8
08: 1ece08/d8 1ecbb8/100 1ecf20/58 1ecd58/80 1ecdd8/8 1e91f8/a8 1ea3b0/568 1eabf0/128 1eccb8/a0 1eca48/b8
09: 1e91f8/a8 1e9358/240 1ead18/208 1ecb00/b8
0a: 1ece08/d8 1ecbb8/100 1e91f8/a8 1e9358/240 1ead18/208 1eccb8/a0 1ecb00/b8
0b: A=(1e92a0/b8 1e99d8/458) B=(1e91f8/a8 1e9598/440), then 1ead18/208 1ecb00/b8
0c: 1e91f8/a8 1e92a0/b8 1e9e30/580 1ead18/208 1ecb00/b8
0d: 1ece08/d8 1ecbb8/100 1e91f8/a8 1e92a0/b8 1e9e30/580 1ead18/208 1eccb8/a0 1ecb00/b8
0e: 1e91f8/a8 1e9358/240 1eaf20/310 1ecb00/b8
0f: 1ecf20/58 1e91f8/a8 1e9358/240 1eaf20/310 1ecb00/b8
10: 1ece08/d8 1ecbb8/100 1e91f8/a8 1e9358/240 1eaf20/310 1eccb8/a0 1ecb00/b8
11: 1ece08/d8 1ecbb8/100 1ecf20/58 1e91f8/a8 1e9358/240 1eaf20/310 1eccb8/a0 1ecb00/b8
12: A=(1e92a0/b8 1e99d8/458) B=(1e91f8/a8 1e9598/440), then 1eaf20/310 1ecb00/b8
13: 1e91f8/a8 1e92a0/b8 1e9e30/580 1eaf20/310 1ecb00/b8
14: 1ecf20/58 1e91f8/a8 1e92a0/b8 1e9e30/580 1eaf20/310 1ecb00/b8
15: 1ece08/d8 1ecbb8/100 1e91f8/a8 1e92a0/b8 1e9e30/580 1eaf20/310 1eccb8/a0 1ecb00/b8
16: 1ece08/d8 1ecbb8/100 1ecf20/58 1e91f8/a8 1e92a0/b8 1e9e30/580 1eaf20/310 1eccb8/a0 1ecb00/b8
17: 1e91f8/a8 1e9358/240 1eb230/340 1ecb00/b8
18: 1ecf20/58 1e91f8/a8 1e9358/240 1eb230/340 1ecb00/b8
19: A=(1e92a0/b8 1e99d8/458) B=(1e91f8/a8 1e9598/440), then 1eb230/340 1ecb00/b8
1a: 1e91f8/a8 1e92a0/b8 1e9e30/580 1eb230/340 1ecb00/b8
1b: 1ecf20/58 1e91f8/a8 1e92a0/b8 1e9e30/580 1eb230/340 1ecb00/b8
1c: 1eb570/78
1d: if owner state +0xc == 0: 1ed400/9c0 1e91f8/a8 1ea918/2d8 1ebb08/118 1eca48/b8; otherwise 1eb570/78 plus dynamic descriptor
1e: 1e91f8/a8 1e9358/240 1eb5e8/270 1eca48/b8
1f: optional 1ed3e0/20 1ed400/9c0; branch body uses 1e91f8/a8 1e9598/440 and 1ebc20/248 or 1eb5e8/270; suffix 1eca48/b8
20: 1e91f8/a8 1ea3b0/568 1eb5e8/270 1eca48/b8
21: 1e91f8/a8 1e9358/240 1eb858/2b0 1eca48/b8
22: optional 1ed3e0/20 1ed400/9c0; branch body uses 1e91f8/a8 1e9598/440 and 1ebe68/290 or 1eb858/2b0; suffix 1eca48/b8
23: 1e91f8/a8 1ea3b0/568 1eb858/2b0 1eca48/b8
24: 1ece08/d8 1ecee0/20
25: 1ecf78/58 1ece08/d8 1ecee0/20
26: 1ecd58/80 1ecde0/28
27: 1ecf78/58 1ecd58/80 1ecde0/28
28: 1ecf00/20
29: 1ec0f8/78 1ec170/1b8 1ec990/b8 1eca48/b8
2a: 1ecfd0/60 1ec0f8/78 1ec170/1b8 1ec990/b8 1eca48/b8
2b: 1ec0f8/78 1ec170/1b8 1ec328/280 1ec990/b8 1eca48/b8
2c: 1ec0f8/78 1ec170/1b8 1ec5a8/3e8 1ec990/b8 1eca48/b8
2d: 1ecfd0/60 1ec0f8/78 1ec170/1b8 1ec5a8/3e8 1ec990/b8 1eca48/b8
2e: 1ed030/3b0 1e91f8/a8 1ea918/2d8 1ebb08/118 1eca48/b8
2f: 1eddc0/630
30: 1eed60/40 1eddc0/630
31: 1e92a0/b8 1e9358/240 1ee3f0/8e0 1ecb00/b8
32: 1e92a0/b8; conditional 1e99d8/458; then 1ee3f0/8e0 1ecb00/b8
33: 1e92a0/b8 1e9e30/580 1ee3f0/8e0 1ecb00/b8
34: 1e92a0/b8 [optional 1eecd0/58] 1e9e30/580 [optional 1eed28/38] 1ee3f0/8e0 1ecb00/b8
35: 1eeda0/38
36: 1eedd8/968
37: 1ef740/20
38: 1ef760/20
```

For slot `0x0b`, branch A is selected exactly by:

```c
owner = *(obj + 0x414);
state = *(owner + 8);
use_a = (*(state + 0x0c) != 0) && (*(state + 0x40) == 0);
```

This branch is at Windows `0x145db1..0x145e8b`.  Slots `0x12` and `0x19`
strongly share that A/B prefix construction.  Conditions for `0x1f`, `0x22`,
`0x32`, and `0x34` are only partially decoded.

## Fixed-function vertex selector

The selector and two-fragment linker are independently present in both vendor
drivers [BIN]:

| role | Windows `MxMD.dll` | Linux `m9x_drv.so` |
|---|---:|---:|
| main selector | `0x13b2b0`, wrapper `0x13b260` | `m9x1091` `0x44a9f0`, wrapper `m9x11255` `0x44add0` |
| companion selector | `0x13b570` | `m9x27688` `0x44ae10` |
| two-fragment linker | `0x13bf80` | corresponding linked-program path |

The Windows pipeline calls main selector at `0x13c3a7`, companion selector at
`0x13c3ec`, and linker at `0x13c503`.  The Linux order is `m9x11255` at
`0x44c5ae`, then `m9x27688` at `0x44c5e3`.

The encoded declaration uses `t = D & 0x0f` and `m = D & 0x30`.  Its low-nibble
families match `D3DDECLTYPE` exactly [DER]:

| declaration types | enum values | selector family |
|---|---|---|
| `UBYTE4,SHORT2,SHORT4,UBYTE4N,SHORT2N,SHORT4N,USHORT4N,UDEC3` | `5,6,7,8,9,10,12,13` | packed-input conversion |
| `FLOAT3,FLOAT4,D3DCOLOR,USHORT2N,DEC3N` | `2,3,4,11,14` | float/color/special conversion |
| `FLOAT1,FLOAT2,FLOAT16_2`, default | `0,1,15` | generic conversion |

`FLOAT16_4` needs higher format bits because its enum value 16 aliases low
nibble zero.  Helper `0x13c8d0` recognizes packed types
`5,6,7,9,10,12,13`, while type 8 is forced into the same family.

The main selector records `{P,S,M}` and the companion optionally replaces `S`
with `Q`.  Linker `0x13bf80` concatenates `P + (Q when active, otherwise S)`.
Roles overlap between slots:

| role | slot IDs, hexadecimal |
|---|---|
| `P`, main conversion/fixed-function body | `00,01,04-06,09-0f,12-14,17-24,29-2f,31-35` |
| `Q`, conditional companion/continuation | `02,03,07,08,0a,0b,0d,0e,10,11,15,16,2a,2d,2f,30,35,36` |
| `S`, default/alternate continuation | `24-29` |
| `M`, class marker | `37,38` |

The main selector writes `cumulative_offset[P] >> 3` and
`cumulative_offset[S] >> 3` to Windows pass fields `+0xa8/+0xaa` or Linux
`+0xb0/+0xb2`.  The companion writes `cumulative_offset[Q] >> 3` at Windows
`+0xbc` or Linux `+0xd0`.  The trace tuple has
Windows stride `0x10` and Linux stride `0x18`; the cumulative-offset table is
at Windows library `+0x418` or Linux `+0x7b0`.  Pass strides are `0xc04` and
`0xc34`, respectively.

Let `Fn` denote bit `n` of selector flags `F`.  The exact base `P` selection is:

| predicate | base `P` |
|---|---:|
| `F7 && !F4` | `31` |
| `F7 && F4 && !F8` | `32` |
| `F7 && F4 && F8 && (m==0x10 || (D&0x6000)==0x2000)` | `34` |
| remaining `F7` | `33` |
| packed, `!F3`, `!F4 && !F9` | `00` |
| packed, `!F3`, later zero-`F8/F9` case | `04` |
| remaining packed `!F3` | `05` |
| packed, `F3,!F6,m==0x20,!F5,!F4,!F9` | `09` |
| same feature branch, later zero-`F8/F9` case | `0b` |
| remaining same branch | `0c` |
| packed, `F3,!F6,m==0x20,F5,!F4,!F9` | `0e` |
| same feature branch, later zero-`F8/F9` case | `12` |
| remaining same branch | `13` |
| packed, `F3,!F6,m!=0x20,!F5` | `1c + F4` |
| packed, `F3,!F6,m!=0x20,F5,!F4` | `1e` |
| packed, `F3,!F6,m!=0x20,F5,F4` | `1f + F8` |
| packed, `F3,F6,m==0x20,!F4,!F9` | `17` |
| same feature branch, later zero-`F8/F9` case | `19` |
| remaining same branch | `1a` |
| packed, `F3,F6,m!=0x20,!F4` | `21` |
| packed, `F3,F6,m!=0x20,F4` | `22 + F8` |
| float/color, `!F4 && !F9` | `29` |
| float/color, later zero-`F8/F9` case | `2b` |
| remaining float/color | `2c` |
| generic declaration fallback | `2e` |

Flag `F10` normally increments both `P` and `S`.  The base continuation is
`S=24` for `F6`, `S=28` for `!F6 && mode==1 && !global_inhibit`, and `S=26`
otherwise.  Windows uniquely rejects special slot `28` with `F10`, falls back
to `26`, then increments to `27`; Linux preserves the special choice and uses
`29`.  This is a real cross-version behavior difference [BIN].

The companion override is:

| predicate | `Q` |
|---|---:|
| `F7`, special format condition | `30` |
| other `F7` | `2f` |
| `!F7,F11,!F3,!F4,!F9` | `02 + F10` |
| `!F7,F11,!F3`, remaining | `07 + F10` |
| `!F7,F11,F3,!F5,!F4,!F9` | `0a + F10` |
| same branch, remaining | `0d + F10` |
| `!F7,F11,F3,F5,!F4,!F9` | `10 + F10` |
| same branch, remaining | `15 + F10` |
| `!F7,!F11,F10,t in {FLOAT3,FLOAT4},!F9` | `2a` |
| same with `F9` | `2d` |
| pass zero, derived fallback A/B | `35` / `36` |
| otherwise | retain `S` |

For the `FLOAT3/FLOAT4` choice between `2a` and `2d`, Windows tests cached `F9`
at `0x13b6b9..0x13b6c9`, while Linux tests a nested global-state bit at
`0x44b030..0x44b042`.  They may be synchronized earlier, but the physical
sources differ and an open implementation should derive this choice from its
own explicit state rather than imitate either object layout.

Marker `M` is `38` for `F7` or for packed input with
`F3 && (F5 || F6)`; otherwise it is `37`.  No direct lookup of `M` through the
cumulative-offset table was found, so its hardware use remains unknown.

Proven flag construction includes `F2=packed_numeric`,
`F7=F2&&(D&0x1000)`, and `F3=F2&&((D&0x30)!=0x30)`.  `F4` reflects nonzero
context-derived state mask `&0x7e`; `F8` is helper `0x12bed0(0,1)`; `F9`
mirrors context `+0x2504` bit 4 and accompanies installation of a 4x4 identity
matrix; `F10` mirrors the next context bit; and `F11` accompanies allocation of
floating-point constants.  Their final D3D semantic names remain unresolved.

A prior sign-test interpretation as `F bit31` is wrong.  Windows and Linux both
test the sign of the low byte, proving `F bit7`.

### Auxiliary records belong to this vertex path

The previously neutral `upload-1300` through `upload-1500` names can now be
assigned to this fixed-function vertex/declaration-conversion path.  Wrapper
`0x13c2c0`, whose only external use is the pipeline around the vertex
declaration and device `+0xac0`, calls the following four state builders before
linker/padder `0x13bf80` [BIN]:

```text
call 0x13c409 -> 0x13b7e0
call 0x13c436 -> 0x13b9b0
call 0x13c464 -> 0x13bb80
call 0x13c4a8 -> 0x13bda0
```

Builder `0x13b7e0` copies eight floats from device `+0xac0..+0xadc` to the
current Windows pass at `+0x7c0..+0x7dc` and derives two adjacent controls.
Emitter `0xf78c0` then forms selector `0x80001300` as follows; this is the exact
Windows map and explains why it mixes copied values and controls [BIN][DER]:

| port | Windows pass source |
|---:|---:|
| `60,61,62` | `+0x7c0,+0x7c4,+0x7c8` |
| `63,64,65,66,67` | `+0x844,+0x7d0,+0x7d4,+0x7d8,+0x840` |

Builder `0x13b9b0` copies exactly 24 dwords (`rep movsd`, `ecx=0x18`) from
device `+0xb44` to pass `+0x7e0`.  The next three selectors are consecutive
pieces of that one table:

| selector | Windows pass source |
|---:|---:|
| `0x80001320` | `+0x7e0..+0x7fc` |
| `0x80001340` | `+0x800..+0x81c` |
| `0x80001360` | `+0x820..+0x83c` |

Builder `0x13bda0` fills resource/control state at `+0x848..+0x86c`.
Emitter `0xf7c60` consumes it for selector `0x80001500`: ports `62/63` are two
relocations of resource `+0x868/+0x86c` with usage pairs `0x15/0x16`, ports
`66/67` are two relocations of resource `+0x850/+0x854` with usage pairs
`0x13/0x14`, and the other ports carry dimensions and controls.  Selector
`1500` is therefore a vertex-conversion resource/control record.  It is not a
shader constant record.  The companion static-RE note retains the Linux field
offsets, whose object layout differs while preserving all five selectors.

## Native instruction encoder

The D3D compiler supplies the missing join between its optimizer and native
code.  Optimizer `0x1958a0` calls backend `0x1d1d40` at `0x195b88`; the latter
eventually presents 0x44-byte instruction objects to a native emitter.  The
instruction constructor is `0x1da2c0`, the whole-program emitter is
`0x1db510`, and the exact one-slot packer is `0x1db840..0x1db9bb` [BIN].  The
packer is the third method in the vtable at `0x1f50bc` and appends one pair of
dwords to an output array limited to `0x100` slots.

The relevant intermediate instruction members are [BIN]:

| member | initial value | proven use |
|---:|---:|---|
| `+0x08` | `0` | base for native `W1`; operation and modifier bits |
| `+0x0c` | `0`, 16-bit | base for native `W0` low half |
| `+0x0e` | `0`, 16-bit | presence/format gates used by the packer |
| `+0x10,+0x14,+0x18,+0x20,+0x24` | `0xf0000000` | explicit source/resource descriptors |
| `+0x1c` | `0xf0000000` | explicit result/destination descriptor |
| `+0x28,+0x2a` | `0`, 16-bit | alternate selectors for two physical fields |
| `+0x2c,+0x2e,+0x30,+0x32,+0x34` | low nibble cleared | per-descriptor component metadata used by analysis/rewrites |
| `+0x38,+0x3c` | `0xf0000000` | inferred/implicit descriptor overrides |

`0xf0000000` is the absent-descriptor sentinel.  Routine `0x1da4a0` walks the
five pairs `(+0x14,+0x2c)`, `(+0x18,+0x2e)`, `(+0x20,+0x30)`,
`(+0x10,+0x32)`, and `(+0x24,+0x34)` while constructing the compiler's
resource/dependency list.  It handles `+0x1c` separately at
`0x1da935..0x1da94f`, storing it in the result-side local rather than adding it
to that source collection.  Routine `0x1daf60` preserves the same separation
while renumbering descriptors.  The arithmetic lowerers provide an independent
data-flow check: the first RCP instruction receives the caller's source in
`+0x14` or the format-dependent alternate `+0x10` and a newly allocated
temporary in `+0x1c`; the final instruction reads intermediate temporaries
through `+0x14/+0x18/+0x20` and places the caller's result descriptor in
`+0x1c`.  This proves the direction shown in the table.  Exact register-class
names are joined below.

Converter `0x1a3e00` supplies a direct bridge from the compiler token described
in the companion note to this backend's 0x88-byte instruction IR [BIN][DER]:

| backend IR | compiler token source |
|---:|---:|
| `+0x00,+0x04` | token `+0x08` bits 0 and 1, conditional and predicate-enable |
| `+0x0c` | token `+0x04` low 16-bit D3D operation |
| `+0x14` | destination descriptor at token `+0x44` |
| `+0x1c,+0x20,+0x24,+0x28` | four descriptors at token `+0x7c+0x38*n` |
| `+0x30` | predicate descriptor at token `+0x0c` |
| `+0x60,+0x64` | source-zero auxiliary dwords at token `+0xa4,+0xa8` |
| `+0x68..+0x7c` | corresponding auxiliary pairs for sources one through three |

This also corrects an earlier object-lifetime mistake: IR `+0x60/+0x64` are
per-source fields carried by each instruction, not members of a program-wide
context.

### Compiler register files and native descriptor classes

The register-file formatter at `MxMGLX.dll` `0x106ada60` names the same
low-five-bit descriptor codes switched on by MxMD converters `0x1a34e0`,
`0x1a3760`, and `0x1a3ac0`.  This joins the accepted compiler files to native
descriptor classes [BIN][DER].  For a direct logical index `i`, the proven
source transforms are:

| code | compiler name | native descriptor transform |
|---:|---|---|
| `0` | `r` temporary | `2*i` |
| `1` | `v` input | `0x10000000 + 2*i` |
| `2` | `c` constant | `0x20000000 + 2*i` |
| `3` | `a` address | `2*i`, with relative form checked separately |
| `6` | `o` output | output-link remap before final encoding |
| `7` | `ci` | `0x20000000 + 2*i` |
| `10` | `s` sampler | `0x70000000 + i` |
| `14` | `cb` constant buffer | `0x20000000 + 2*i` |
| `17` | `vPos`/`vFace`/`gp` system input | linker remap plus `0x40000000` |
| `18` | `l` literal | `i - 0x20000000` |
| `30,31` | `rb`,`ri` internal | `2*i` |

All other source file codes are rejected by the 32-byte dispatch table at
`0x1a3a98`.  Destinations accept only `r`, `a`, `o`, `p`, `rb`, and `ri`.
The direct transforms for `r/a/rb/ri` are `2*i`, while predicate file `p`
becomes `0x60000000 + 2*i`; `o` goes through the output-link table and its
special sentinel case.  These are compiler-facing descriptor classes.  The
seven-bit physical selector is selected later by the instruction format, so a
class prefix must not be truncated before packing.

The varying remap also reaches this native descriptor conversion.  The GLSL
linker builds fourteen dwords at `MxMGLX.dll +0x5bbc` and passes that table plus
its high-water mark through `0x1055deb0` while installing the paired stage
program.  In MxMD, an `o` descriptor enters the context's `+0x34..+0x38`
16-byte-stride link table: converters `0x1a34e0` and `0x1a3760` compare the
logical index with entry `+0x04`, replace it with entry `+0x00`, and use the
file/class fields in entry `+0x0c` before producing the native descriptor
[BIN][DER].  The corresponding `v` input path uses the same record shape in
context `+0x24..+0x28`.  Physical varying assignment therefore happens in an
explicit linker table before the final seven-bit selector is packed; logical
GLSL varying order is not a hardware register number.

The final selector arithmetic is now joined.  Let `p` be entry `+0x00` from
the matched physical-link record.  A fragment `v` descriptor is
`0x10000000 + 2*p`, so its low seven-bit selector is `2*p`.  A vertex `o`
descriptor is `0x0000ffd8 + 2*p`, giving selector `0x58 + 2*p`; the special
output-class case uses descriptor `0x0000fff8`, selector `0x78` [BIN][DER].
The linker's fourteen-slot bound keeps the ordinary formulas below `0x80`.
The class prefixes remain relevant to earlier compiler decisions, but the
gated packer consumes exactly these low seven bits.

Let `replace(x, hi, lo, v)` replace inclusive bits `hi:lo` in `x` with the low
`hi-lo+1` bits of `v`.  Ignoring a separate program-property side effect at
`0x1db941..0x1db988`, the packer is exactly [DER]:

```c
W0 = 0xffff8000 | insn_u16[0x0c];
W1 = insn_u32[0x08] | 0x0000000f; /* bits 20:19 are preserved */
g = insn_u16[0x0e];

if (g & 0x20) {
        q = insn_u16[0x14] & 0x7f;
        W0 = replace(W0, 31, 29, q);
        W1 = replace(W1,  3,  0, q >> 3);
}
if (g & 0x40) W0 = replace(W0, 28, 22, insn_u32[0x18]);
if (g & 0x80) W0 = replace(W0, 21, 15, insn_u32[0x20]);
if (g & 0x01) W1 = replace(W1, 20, 14, bank7(insn_u32[0x10]));
if (g & 0x02) W0 = replace(W0, 21, 15, bank7(insn_u32[0x24]));
if (g & 0x04) W0 = replace(W0, 14,  8, insn_u32[0x1c]);
if (g & 0x08) W1 = replace(W1, 20, 14, insn_u16[0x28]);
if (g & 0x10) W0 = replace(W0, 21, 15, insn_u16[0x2a]);
```

Here `bank7(d)` returns `(d + 0x40) & 0x7f` when `d & 0xf0000000` is
`0xb0000000`, and `d & 0x7f` otherwise.  This is direct evidence that at least
two internal descriptor classes share the seven-bit native selector namespace.
The overlapping destinations in the table are real: format gates choose
whether a physical field holds an operation or one of several descriptor
forms.  They must not be implemented as simultaneous independent operands.

### Operation and component fields

Three fields now have compiler-side operation evidence [BIN][DER]:

| native field | encoder/decoder evidence | conclusion |
|---:|---|---|
| `W0[4:0]` | setter `0x1af390`; implicit-resource decoder `0x1db9d0`; explicit tests for `0x1c..0x1e` at `0x1da987` | operation selector with a proven result-component family |
| `W1[25:21]` | setter `0x1dbe50`; expanded-operation decoder `0x1dc220` | second operation selector |
| `W1[18:14]` | setter `0x1dc040`; expanded-operation decoder `0x1dc2d0` | format-dependent third operation selector, sharing `W1[20:14]` with a seven-bit descriptor |

The two `W1` operation setters accept an expanded compiler operation number,
store a five-bit base selector, and place extension choices in separate
modifier bits.  Thus values returned by `0x1dc220` or `0x1dc2d0` can exceed
31 even though the base operation field is five bits.  Native `W0[14:8]` is
also decoded through a sparse implicit-resource table at `0x1dbb40`, but the
packer can replace all seven bits with descriptor `+0x1c`; it remains a
format-dependent selector rather than a safely named fourth operation.

The compiler source operation in its 0x88-byte input record is distinct from
all three native operation fields.  Dispatch `0x1ca2d0` recognizes source
operation values `6`, `7`, `14`, and `15`; these are the D3D `RCP`, `RSQ`,
`EXP`, and `LOG` values and lead to four characteristic multi-instruction
lowerers [BIN][DER]:

| source operation | lowerer | native instruction objects | directly observed operation-setter inputs |
|---|---:|---:|---|
| `RCP` (`6`) | `0x1b0840` | 6 | second `0x11,0x13`; third `0x1a` |
| `RSQ` (`7`) | `0x1b1210` | 7 | second `0x14,0x111,0x113`; third `0x1a` |
| `EXP` (`14`) | `0x1b1ba0` | 6 | second `0x14,0x01`; third `0x14,0x02` |
| `LOG` (`15`) | `0x1b2550` | 9 | second `0x14,0x02`; third `0x14,0x01` |

These are semantic anchors for lowering sequences, not one-to-one native
mnemonics.  In particular, RSQ uses expanded setter values `0x111` and
`0x113`: they retain five-bit bases `0x11` and `0x13` while selecting extension
bits.  Destination masks and descriptor classes also select or omit stages, so
the constant call sites above are deliberately not presented as complete
sequences.

The generic builder at `0x1b5390` provides five more exact D3D-operation
anchors.  Its 46-byte table at `0x1b5b30` is indexed by `opcode-1`; every entry
uses the default handler except `MOV=1`, `FRC=19`, `SGN=34`, `ABS=35`, and
`MOVA=46` [BIN][DER].  `MOV` selects expanded second operation `0x14` or
`0x15` and a mode dependent on result class, `FRC` selects second operation
`0x14` with mode 2, `SGN` selects `0x14/0x15` with mode 5, `ABS` selects
`0x14` with mode 4, and `MOVA` selects `0x14` with mode zero.  D3D `DSX=91`
and `DSY=92` bypass that table and select expanded third-operation markers
`0x88` and `0x89`, respectively.  These joins identify lowering controls and
differentiate arithmetic operations, but they also demonstrate why assigning
`0x14` itself the mnemonic `MOV`, `FRC`, `ABS`, or `MOVA` would be wrong.

Helper `0x1afd30` establishes the result-component order for the simple
descriptor-class path.  It selects a two-bit pair from the source-level D3D
destination mask in bits `19:16`, using pair zero for `xy` and pair one for
`zw`, and returns a `W0[4:0]` value as follows [DER]:

| active components in selected pair | `W0[4:0]` |
|---|---:|
| neither | `0x00` |
| first (`x` or `z`) | `0x1d` |
| second (`y` or `w`) | `0x1e` |
| both | `0x1c` |

The generic compiler path independently maps descriptor component-class bits
`27:26` to `0x1d`, `0x1e`, and `0x1c` for first, second, and both.  These
selectors therefore express component write modes inside a result-operation
family; they are not safe standalone ALU mnemonic assignments.

The lowerers pass the original destination descriptor through helper
`0x1afee0`, shift its result left by five, and insert it in `W0[7:5]`.  This
proves that `W0[7:5]` is a result-class/routing selector.  Descriptor bit 20
forces class 4; otherwise descriptor bits `27:24` map
`0,1,2,3,4..12,13,14,15` to `0,1,2,3,7,4,5,6`.  Semantic names for those
classes remain unknown.

Two helpers encode independent four-bit component masks into scattered `W1`
bits.  For input bits `m[3:0]`, `0x1af3e0` writes them to
`W1[26,12,8,6]`, while `0x1af430` writes them to `W1[27,13,9,7]`, in
`m[3],m[2],m[1],m[0]` order [DER].  The whole-program emitter invokes both
with `m=0xa`.  Their association with the two `W1` operation lanes is strong;
observed lowerer call sites also pass `2,4,5,6,0xa,0xe` according to format.
No call site found passes the source D3D write mask directly.  The `xyzw`
result order above therefore does not assign component names to these scattered
mask bits; that still awaits a controlled compiler delta.

### Source-operand expansion and stream splicing

Whole-program emitter `0x1db510` does more than pack one translated input
instruction.  Its second argument is the output stream and its third argument
is the current 0x88-byte instruction IR.  The tests below are its source-zero
auxiliary dwords copied from compiler-token `+0xa4/+0xa8`.  After cloning and
translating the input, it follows these mutually exclusive paths [BIN][DER]:

```c
if (instruction_ir_u32[0x60 / 4] != 0) {
        /* Replace the translated input with a complementary pair. */
        append(synthetic_op0_1d_op1_14_mask_a_b_0a);
        append(synthetic_op0_1e_op1_14_mask_a_b_0a);
        return success;
}
if (instruction_ir_u32[0x64 / 4] != 0) {
        append(synthetic_op1_00);
        append(the_same_synthetic_op1_00);
        *(uint32_t *)(output + 0x397) |= 0x20000000;
}
append(translated_input);
```

The order of `0x1d` and `0x1e` in the replacement pair can swap according to
a transformed descriptor bit.  Descriptor class bits are also adjusted by a
format property.  The safe conclusion is a format-dependent result-pair
expansion, not an export mnemonic assignment.  The `+0x64` pair is likewise a
compiler-generated control sequence whose public meaning is not yet known.

The source fields have a useful exact negative boundary.  MxMGLX clone
constructor `0x106ade90` copies all four 0x38-byte operand descriptors,
including source-zero descriptor `+0x28/+0x2c` into token `+0xa4/+0xa8`.
Its own bounded operand formatter `0x106afa90..0x106aff2c` reads descriptor
`+0x0c`, `+0x14/+0x18`, and `+0x20/+0x24` for the visible register index and
range forms, but never reads `+0x28/+0x2c` [BIN].  The auxiliary pair is
therefore hidden compiler annotation, not the printed relative-address or
range bounds.  Its two nonzero expansion effects above are proven; semantic
names remain unproved.

The one-slot packer has an independent side effect on the same output property
bit.  Before format gates can replace the overlapping physical field, it tests
the instruction object's internal `W1` high nibble for `8` or `9`, requires
expanded second operation `0`, and requires expanded third operation in
`{0x81,0x82,0x83,0x182,0x183}`.  Only that combination sets output
`+0x397` bit `0x20000000` [BIN].  A raw packed slot cannot always reconstruct
this test because the packer may replace `W1[20:14]` with a descriptor.

The direct consumer at `0x16ea00` fixes the property's compiler meaning.
Function `0x16ea25` tests bit 29 and returns zero immediately when it is set.
When clear, it allocates `count * 0xb4 + 4`, clones all counted native slots
through `0x16eb50`, performs its requested transformations, and repacks the
stream through `0x16eba0` [BIN][DER].  Bit 29 is therefore a compiler
transformation barrier for this rewrite pass.  It is out-of-stream metadata;
this evidence does not assign it a hardware `END`, export, or texture meaning.

A derived backend at vtable `0x1f5104` adds another context-sensitive path.
Emitter `0x1dcaf0` records generated-run boundaries in output bytes
`+0x39b/+0x39c` (using `+0x39d` temporarily).  Stream copier `0x14e900`
recognizes such a run, requires a boundary slot with `W0[14:8]=0x3d`, and
patches the last slot from the first slot: first `W0[14:8]` is copied into last
`W0[21:15]`, and last `W1[5]` records whether first `W0[4:0]` is `0x1e`.
For one following `W0[14:8]=0x21`, `W0[4:0]=0x1d` form, it replaces the same
field in the rest of the special run with `0x7f` [BIN][DER].  Native slot
meaning can therefore depend on compiler-run boundaries and neighboring
slots.  Fixed fragments must not be decoded as isolated source operations.

The low-operation implicit-descriptor table at `0x1dba44` contains these 32
case indices [BIN]:

```text
00 01 02 03 00 01 02 03 00 01 02 03 04 05 04 05
00 01 02 03 00 01 02 03 00 01 02 03 06 06 06 07
```

Cases `0..6` return, respectively, `0x64000000`, `0x68000000`,
`0x64000001`, `0x68000001`, `0x6c000000`, `0x6c000001`, and
`0xdc000000`; case 7 returns the absent sentinel.  A second table at
`0x1dbb14`, indexed by the same `W0[4:0]` selector, maps to a four-bit
operation-derived component form:

```text
0c 04 0c 04 08 0f 08 0f 07 0b 07 0b 02 02 02 02
0c 04 0c 04 08 0f 08 0f 07 0b 07 0b 0e 0e 0e 02
```

The sparse `W0[14:8]` table at `0x1dbbd0` returns a descriptor only for
selectors `1,2,3,4,8,12,16,32,48`, mapping respectively to
`0x64000000`, `0x68000000`, `0x6c000000`, `0x64000001`,
`0x68000001`, `0x6c000001`, `0xd4000000`, `0xd8000000`, and
`0xdc000000`.  All other values return the absent sentinel [BIN].  These
constants expose register/resource classes but do not supply safe class names.

No native disassembler string table was found.  In particular, the readable
`mov`, `mad`, `rcp`, and related switch at `MxMGLX.dll` `0x106ae3b0` decodes
the 16-bit Iris token opcode described in the companion document.  The Linux
ICD has the corresponding formatter strings near `0xecbd6b`.  Neither is a
decoder for these native 64-bit slots, so Iris opcode numbers must not be
copied into the fields above.

This also fixes the static boundary for a complete ALU/texture/control
mnemonic table.  The exhaustive 32-entry native tables at `0x1dba44` and
`0x1dbb14` select implicit descriptors and component forms, not names; the
sparse seven-bit table at `0x1dbbd0` does the same.  Conversely, one expanded
selector such as `0x14` occurs in the lowerings of MOV, FRC, ABS, MOVA, RSQ,
EXP, and LOG.  The mapping is many-to-many and depends on the other operation
fields, descriptors, gates, and neighboring generated slots.  The saved ICD
exports only its public `Drv*` surface, while MxMD is a native-subsystem image,
so no supported offline compiler entry point was found.  Numeric operation
families and their source-operation anchors are reproducible; assigning a
complete native mnemonic table still requires captured compiler output or a
native slot decoder.

## Dynamically built fragment

Windows `0x147d30` and Linux `m9x25489` at `0x445ea0` allocate four slots; the
resource copy path at Windows `0x120a30` uses `count * 8`.  The exact program is:

```text
{ 0xffc0291d, 0xe29fff1f }
{ 0xffc1291e, 0xa29fff1f }
{ 0x00000000, 0x00000000 }
{ 0x00000000, 0x00000000 }
```

The builder independently manipulates dword 0 low values `0x1d/0x1e`, dword 0
bit 16, dword 0 bits `22:15`, and dword 1 fields selected by masks
`0xfe9fffff`, `0xf7ffff7f`, and `0xfbffffbf`, plus bits `0x800`, `0x400`,
`0x20`, and `0x10`.  Joined to the backend packer, the first two slots have
operation tuples `(W0[4:0],W1[25:21]) = (0x1d,0x14)` and
`(0x1e,0x14)`.  Their reconstructed split selector
`(W1[3:0] << 3) | W0[31:29]`, `W0[28:22]`, and the untouched
`W1[20:14]` are all `0x7f`, the packer's default seven-bit sentinel.  Slot 1
sets `W0[21:15]=2`, while slot 0 leaves that field zero.  The encoder's
whole-program path also constructs the `0x1d/0x1e` family, and analysis routine
`0x1da987..0x1daa74` treats `0x1c/0x1d/0x1e` paired with second operation
`0x14` as a special family.  This proves the numeric operation selectors and
their pairing.  The `0x1d/0x1e` values are specifically the complementary
first/second result-component forms proved above, rather than evidence for
terminal or export mnemonics.  The purpose of the complete two-live-slot
fragment remains unresolved.

## Predicate and termination boundary

### Minimal MOV programs: exact frontend and remaining native join

The smallest compiler input is now reproducible.  Helper `0x1940b0` zeroes one
`0x88`-byte backend IR record at `0x1940be..0x1940d0`, installs the normal
absent-descriptor sentinels, and its first call at `0x19413d..0x194172` constructs an
ordinary D3D `MOV` with exactly these live fields [BIN]:

```text
IR +0x0c = 1             D3D MOV
IR +0x14 = destination descriptor
IR +0x1c = source-zero descriptor
other operation and source fields remain zero/absent
CALL 0x1b5390
```

This removes an ambiguity between the source-token and backend-IR layouts.  A
full four-component move uses destination mask `0xf` in descriptor bits
`19:16`; identity source swizzle is `0xe4` in bits `23:16`.  After the already
proved link transforms, an input at physical link `p` is
`0x10000000 | 2*p` before its source swizzle is restored, an ordinary vertex
output is `0x0000ffd8 + 2*p`, and the special output form is `0x0000fff8`.
A constant source `c[i]` becomes `0x20000000 + 2*i`; because its top nibble is
`2`, generic lowerer `0x1b5972..0x1b5a6a` places it in intermediate descriptor
`+0x10`, sets format gate `1`, and the one-slot packer writes its low seven bits
to `W1[20:14]`.  Thus `c0` selects native descriptor value zero in that gated
field [BIN][DER].

The raw two-dword move still cannot be generated from those facts alone.
`MOV` dispatch at `0x1b552b..0x1b55a9` examines descriptor bits `27:24` and, for
the normal zero case, also reads compiler context `+0x1f0/+0x1f4`.  It selects
expanded second operation `0x14` with mode 10 only when `+0x1f4 == 0` and
`+0x1f0 >= 0x400`; otherwise it selects operation `0x15` with mode 6.  The mode
is then scattered through `W1` by `0x1b5813..0x1b58a6`.  A guessed single-slot
`MOV` encoding would consequently choose one of two compiler-generated forms
without knowing the stage/compiler configuration [BIN].

The two context fields now have an exact producer and whole-program join.
The live compile/install path at `0x4569f..0x456d4` copies the request type to
`ctx+0x1e8`, the outer object at `+0x4f8` to `ctx+0x1f4`, and the adjacent
outer value at `+0x4f4` to `ctx+0x204`.  Installer `0x4dfe0` switches on the
seven request types.  Types 2 and 5 copy the installed object's `+0x1c` to
`ctx+0x1f0`; types 0 and 6 allocate an auxiliary object and copy `ctx+0x1f4`
into its first dword; types 1, 3, and 4 do neither [BIN].  Finalizer
`0x195b61..0x195b8c` evaluates the identical predicate and passes it as the
fourth argument to whole-program backend `0x1d1d40` [BIN].  This proves that
the `0x14/mode-10` versus `0x15/mode-6` choice is a program/backend mode, not
an output-register or shader-stage discriminator.  Static code does not prove
that outer `+0x4f8` is nonzero for every live vertex or fragment request, nor
does it name request types 2 and 5.

Fresh-instruction construction rules out a hidden default that could close
this gap.  Windows constructor `0x1da2c0` initializes descriptors
`+0x10..+0x24,+0x38,+0x3c` to `0xf0000000`, zeros both operation words and gate,
and leaves no terminal/export value.  Linux `m9x15235` at `0x516d40` performs
the same initialization with fields shifted by four bytes after its 64-bit
object header [BIN].

### Class-12 is an intermediate special result, not the terminal endpoint

Frontend `0x1a4cdc..0x1a4ced` increments program counter `+0x5a8` whenever
the converted destination low word is `0xfff8`.  Program finalizer `0x1c8590`,
called unconditionally from optimizer `0x195991`, reads that counter and
synthesizes ordinary opcode-1 IR records through the same generic lowerer
[BIN]:

```text
condition                  opcode  destination  source
+0x5a4 <= 0, +0x5a8 > 0      1    0x3001fffe   0x0000fff8
+0x5a4 > 0,  +0x5a8 > 0      1    0x3003fffe   0x0044fff8
```

The latter path also builds a helper record with destination `0x0002fff8` and
sources `0x0000fff9/0x0055fff9`.  Class 12 / selector `0x78` is therefore an
intermediate special result, while `0x3001fffe` (or the multi-result form) is
the final synthetic endpoint.  The finalizer appends these bridge MOVs before
counted native emission; there is no separate `END` record synthesized by this
frontend-finalization step [DER].  Native hardware stop behavior remains
bounded below because the later backend receives an explicit program count but
the bind carries no count or end slot.

The saved GL ICD independently assigns its special vertex and fragment outputs
to a class numbered 12.  The vertex parser recognizes `gl_Position` at
`0x1071e725` and the fragment parser recognizes `gl_FragColor` at
`0x107275c5`; both call the same helper `0x1071ff00`.  That helper builds a
nine-dword symbol record and writes literal 12 at record offset `+0x10` (the
fifth dword) before
installing it through `0x10714620` [BIN].  This proves that class 12 is the
stage-relative final color/position result in the GLSL frontend: it means
`gl_Position` in a vertex compilation and `gl_FragColor` in a fragment
compilation.  It does not by itself prove that the identically numbered MxMD
link class survives the private serialization unchanged.  That last
cross-module identity still stops at the unrecovered opcode-`0x59/0x67`
transport consumer documented in `parhelia-m9-program-stage-entry-re.md`.
This also supplies hard negative evidence against assigning expanded operation
`0x14` or `0x15` to one stage by name.  Both stage-final symbols reach the same
frontend helper, while MxMD chooses `0x14/mode-10` versus `0x15/mode-6` only
from `ctx+0x1f4 == 0 && ctx+0x1f0 >= 0x400`.  Until the serialized stage flag
is joined to those context values, neither operation has fixed vertex or
fragment ownership [DER].

Two bindings are still absent for a runnable minimal pair:

* both API final outputs are class 12 in MxMGLX and class 12 feeds the terminal
  endpoint MOV in MxMD, but the private transport consumer that would prove
  these are the same class is absent, and no static path selects the
  context-dependent native MOV form for either stage;
* sparse four-dword upload records have an exact key-to-selector formula, but
  no static data flow joins an IR `c[i]` descriptor to a sparse-record key.
  In particular, assuming that `c0` means sparse key zero would be a guess.

Combined with the count/termination result below, this is the exact static
boundary for a passthrough vertex shader and solid-color fragment shader.  We
can build their compiler IR, link ordinary varyings, identify the constant
read selector, upload native bytes, and bind two entry starts.  We cannot yet
prove the class-number identity across the private interface, choose the
context-dependent raw `MOV` form, bind the solid-color payload to its sparse
key, or prove the hardware stop operation.  The saved fixed fragments cannot
close those joins: their complete selected chains have no common terminal bit.

The smallest decisive compiler delta is now narrower.  Compile a one-MOV
program twice while holding its IR, varying map, and constants fixed, changing
only the stage.  Break after `0x195991` and record `ctx+0x1e8/+0x1f0/+0x1f4`,
then break after `0x195b88` and copy the counted slots.  Dumping the converted
destination descriptor in each run proves whether the frontend class-12
ordinal crosses the private interface unchanged.  This two-compile capture
resolves the stage enum, the native MOV branch, and the final class join without
MMIO tracing.

The static databases do not expose a standalone native predicate or `END` bit.
Across the final slot of all 48 fixed blobs, the bitwise intersection is zero
for both dwords: no bit is set in every fragment ending.  Several fragment
tails are all-zero slots, and linker `0x13bf80` appends identical all-zero slots
to round a linked stream to four-slot upload lines.  An all-zero slot is thus
padding/NOP-like and cannot be treated as a mandatory encoded `END`.  Because
the fixed blobs are composable pieces, this negative result alone does not rule
out a terminator in a complete program.  The per-instruction expansion above
supplies stronger negative evidence against a universal per-slot `END`: the
backend can replace one instruction, prepend two slots, patch fields while
splicing runs, and set an output-program property outside the stream.  The
property's compiler meaning is the rewrite barrier proved above, not `END`.

The compiler-native stream itself is demonstrably count-delimited.  Packer
`0x1db98d..0x1db9a5` reads output count `+0x04`, rejects an append past 256
slots, writes the two-dword slot at `+0x08 + 8*count`, and increments the
count.  Clone helper `0x16eb50` iterates exactly while its index is below that
same count, and advances its expanded instruction destination by `0xb4`
[BIN][DER].  Neither path scans for or appends an in-band terminator.  The D3D
frontend `END` token, compiler stream count, four-slot upload padding, and any
hardware fetch-stop state are separate layers.  The final program-state join
is now exact.  Windows uses linked `+0xbb0` in only the resource cleanup test,
the `0xf6ecd` command-size calculation, and the `0xf7713..0xf7813` CPU upload
loop.  The loop advances its source and byte offset by `0x20`, writes selector
`68` followed by ports `60..67`, and stops when the offset reaches `+0xbb0`.
Its size calculation is exactly `15 * (+0xbb0 / 32) + 10` command dwords.
Linux independently performs the same calculation at `0x4b7c0b`, reads
`+0xbd8/+0xbe0` as length/pointer, and has the same 32-byte loop at
`0x4b9450..0x4b9563` [BIN].  Exhaustive direct-displacement xrefs in the two
emitters find no later use of either length.

Windows `0xf7819..0xf78aa` and Linux `m9x11728`
`0x4b9700..0x4b97a5` then program one or two 16-bit instruction starts plus
literal `69 <- 4`, but no instruction count or end address [BIN].  Consequently
the aligned byte length is a host-to-instruction-RAM transport bound only; the
hardware cannot consume it as an execution count.  Zero fill is alignment
padding, and the compiler and linker append no distinguished zero sentinel.
The next entry cannot be the general end either: the split bind moves that
entry to `6d`, and neither form supplies an end for the final entry.  Compiler
property `+0x397` bit 29 cannot terminate hardware because it is consumed only
as an optimizer rewrite barrier and never reaches the bind.  These paths rule
out compiler count, aligned byte count, padding, next-entry boundary, and that
property bit as hardware stop rules [DER].

The remaining stop rule is in-stream native control or an implicit engine
condition.  It is not one universal terminal encoding visible in the fixed
database: the lists selected by `0x13b2b0/0x13b570` are complete entry lists,
linker `0x13bf80` copies their exact fragments without inserting a record
between them, and the final live slots of the selectable tails have no common
bit in either dword.  The mandatory output finalizer similarly adds a normal
opcode-1 bridge MOV and no separate `END`.  Static binaries therefore do not
distinguish a terminal operation embedded in one of the varying tail forms
from an engine stop triggered by completing the stage-relative endpoint.  A
native compiler slot must not be assigned a universal `END` bit from this
evidence.

The final compiler-side invariant has now been isolated more tightly.  All
three synthetic endpoint destinations (`0x3001fffe`, `0x3002fffe`, and
`0x3003fffe`) preserve low selector `0x7e`; generic lowerer `0x1b5390` sets
destination gate `0x04`, and packer `0x1db902..0x1db914` copies that selector
to `W0[14:8]`.  The ordinary stage-relative result feeding it has selector
`0x78`.  Thus termination is narrowed to a write to endpoint selector `0x7e`
or the complete operation/control form of that final MOV.  Static data cannot
choose between those two.  The cross-version bind proof and smallest decisive
capture are recorded in `parhelia-m9-shader-termination-boundary.md` [BIN][DER].

The selected fixed-function image reinforces that boundary.  Linker
`0x13bf80` records the starts of `P` and its `S/Q` continuation at
`+0xba8/+0xbac`; companion selector `0x13b570` sets `+0xba4` only when `Q`
overrides `S`.  With no override the starts are packed into the low/high halves
of `6a`.  With an override the primary start remains in `6a[15:0]` and the
companion start moves to `6d[15:0]` [BIN][DER].  Neither adjacency nor the
second start can safely be treated as the first run's exclusive end, because
the split form does not encode such a range and supplies no end for the
companion run.

`MxMGLX.dll` supplies a useful cross-layer negative check.  Its legacy Matrox
fragment-shader validator at `0x1012c1c0` rejects a zero API-instruction count,
then at `0x1012c215` multiplies that count by its `0x94`-byte frontend-record
stride and requires result class `4` in the final record.  The associated
diagnostics are exactly “Too few instructions” and “Final instruction does not
write to output register” [BIN].  This proves a frontend final-instruction
rule, but that count is absent from the MxMD/Linux native bind above and must
not be repurposed as a hardware execution bound.

The compiler-to-MxMD predicate path is now joined statically.  Source token
`+0x08` bit 1 becomes backend IR `+0x04` predicate-enable, and the predicate
descriptor at token `+0x0c` becomes IR `+0x30`.  Converter `0x1a3ac0` accepts
three files: `cb(14)` becomes `0x20000000+2*i`, `p(19)` becomes
`0x60000000+2*i`, and internal `rb(30)` becomes `2*i` [BIN][DER].  The `p`
case is the ordinary predicate-register class.

Generic lowerer `0x1b5390` consumes both IR fields for every emitted component.
It passes IR `+0x30`, predicate enable, the component number, and the selected
two-bit component form to helper `0x1afd30`; the returned value replaces
intermediate `+0x0c` low five bits, which pack directly as `W0[4:0]`.  It also
copies predicate-enable into intermediate gate bit `+0x0e[10]` and conditional
marker IR `+0x00` into gate bit `+0x0e[12]`.  Those gate bits are compiler
metadata and are not copied verbatim into the packed slot.  Predicate polarity
from descriptor bits `27:24` changes the helper's returned operation family.
Thus this backend lowers a predicate into the per-component `W0[4:0]`
operation form; there is no evidence for one universal packed predicate bit.
The special lowerers now give the exact distribution boundary.  Their
result-bearing calls to `0x1afd30` all reload IR `+0x30` and `+0x04`: RCP has
two such sites (`0x1b0fbc`, `0x1b1168`), RSQ two (`0x1b195c`, `0x1b1ae5`),
EXP two (`0x1b2350`, `0x1b24a1`), and LOG three (`0x1b2bb7`, `0x1b315f`,
`0x1b32ca`) [BIN].  RCP also calls the helper at `0x1b0cae` with a zero
descriptor for an internal preparatory result.  Thus the source predicate is
distributed to each externally visible result form while that RCP internal
form is deliberately descriptor-free.  Other synthetic objects in these
sequences do not pass through the predicate helper.

The remaining predicate boundary is the complete numeric mapping of the
helper's operation-family results under every descriptor polarity and mask.
It still needs controlled compiler output around the call to `0xadf00`; a
standalone packed predicate bit should remain unnamed.

The next static/runtime hooks should log `(slot, descriptor pointer, length)` in
`0xadf00` while `0x1454a0` runs, then dump the unpadded linked stream immediately
before `0x13bf80` rounds it to 32 bytes.  Comparing those 64-bit pairs with
controlled Iris source variants will expose compiler-to-native deltas before
MMIO capture is needed.

`tools/m9-shader-diff.py` automates that comparison.  It treats raw files as
two-dword slots, reports the containing four-slot upload line and lane, and
prints XOR masks plus changed bit ranges for each differing instruction.  Its
`--fields` mode also prints the already-proven numeric operation selectors,
result-routing selector, overlapping seven-bit descriptor fields, split
selector, and both scattered component masks before and after each change.  It
labels only the proven `0x1c/0x1d/0x1e` result-component modes and all-zero
slots; it deliberately does not guess native ALU, texture, predicate, or
termination names.

`tools/m9-native-static-evidence.py` checks the two saved PE identities and the
Linux `m9x_drv.so` identity, uses proper PE image-base/RVA translation, verifies
the fixed-fragment hash, emitter vtable slot, matching Windows/Linux native
instruction constructors, canonical minimal-MOV IR call site, and the
context-dependent MOV decision.  It also decodes the D3D and
register-file dispatch tables, verifies final varying transforms, special
predicate sites, count-delimited append/clone paths, the property-bit rewrite
barrier, the exact linked starts and packed/split bind forms, the
operand-formatter negative boundary, and the four fixed-function vertex
state-builder calls.  Run it from
the repository root; a table or binary mismatch is a hard error.
