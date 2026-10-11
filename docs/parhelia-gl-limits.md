# Parhelia GL capability limits and the state-validation path

What the vendor OpenGL ICD declares the hardware can do, what it actually
enforces, what the Windows Direct3D driver declares for the *same silicon*, and
how a GL state change becomes a hardware pipeline configuration.

Sources, all read directly:

* `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0` — the Linux
  ICD (ELF64, stripped but with C++ RTTI names). Disassembly:
  `scratchpad/libGL64.asm`. Section note: `.rodata` has vaddr == file offset;
  `.data` and `.data.rel.ro` have vaddr = file offset + 0x200000.
* `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` — the kernel module,
  **with symbols** (`mtxNNNN`). Disassembly: `scratchpad/mtxk.asm`.
* `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` — the Windows XPDM D3D
  driver. Disassembly: `scratchpad/pard.asm`.
* **`matroxdriver-x86_64-cvs-20090511/opengl6/include/glext.h`** — the vendor's
  own header, shipped in the driver tree. It declares the complete
  `GL_MTX_fragment_shader` token block (lines 1446 ff.: get-parameters, texture
  opcodes, ALU opcodes, registers, result and argument modifiers, replicate
  channels, texture formats, sampling hints) **and** the full
  `GL_EXT_vertex_shader` block (line 1298 ff., prototypes at 2045 ff.). Names
  that would otherwise have to be inferred are simply written down here; it
  should be cross-referenced from `docs/parhelia-fragment-isa.md` and
  `docs/parhelia-vertex-isa.md` too.
* `captures/glsweep-0528-raw.txt` — the live 33-op sweep on a Parhelia 0x0528,
  decoded in `docs/parhelia-gl-feature-map.md`.

Tags: **[CONFIRMED]** = read out of the code/data, or two independent sources
agree; **[INFERRED]** = best reading, self-consistent but not proven;
**[AMBIGUOUS]** = unresolved, do not build on it.

Scope note: the M9 (0x0540) is a different render engine and is not covered.

---

## 1. The PIPELINE-KEY — decoded

**It is a bitfield: a packed vertex-format / pipeline descriptor.** Not a hash,
not a program handle, not an index into a table of pipeline configurations. Every
field is a small integer describing one vertex attribute slot, and the record's
other five dwords are constants. The decode below accounts for **all six** key
values in the live capture and for their vertex strides, exactly.

### 1.1 The record

`docs/parhelia-gl-feature-map.md` §2 records it as `(0x07, 1)` + 6 dwords
`{key, 0, 0, ffffffff, ffffffff, ffffffff}`. Both halves of the stack agree on
that shape [CONFIRMED]:

* ICD: emitted by `reserve(buf, opcode=0x07, elemsize=0x18, count=1)` — three
  emit sites, `libGL64 0xc5113` and `0xc76b8` (fixed-function paths) and
  `0xf3461` (`GLPrimitiveSoftware` path). All three copy six dwords from a
  cached struct, which is the only place the key ever comes from.
* Kernel: command-list opcode 0x07 → handler `.text+0x45d02` → `mtx651`
  (`0x471c0`) → **`mtx1763` (`0x1b8d0`)**, which copies exactly **24 bytes**
  into the device state block at `+0x1e04` and sets dirty bit 0x20 in the byte
  at `+0x1bec`. The interpreter then advances the stream by `count*3` qwords
  (`0x45ce1`), i.e. 24 bytes per element — matching `elemsize` on the ICD side.

The reset value of that block (`mtx2556`, `0x1bbbd`) is
`{0, 0, 0, -1, -1, -1}` with a 2-bit field at key bits[30:29] forced to 2
(`byte3 = (byte3 & ~0x60) | 0x40`). The ICD's shadow copy is initialised to
`{0xffffffff, 0, 0, -1, -1, -1}` at `libGL64 0x10adb0` so the first draw always
emits. The three `0xffffffff` dwords are therefore just the *default* of three
further descriptor fields that the fixed-function and software paths never
change [CONFIRMED that they are defaults; their meaning is [AMBIGUOUS] — most
likely bound-stream or bound-shader handles, since the hardware-vertex-shader
path is the only one that could set them].

### 1.2 Key bit layout

Recovered from three independent places that all agree:

* the fixed-function builder `libGL64 0xc3f20` (key lives at `ctx+0xa560`),
* the `GLPrimitiveSoftware` builder `libGL64 0xf30e0` (key at `obj+0x25b0`),
* the matching per-vertex writer `libGL64 0xf37b0`, which walks the same fields
  to decide what to store per vertex — this is what pins each field to a real
  attribute and a real byte count.

| bits | field | values seen | vertex cost |
|---|---|---|---|
| [1:0] | **position format** | 0 = `x,y`; 1 = `x,y,z`; 2 = `x,y,z,w` | 8 / 12 / 16 B |
| [3:2] | **normal** | 0 = absent, 1 = present (`nx,ny,nz`) | 0 / 12 B |
| [6:4] | **diffuse colour** | 0 = absent, 1 = `r,g,b`, 5 = `r,g,b,a` | 0 / 12 / 16 B |
| 7 | unused in every path seen | 0 | — |
| [10:8] | **secondary colour / specular / fog coordinate** — one shared slot, see §5.5 | 0 = absent; 1 (fixed-function) / 5 (software) = present | 0 / 16 B |
| 11 | software-T&L / pre-transformed marker | set only by the `GLPrimitiveSoftware` builder | — |
| [15:12] | unused | 0 | — |
| [18:16] | **texcoord set 0** | 0 = stage off, else component count (2 fixed-function, 4 software) | n × 4 B |
| [21:19] | **texcoord set 1** | same | |
| [24:22] | **texcoord set 2** | same | |
| [27:25] | **texcoord set 3** | same | |
| 28 | **per-vertex point size** — one extra dword [INFERRED, see below] | from `obj+0x2688` | +4 B |
| [30:29] | 2-bit selector, kernel default 2, fixed-function sets 1 | | [AMBIGUOUS] |

The four texture-stage shifts **{16, 19, 22, 25}** are not inferred — they are
read from literal tables in `.rodata`: `0x36ada0` (used by the software builder,
`0xf31a5` and `0xf3895`) and `0x369830` (used by the fixed-function builder,
`0xc4112`). Both contain `16, 19, 22, 25` [CONFIRMED].

The fixed-function builder reads its inputs from a small byte array of
"requested attributes" at `ctx+0xa530`, which is where a future pass should look
to name the remaining fields: `+0xa530 & 3` → position code (`0xc3f88`),
`+0xa53a` → normal (`0xc3fc9`), `+0xa538`/`+0xa539` → colour (`0xc4011`,
`0xc4020`), `+0xa531` bit 2 and bit 7 → secondary-colour selectors (`0xc4062`,
`0xc40b6`), `+0xa540` → the bits[30:29] selector (`0xc40d1`), `+0xa534` → a
6-bit-per-stage texture word read with the shift table `0x369840 = {0,6,12,18}`
(`0xc4140`). In the software builder the bit-28 attribute comes from
`obj+0x2688` (`0xf3208`, and the matching per-vertex store at `0xf381d`, which
writes a single dword from an array at `ctx+0x25a8+0x240`). That flag is set at
`0xfce1c` when a byte in the point-parameter block, `state+0xad1`, is on **and**
either polygon mode is `GL_POINT` (0x1b00) or `obj+0x284` is zero. The
neighbouring bytes of that block are named by `glGet`: `state+0xad0` =
`GL_POINT_SMOOTH` (0x0B10), `+0xad4` = `GL_POINT_SIZE` (0x0B11), `+0xad8/+0xadc`
= `GL_POINT_SIZE_MIN/MAX`, `+0xae0..0xae8` = `GL_POINT_DISTANCE_ATTENUATION`
(0x8129). So key bit 28 is **a per-vertex point size**, the attribute
`GL_ARB_point_parameters` needs, and it matches D3D's `FVFCaps` PSIZE bit
[INFERRED, but from two independent directions].

Field values [3:2], [6:4] and [10:8] are *codes*, not component counts — the
builder ORs in a constant per case (`|1`, `|5`, `|0x12`, …) and later uses the
code as an index into a table of per-attribute copy routines
(`obj+0xb0d0`, `obj+0xb3b0`, `obj+0xb7d0` at `0xc4002`, `0xc408e`, `0xc4128`).
Only the four texture fields hold a literal component count.

### 1.3 Every captured key, decoded

| key | position | normal | diffuse | tex | components | stride | capture |
|---|---|---|---|---|---|---|---|
| `0x00000010` | `x,y` | – | RGB | – | 2+3 = 5 | **20** | plain ✓ |
| `0x00000011` | `x,y,z` | – | RGB | – | 3+3 = 6 | **24** | depth ✓ |
| `0x00000051` | `x,y,z` | – | RGBA | – | 3+4 = 7 | **28** | blend ✓ |
| `0x00000005` | `x,y,z` | yes | – | – | 3+3 = 6 | **24** | lighting ✓ |
| `0x00020011` | `x,y,z` | – | RGB | set0 = 2 | 3+3+2 = 8 | **32** | textured ✓ |
| `0x00000852` | `x,y,z,w` | – | RGBA | – | 4+4 = 8 | **32** | wide line ✓ |

Every stride in `docs/parhelia-gl-feature-map.md` §4 falls out of the bit
decode with no free parameters [CONFIRMED]. This also disposes of the
"not a monotonic bitmask / lighting drops bit 4" puzzle: bit 4 is not a flag, it
is the low bit of the 3-bit **diffuse colour** field, and lighting has no
diffuse colour in the vertex — it has a normal instead, in the [3:2] field.

The strides are not derived after the fact either: the builders accumulate the
byte count into `obj+0x25d0` alongside the key (`+0x20` base, `+0x10` per active
texture stage, `+0x10` for secondary colour, `+4` for the bit-28 attribute —
`0xf3159`, `0xf31f3`, `0xf319e`, `0xf3216`), and that same word is what the
`(0x08, 1)` vertex-format record carries at payload offset `+0x10` (`0xf3404`)
— the field the capture calls "vertex stride in bytes" [CONFIRMED].

### 1.4 The static template table — direct proof

The fixed-function path does not always compute the key; for common vertex
layouts it picks a **pre-built descriptor** from a table of function-local
statics in `.bss`, each built once behind a `__cxa_guard`. Reading the
constructors gives the key values literally:

| descriptor | key | builder | decode |
|---|---|---|---|
| `0x69a860` | **0x00000051** | `0xc4744` | pos `xyz` + diffuse RGBA |
| `0x69a880` | **0x00000011** | `0xc46b9` | pos `xyz` + diffuse RGB |
| `0x69a8a0` | **0x00000005** | `0xc4597` | pos `xyz` + normal |
| `0x69a8c0` | **0x00020005** | `0xc4500` | pos `xyz` + normal + texcoord0 (2) |
| `0x69a8e0` | **0x00000015** | `0xc4475` | pos `xyz` + normal + diffuse RGB |
| `0x69a900` | **0x00000001** | `0xc43ea` | pos `xyz` only |
| `0x69a920` | **0x00120005** | `0xc4622` | pos `xyz` + normal + texcoord0 (2) + texcoord1 (2) |

Each constructor writes the same 24-byte shape: key, two zero dwords, three
`0xffffffff` [CONFIRMED]. Three of these keys (`0x05`, `0x11`, `0x51`) are
byte-for-byte the values the live capture recorded, and `0x00020005` /
`0x00120005` show the texture field taking the value **2 = two texcoord floats**
at shift 16 and 19 — which is what makes `0x20011` in the capture readable at a
glance.

### 1.5 What the kernel does with it

`mtx1763` stores the block and sets a dirty bit; **nothing in `mtx_parhelia.o`
reads `+0x1e04` back** (the only three references to that offset are the one
writer and two initialisers) [CONFIRMED]. So the pipeline key is *not* itself
programmed into a register. It is the vertex-format contract between the ICD and
the kernel's vertex-fetch setup, kept in the per-context shadow for state
restore. An open driver reproduces its *effect* by agreeing on the vertex layout
with its own vertex fetcher; it does not need to reproduce the encoding.

---

## 2. The command-record grammar (needed for everything below)

The ICD builds its stream through two tiny helpers, and reading them settles
several open questions in `docs/parhelia-gl-feature-map.md`.

`reserve(buf, opcode, elemsize, count)` — **`libGL64 0x1c9c50`** [CONFIRMED]:

1. if `buf[0x34]` is set, call the context's virtual `[0xb8]` to (re)open the buffer;
2. if `write_ptr + 8 + elemsize*count` passes the end, **flush** (`0x1c9be0`);
3. **if the previous record's opcode equals this one, just add `count` to its
   header and append** — otherwise write a fresh `(opcode, count)` header and
   remember it in `buf[0x28]`;
4. return the payload pointer; `buf[0x30] = elemsize*count`.

`commit(buf)` — **`0x1c9ad0`**: `write_ptr += buf[0x30]`.

Consequences:

* A "run of `(index, value)` pairs" in the capture is **one record** with a
  large `count`, not many records. That is the coalescing in step 3.
* `flush` (`0x1c9be0`) submits `[buf+0x18] .. write_ptr` through the device's
  virtual `[0x60]`, then **rewrites the first two dwords of the buffer as
  `(0x000000ff, 0x00000001)`** and points both the write pointer and the
  "previous record" pointer at the buffer base (`0x1c9c13`–`0x1c9c28`).
  **This is the "CLOBBERED first token" of `docs/parhelia-gl-feature-map.md`
  §2** — it is not a clobber and not a lost record: it is a post-submit sentinel
  whose only job is to make the opcode compare in step 3 fail, so the next
  record cannot coalesce into a stale one. The capture reads the buffer after
  the reset, so the *previous* stream's first record has already been
  overwritten [CONFIRMED, and it removes the need to guess the first token].

### 2.1 Complete opcode table

Left: every `reserve()` call site in the ICD (279 of them), grouped by
`(opcode, elemsize)`. Right: the kernel command-list interpreter `mtx558`
(`0x44ef0`), whose 82-entry jump table at `.rodata+0x1b08` dispatches opcodes
0x00–0x51 (opcodes with no handler fall to the default at `.text+0x452b8`).

| op | elemsize | ICD sites | kernel handler | role |
|---|---|---|---|---|
| 0x05 | 8 | 53 | `mtx1734` `0x1abf0` | **render-state bank A**, index ≤ 0x1c |
| 0x06 | 8 | 11 | `mtx3081` `0x1ae50` | **texture-stage state**, `(u16 group ≤ 10, u16 sub ≤ 10, u32)` |
| 0x07 | 0x18 | 3 | `mtx651` → `mtx1763` | **PIPELINE-KEY** (§1) |
| 0x08 | 0x20 | 2 | `mtx2530` | **vertex-format / attribute declaration** |
| 0x0a | 0x48 | 2 | inline `0x45ad9` | |
| 0x0c | 8 | 111 | `mtx2617` `0x19950` | **render-state bank B**, index ≤ 0xab; 0x101–0x200 = enable bitmap |
| 0x0d | 0x20 | 4 | `mtx3074` | |
| 0x0e | 8 | 34 | inline `0x459dc` | **texture-stage state array**, `(u16 group ≤ 10, u16 sub, u32)`, 130-byte structs at `+0xdd7` |
| 0x12 | 0x10 | 4 | inline `0x45f5f` | vec4 / clip rect (shadowed at `+0xcd9`) |
| 0x15 | 0x24 | 1 | inline `0x45d83` | **CLEAR** (9 dwords) |
| 0x19 | 0x20 | 1 | `mtx790` | |
| 0x1a | 0x18 | 1 | `mtx1314` | |
| 0x1b | 0x28 | 1 | `mtx2196` | **DRAW** (10 dwords) |
| 0x1c | 0xc8 | 1 | `mtx1385` | |
| 0x20 | 0x44 | 6 | | matrix upload: slot + 16 floats |
| 0x21 | **0x70** | 1 | `mtx227` | **light / material block — 112 bytes, not 120** |
| 0x21 | 8 | 1 | | |
| 0x22 | 0x48 | 1 | `mtx2441` | |
| 0x23 | 0x10 | 1 | | 4 floats (light-model ambient) |
| 0x24 | 8 | 1 | `mtx3554` | |
| 0x25 | 0x14 | 2 | | |
| 0x26 | 0x10 | 3 | | viewport rect |
| 0x27 | 8 | 2 | | depth range (2 floats) |
| 0x35 | 0x10 | 2 | | |
| 0x36 | 8 | 1 | `mtx1688` | point size + line width (2 floats) |
| 0x37 | 0x10 | 1 | `mtx2824` | |
| 0x40 | — | 1 | `mtx2166` | bulk vertex data |
| 0x41/0x42 | 8 | 1/2 | `mtx2488` | |
| 0x44 | 0x30 | 1 | `mtx631` | |
| 0x45 | **0x670** | 1 | `mtx3443` | shader microcode |
| 0x46 | **0x678** | 5 | inline `0x453d1` | shader microcode (the 0x664-byte fragment program + header) |
| 0x47 | 8 | 3 | `mtx2833` | |
| 0x48 | 0x18 | 5 | | |
| 0x49 | 0x14 | 2 | `LocMemCpy` | |
| 0x50 | 0x20 | 1 | `mtx3996` | |
| 0x51 | 8 | 1 | `mtx4017` | |

**Opcodes 0x45 and 0x46 are the same program object, defined two ways**
[CONFIRMED]: both carry a **0x664-byte** blob (the fragment/shader program
object of `docs/parhelia-fragment-isa.md`). `0x45` (`libGL64 0x1137d4`) is
`memcpy(payload, blob, 0x664)` with no header — elemsize 0x670 is the padded
size. `0x46` (`0x113a34`, and four more sites) prepends an 8-byte header holding
a **program id taken from an incrementing counter** at `[[ctx+0x20]+0x7c00]+0x68`
(`0x113a5b`: `id = ++counter`), then the same 0x664 bytes — elemsize 0x678. That
is the `{0x46, 1, program_id, 0}` header the capture found with `program id = 3`,
and it explains why `tex_env_modulate` uploaded nothing: the id is only minted
when a *new* program object is created, so a cache hit re-selects an existing id
through the program-select write instead.

The two texture-stage banks are sized by the ICD's own reset routine
(`libGL64 0x10a812`–`0x10aaa6`), which emits opcode 0x0e with `count = 0x70` as
**4 stages × 28 states** (`group = 0..3`, `sub = 0..0x1c`, stage struct stride
0x80 at `ctx+0x400`), and opcode 0x06 with `count = 0x24` as **4 stages × 9
states** (`0x10acb9`–`0x10ad95`, stage stride 0x28). So the kernel's `group ≤
10` is a generous validity bound, not evidence of more than four stages — 4
agrees with `GL_MAX_TEXTURE_UNITS_ARB` and with D3D `MaxTextureBlendStages`
[CONFIRMED].

Corrections this makes to `docs/parhelia-gl-feature-map.md` §2:

* the light/material element is **0x70 = 28 dwords**, not 30;
* the `(0x08,1)` payload is 0x20 bytes = 8 dwords, and the "mask" dword at
  payload `+0x18` is the literal constant `0xffffffff` (`libGL64 0xf340a`);
* `(0x08, N)` has a *second* shape: `libGL64 0xc4d40` emits `count =
  [obj+0x170]` elements of 0x20 bytes copied from `obj+0x70 + i*0x20` — a real
  attribute-declaration array, not a single descriptor.

### 2.2 The record index is a render-state id, **not** an MMIO register index

This is the most consequential correction in this document, and it is read
straight out of the kernel.

`docs/parhelia-gl-driver.md` and `docs/parhelia-gl-feature-map.md` §3 both model
the `(index, value)` records as `MMIO reg = 0x1000 + index*4`. The kernel's
handlers say otherwise [CONFIRMED]:

* **op 0x0c** → `mtx2617` (`0x19950`): if `index ≤ 0x100` it is shadowed at
  `ctx[0x10 + index*4]`, and if it changed, dispatched through a **172-entry
  jump table at `.rodata+0xba0`** (`index ≤ 0xab`). Each entry writes a
  *differently shaped* field — e.g. index 0x07 → `ctx+0xc9b` (dword) + dirty bit
  0x4 at `+0xc87`; index 0x08 → a 2-bit field inside byte `ctx+0x1be4` + dirty
  bit 0x80 at `+0x1bec`; index 0x09 → the top 2 bits of the same byte; index
  0x16 → a 2-bit field in `ctx+0x1be5`. Indices `0x101..0x200` are not states at
  all — they set/clear bits in an **enable bitmap** at `ctx+0x414`.
* **op 0x05** → `mtx1734` (`0x1abf0`): `index ≤ 0x1c`, shadow `ctx[0x3a0 +
  index*4]`, again a per-index jump table writing bitfields.
* **op 0x06** → `mtx3081` (`0x1ae50`): a 2-D `(group, sub)` array at
  `ctx+0xa74`, both ≤ 10 — D3D-style texture-stage state.
* **op 0x0e** → inline (`0x459dc`): `(group ≤ 10, sub, value)` into 130-byte
  per-stage structs at `ctx+0xdd7 + group*130`.

**Worked example — the depth group.** The live capture maps
`glEnable(GL_DEPTH_TEST)` to index 0x07, `glDepthMask` to 0x0e and
`glDepthFunc` to 0x17 in bank 0x0c. In `mtx2617`'s table those three land on
`0x19bbe`, `0x19c8d` and `0x19dc0` — and all three **set the same dirty bit 0x4
in `ctx+0xc87`** while writing three adjacent fields: `ctx+0xc9b` (dword,
enable), `ctx+0xc9f` (**byte**, write mask) and `ctx+0xca0` (dword, function)
[CONFIRMED]. The GL semantics the capture established are exactly right; what
they name is a driver state group, and the field widths (a byte for the mask, a
dword for the enum) are not the uniform 32-bit registers the
`0x1000 + index*4` model implies. Indices that need no immediate side effect —
0x00, 0x04, 0x0b and 0x1e–0x21 among them — dispatch to the shared no-op stub
at `0x199b0`, which only updates the shadow.

Nothing in any of these paths touches MMIO. The register programming is a
*second* pass in the kernel — `mtx3712` (`0x188f0`) walks the dirty words
(`+0xc87`, `+0xc08`, `+0xd54`, `+0x1b44`, `+0xdbb`, `+0x1bd8`, …) and calls
per-group updaters (`mtx698`, `mtx297`, …), and the resulting packets go out
over the DMA ring. The live mmiotrace in `docs/parhelia-mmio-capture.md`
independently shows **no** `0x1000+idx*4` writes at draw time — the geometry and
state travel in the ring buffer.

**The decisive argument.** A second, independent pass over the ICD produced a
complete index→GL-feature map for both small banks, and it collides with itself
under `0x1000 + index*4`:

* bank **0x05**: idx 0x09/0x0a/0x0b/0x0c = blend `srcA`/`srcRGB`/`dstA`/`dstRGB`
  (emitted at `libGL64 0x114c84`–`0x114dac`), idx **0x15/0x16/0x17** =
  polygon-offset factor / units / enable (`0x112570`), idx 0x08 = polygon mode,
  idx 0x01 = colour sum, idx **0x12** = vertex-shader mode (§5.3);
* the live capture pins **0x1058** to `glCullFace` and **0x105c** to
  `glDepthFunc`.

Apply `0x1000 + index*4` to bank 0x05 and polygon-offset units/enable land on
`0x1058`/`0x105c` — on top of cull face and depth func. Both banks use small
indices from overlapping ranges, so **at most one of them can be a register
bank, and neither is**: `mtx1734` (bank 0x05, index ≤ 0x1c) and `mtx2617` (bank
0x0c, index ≤ 0xab) each dispatch through their own jump table into differently
shaped shadow fields, and neither writes MMIO. The blend factors match
`0x1000+idx*4` because the *kernel* happens to program them there, not because
the stream carries register numbers.

The index→GL-feature map is still valuable in its own right. Bank 0x05: 0x01
colour sum, 0x08 polygon mode, 0x09–0x0c blend factors, 0x12 vertex-shader
mode, 0x15–0x17 polygon offset. Bank 0x0c: 0x07/0x0e/0x17 depth
enable/mask/func, 0x0c logic op, 0x18/0x19 alpha ref/func, 0x1b blend enable,
0x1c fog enable, 0x1d colour sum, 0x23 and 0x8c fog mode (depth- versus
coordinate-sourced), 0x34–0x3a stencil enable/ops/func/ref/mask, 0x9a/0x9b/0xa6
point size/min/max, 0xa8 colour write mask, 0xab blend equation. Treat these as
**state ids**, and get the register from the kernel's tables.

So the per-feature table in `docs/parhelia-gl-feature-map.md` §3 remains
valuable and correct as *"GL call → driver render-state id → value"* — the
GL-semantic mapping is real and hard-won — but the "= MMIO register 0x101c"
column is an unproven relabelling. There are **four** index namespaces (banks
0x05, 0x06, 0x0c, 0x0e), and the same numeric index means different things in
each. An open driver must supply the second-stage translation itself; the
kernel's jump tables are the specification for it.

---

## 3. The complete limit table

### 3.1 How the ICD answers `glGet`

`glGetString` `0x1aa2f0` tail-calls `0x120d40`:

| query | value | where |
|---|---|---|
| `GL_VENDOR` | `"Matrox Graphics Inc."` | `.rodata 0x36be5a` |
| `GL_RENDERER` | `"OpenGL Linux library for Parhelia"` | `.rodata 0x36be38` |
| `GL_VERSION` | `"1.3 "` + `"ICD v1.03"` | literal `"1.3 "` at `0x120dfe`; the suffix comes from `ICDLinkImpl::GetVersion()` at **`0x364ed0`** — `strlen`, then `movabs rdx, 0x302e317620444349` (`"ICD v1.0"`) at `0x364edc` and `mov WORD [end+8], 0x0033` (`'3'`, NUL) at `0x364ee6` |
| `GL_EXTENSIONS` | `mov rax,[rip+…] # 65c930; ret` at `0x120e2b` — one load. The global is initialised by a single `R_X86_64_RELATIVE` reloc to **`0x66e0e0`** (file 0x46e0e0), the 1189-byte / 51-token string, and is **written nowhere in `.text`** | §5.1 |

The version string is a hardcoded constant with no chip dependency
[CONFIRMED], and it matches the "1.3 ICD v1.03" ground truth exactly.

`glGetIntegerv` `0x1a9ee0` dispatches through the per-context table slot
`[ctx+0x688]`. There are four parallel implementations, one per type, all with
the same binary-search-over-enum shape:

| entry | address | common tail | invalid-enum |
|---|---|---|---|
| `glGetBooleanv` | `0x1340f0` | `0x13430c` | `0x134436` |
| `glGetDoublev` | `0x137410` | `0x137606` | |
| `glGetIntegerv` | **`0x13aa30`** | `0x13ac83` | **`0x13adb6`** |
| `glGetFloatv` | `0x13dfa0` | `0x13e171` | `0x13e2b6` |

The invalid-enum path sets `GL_INVALID_ENUM` (0x500) into `ctx+0xc` and returns
without writing the caller's buffer. Each of the four answers the **same 256 enums** (extracted by walking the
compare/branch ladder of each function; the two lists are identical).

### 3.2 Constant limits [all CONFIRMED — read as immediates]

| enum | name | int | float |
|---|---|---|---|
| 0x0D33 | `GL_MAX_TEXTURE_SIZE` | **2048** (`0x13df0b`) | 2048.0 (`0x141110`) |
| 0x8073 | `GL_MAX_3D_TEXTURE_SIZE` | **2048** (`0x13bced`) | 2048.0 |
| 0x851C | `GL_MAX_CUBE_MAP_TEXTURE_SIZE` | **2048** (`0x13c546`) | 2048.0 |
| 0x84E2 | `GL_MAX_TEXTURE_UNITS_ARB` | **4** (`0x13c67b`) | 4.0 |
| 0x84FF | `GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT` | **2** (`0x13d653`) | 2.0 (`0x140aa9`) |
| 0x84FD | `GL_MAX_TEXTURE_LOD_BIAS_EXT` | **2** (`0x13dab8`) | 2.0 |
| 0x0D3A | `GL_MAX_VIEWPORT_DIMS` | **4096, 4096** (`0x13ac73`) | |
| 0x0D32 | `GL_MAX_CLIP_PLANES` | **6** (`0x13d48f`) | |
| 0x0D30 | `GL_MAX_EVAL_ORDER` | 8 | |
| 0x0B31 | `GL_MAX_LIST_NESTING` | 64 | |
| 0x0D34 | `GL_MAX_PIXEL_MAP_TABLE` | 32 | |
| 0x0D35 | `GL_MAX_ATTRIB_STACK_DEPTH` | 16 | |
| 0x0D3B | `GL_MAX_CLIENT_ATTRIB_STACK_DEPTH` | 16 | |
| 0x0D37 | `GL_MAX_NAME_STACK_DEPTH` | 64 | |
| 0x0D38 | `GL_MAX_PROJECTION_STACK_DEPTH` | 32 | |
| 0x0D39 | `GL_MAX_TEXTURE_STACK_DEPTH` | 32 | |
| 0x0D50 | `GL_SUBPIXEL_BITS` | **4** | |
| 0x80E8 | `GL_MAX_ELEMENTS_VERTICES` | **100000** (0x186a0) | |
| 0x80E9 | `GL_MAX_ELEMENTS_INDICES` | **100000** | |
| 0x0B12 | `GL_POINT_SIZE_RANGE` | 0, 10 | **0.25, 10.0** |
| 0x0B13 | `GL_POINT_SIZE_GRANULARITY` | 0 | **0.25** |
| 0x0B22 | `GL_LINE_WIDTH_RANGE` | 0, 10 | **0.25, 10.0** |
| 0x0B23 | `GL_LINE_WIDTH_GRANULARITY` | 0 | **0.25** |
| 0x846D | `GL_ALIASED_POINT_SIZE_RANGE` | **1, 2048** | 1.0, 2048.0 |
| 0x846E | `GL_ALIASED_LINE_WIDTH_RANGE` | **1, 2048** | 1.0, 2048.0 |
| 0x86A2 | `GL_NUM_COMPRESSED_TEXTURE_FORMATS` | **4** | |
| 0x86A3 | `GL_COMPRESSED_TEXTURE_FORMATS` | **0x83F0, 0x83F1, 0x83F2, 0x83F3** (DXT1-RGB, DXT1-RGBA, DXT3, DXT5) | |
| 0x0C30/0x0C31/0x0C33 | `GL_INDEX_MODE` / `GL_RGBA_MODE` / `GL_STEREO` | 0 / 1 / 0 | |

Dynamic (visual- or context-dependent, *not* a constant to copy):
`GL_RED/GREEN/BLUE/ALPHA/DEPTH/STENCIL/ACCUM_*_BITS` and `GL_AUX_BUFFERS` are
each a virtual call on the drawable object at `[0x65cf18]` (e.g.
`GL_DEPTH_BITS` → vtable `+0x28` at `0x13c486`, `GL_BLUE_BITS` → `+0x50` at
`0x13d9b1`, `GL_ALPHA_BITS` → `+0x58` at `0x13db24`);
`GL_SAMPLE_BUFFERS` / `GL_SAMPLES` are read from `ctx+0x7c74` / `ctx+0x7c78`
(`0x13cb1f` / `0x13d93d`) — but see §5.4: those two words are written **once**,
at construction (`0x1c7380`), to 0 and 16, and never again. They are not
dynamic and they are not consistent with each other.

### 3.3 `GL_EXT_vertex_shader` limits — answered, but the extension is not advertised

| enum | name | value | address |
|---|---|---|---|
| 0x87C5 | `GL_MAX_VERTEX_SHADER_INSTRUCTIONS_EXT` | **512** | `0x13b7a9` |
| 0x87C6 | `GL_MAX_VERTEX_SHADER_VARIANTS_EXT` | 16 | |
| 0x87C7 | `GL_MAX_VERTEX_SHADER_INVARIANTS_EXT` | 256 | |
| 0x87C8 | `GL_MAX_VERTEX_SHADER_LOCAL_CONSTANTS_EXT` | 256 | |
| 0x87C9 | `GL_MAX_VERTEX_SHADER_LOCALS_EXT` | 12 | |
| 0x87CB | `GL_MAX_OPTIMIZED_VERTEX_SHADER_VARIANTS_EXT` | 16 | |
| 0x87CC | `GL_MAX_OPTIMIZED_VERTEX_SHADER_LOCAL_CONSTANTS_EXT` | 256 | |
| 0x87CD | `GL_MAX_OPTIMIZED_VERTEX_SHADER_INVARIANTS_EXT` | 256 | |
| 0x87CE | `GL_MAX_OPTIMIZED_VERTEX_SHADER_LOCALS_EXT` | 12 | |

Addresses for the rest: 0x87C6 `0x13d5f0`, 0x87C7 `0x13db5c`, 0x87C8
`0x13d0df`, 0x87C9 `0x13c688`. **`0x87CA`
(`GL_MAX_OPTIMIZED_VERTEX_SHADER_INSTRUCTIONS_EXT`) and `0x87CF` are not
answered at all** — the same omission style as §3.5.

The ICD also answers **live per-shader counters** out of the vertex-shader
function object (`[ctx+0x7b78]`): `GL_VERTEX_SHADER_BINDING_EXT` 0x8781
(`0x13bbaa`), `…VARIANTS` 0x87D0 = `+0x0c` (`0x13d803`), `…INVARIANTS` 0x87D1 =
`+0x10` (`0x13dead`), `…LOCAL_CONSTANTS` 0x87D2 = `+0x14` (`0x13b1f6`),
`…LOCALS` 0x87D3 = `+0x18` (`0x13c5e2`), `…OPTIMIZED` 0x87D4 = byte `+0x05`
(`0x13bec9`). A driver does not report live counts for a feature it did not
implement — see §5.3.

### 3.4 `GL_MTX_fragment_shader` limits — **named from the vendor's own header**

`opengl6/include/glext.h` lines 1449–1462 give the names outright, so nothing
here is guessed. Values read from `glGetIntegerv`; enforcement sites from the
shader compiler.

| enum | vendor name | meaning | value | glGet | enforced at |
|---|---|---|---|---|---|
| 0x8A80 | `GL_FRAGMENT_SHADER_MTX` | enable | — | `glIsEnabled` `0x127717` | `glEnable` `0x125592` writes **1** to `ctx+0x7c30`; pipeline selector tests it at `0x1bef93` |
| 0x8A81 | `GL_FRAGMENT_SHADER_BINDING_MTX` | currently bound shader — **live** | `[[ctx+0x7c28]+4]` | `0x13d4c8` | shader ctor `0x1b59c0`, bind `0x1b8220` |
| 0x8A82 | `GL_MAX_TA_OPS_MTX` | max texture-address ops | **4** | `0x13db9d` | the 4-entry TA descriptor array (fragment-ISA §9.5) |
| 0x8A83 | `GL_OPT_TA_OPS_MTX` | optimal TA ops | **4** | `0x13d116` | |
| 0x8A84 | `GL_TA_OPS_MTX` | TA ops used by the bound shader | **4** *(constant)* | `0x13bbc9` | — |
| 0x8A85 | `GL_MAX_FP_OPS_MTX` | **max colour/RGBA instructions** | **10** | `0x13d0ec` | `cmp [shader+0xdc],9; jbe` at **`0x1b6126`** → "Too many color instructions" (`0x36e390`, raised `0x1b615c`) |
| 0x8A86 | `GL_MAX_FP_TEXTURES_MTX` | max textures | **4** | `0x13d4bb` | in-use bitmask `[shader+0x10]` at `0x1b2650`–`0x1b2690` |
| 0x8A87 | `GL_MAX_FP_COLORS_MTX` | max iterated colours (`COLOR0`/`COLOR1`) | **2** | `0x13dac5` | header: `GL_COLOR0_MTX` 0x0B00, `GL_COLOR1_MTX` 0x8459 |
| 0x8A88 | `GL_OPT_FP_OPS_MTX` | optimal instruction count | **5** | `0x13b856` | the "pad to 5 or 10" upload rule (fragment-ISA §1.1/§9.3) |
| 0x8A89 | `GL_OPT_FP_TEXTURES_MTX` | | **4** | `0x13c6a5` | |
| 0x8A8A | `GL_OPT_FP_COLORS_MTX` | | **2** | `0x13d051` | |
| 0x8A8B | `GL_FP_OPS_MTX` | ops used by the bound shader | **5** *(constant)* | `0x13d60d` | — |
| 0x8A8C | `GL_FP_TEXTURES_MTX` | textures used | **4** *(constant)* | `0x13dbb9` | — |
| 0x8A8D | `GL_FP_COLORS_MTX` | colours used | **2** *(constant)* | `0x13c2a6` | — |

Two things to carry forward. First, **`GL_MAX_FP_OPS_MTX` = 10 colour
instructions with an "optimal" count of 5** — this is the real fragment-program
budget and it confirms, from a second direction, the 5/10 slot structure in
`docs/parhelia-fragment-isa.md` §1.1. Second, the four "used by the current
shader" queries (0x8A84, 0x8A8B, 0x8A8C, 0x8A8D) return **hard-coded
constants**, unlike their `GL_EXT_vertex_shader` counterparts (§3.3), so they
cannot be used to introspect a compiled program.

`glext.h` also names the rest of the ISA the way the hardware sees it — texture
opcodes `GL_OP_TEX*_MTX` 0x8A8E–0x8A9D, ALU opcodes
`GL_OP_{MOV,ADD,SUB,MUL,DP3,DP4,MAD,LRP,CND,CMV,CMP}_MTX` 0x8AA0–0x8AAA,
registers `GL_TEMPORARY0/1_MTX` 0x8AB0/0x8AB1 and `GL_OUTPUT_COLOR_MTX` 0x8AB3,
result modifiers `SATURATE` / `SCALE_X2/X4/X8/D2/D4`, argument modifiers
`INVERT/NEGATE/BIAS/BIAS_NEGATE/SIGNED/SIGNED_NEGATE`, texture formats
`TEX_UNSIGNED/SIGNED/BIASED` and the sampling hints
`GL_HINT_{NEAREST,BILINEAR,TRILINEAR,ANISOTROPIC,TEXTURE_1D,2D,3D,CUBE,BEML,BEM}_MTX`.
That is the whole opcode/modifier vocabulary of
`docs/parhelia-fragment-isa.md`, with vendor names.

### 3.5 Limits the ICD does **not** answer

* **`GL_MAX_LIGHTS` (0x0D31) is not implemented.** The 32-bit immediate
  `0x00000d31` does not occur anywhere in the binary — a byte search of the
  whole `.so` finds zero matches, while `0x0d33` (`GL_MAX_TEXTURE_SIZE`) occurs
  at all four `glGet*` sites. Querying it yields `GL_INVALID_ENUM` and an
  untouched result buffer [CONFIRMED].
  The real limit is **12**: `glEnable(GL_LIGHT0+i)` accepts `i ≤ 0x0b`
  (`0x1252c2`), sets the per-light enable flag at `state+0x348 + i*0x78 + 0xc4`
  (`0x1253df`) and OR's bit `6+i` into the light-enable word at `(state+0x348)+0x66c`;
  `i ≥ 12` raises `GL_INVALID_ENUM` (`0x1252d7`). The Windows D3D driver
  independently declares `MaxActiveLights = 12` (§4). **This is the cleanest
  example of the "under-declared resources read as hardware limits" trap in the
  whole ICD — an app that trusts `glGet` learns nothing, and an app that assumes
  the GL 1.3 minimum uses 8 of the 12 the part has.**
* **`GL_MAX_MODELVIEW_STACK_DEPTH` (0x0D36) is not implemented either.** Here
  there is a defensible reason: all three matrix stacks are dynamically grown
  containers (`glPushMatrix` at `0x155cf0` calls a reserve helper `0x1b19a0`
  per mode and only raises `GL_STACK_OVERFLOW` when the container refuses to
  grow), so there is no constant to report. That makes the *reported* 32 for the
  projection and texture stacks a declaration rather than a hardware limit
  [CONFIRMED].

### 3.6 Limits that are actually **enforced**, not merely declared

Worth separating, because they constrain a replacement driver's conformance
choices rather than describing the silicon:

* **Power-of-two textures are mandatory.** `glTexImage2D`/`3D` validate
  `dim ≤ 2048` *and* `(dim-1) & dim == 0` for every dimension, else
  `GL_INVALID_VALUE` — 2D at `0x14a28d`–`0x14a2b8`, 3D at `0x14b078`–`0x14b0bc`
  [CONFIRMED]. See §4 for why this is a **software** restriction.
* **Anisotropy is clamped, not just declared.**
  `glTexParameter(GL_TEXTURE_MAX_ANISOTROPY_EXT, n)` rejects `n < 1.0`
  (`0x131fab`) and then applies `min(n, 2.0f)` — the literal `0x40000000` at
  `0x132062` [CONFIRMED].

---

## 4. The Direct3D declaration for the same silicon, and the gaps

From `MTXPARD.dll`. The caps are two **static `.data` templates** `rep movsd`'d
by `BuildDeviceCaps` (`0x875f0`) — D3DDEVICEDESC from `0x1d1e78`,
D3DHAL_D3DEXTENDEDCAPS from `0x1d1e00` — then reshaped into `D3DCAPS8` by
`BuildD3DCAPS8` (`0x869f0`) and `D3DCAPS9` by `BuildD3DCAPS9` (`0x86ca0`).
There is **no device-ID branch in the caps path at all**; everything variable is
a registry option or one chip-class bit test (§6).

### 4.1 Where the two declarations agree [CONFIRMED both sides]

| quantity | GL ICD | D3D |
|---|---|---|
| max 2D texture | 2048 | `MaxTextureWidth/Height` 2048 (`.data 0x1d1e08/0x1d1e10`) |
| max volume texture | 2048 | `MaxVolumeExtent` 2048 (`0x86bc4`) |
| max anisotropy | 2 | `MaxAnisotropy` 2 (`0x1d1e2c`) |
| texture units | `GL_MAX_TEXTURE_UNITS_ARB` 4 | `MaxTextureBlendStages` 4, `MaxSimultaneousTextures` 4 (`0x1d1e50`) |
| user clip planes | `GL_MAX_CLIP_PLANES` 6 | `MaxUserClipPlanes` 6 (`0x1d1e5c`) |
| lights | *(not answered)* — real limit 12 | `MaxActiveLights` **12** (`0x1d1e54`) |
| S3TC | 4 GL formats (DXT1/DXT1a/DXT3/DXT5) | DXT1–DXT5, all five, as 2D **and** cube **and** volume |
| blend factors | full enum ZERO…1−CONSTANT_ALPHA | `SrcBlendCaps` = `DestBlendCaps` = 0x1FFF — every factor **up to `BOTHINVSRCALPHA`**; `BLENDFACTOR` (0x2000) and `INVBLENDFACTOR` (0x4000) are **clear** |
| depth/alpha compare | 8 functions each | `ZCmpCaps` = `AlphaCmpCaps` = 0xFF |
| stencil ops | KEEP…DECR incl. wrap | `StencilCaps` = 0xFF |

The agreement on 2048 / 4 units / anisotropy 2 is the useful negative result:
those really are hardware limits, not conservative declarations.

### 4.2 Where they disagree — candidate under-declarations

| # | GL says | D3D says | assessment |
|---|---|---|---|
| 1 | **power-of-two only** (`GL_INVALID_VALUE` otherwise, §3.6) | `TextureCaps` = 0x1EC4D with **`D3DPTEXTURECAPS_POW2` clear**, and `CUBEMAP_POW2` / `VOLUMEMAP_POW2` / `SQUAREONLY` also clear (`.data 0x1d1efc`) | **The hardware does non-power-of-two textures — 2D, cube and volume, mipmapped.** The GL restriction is the ICD being a GL 1.3 driver (where POT is required by spec) and never adding `GL_ARB_texture_non_power_of_two` or `GL_ARB_texture_rectangle`. An open driver can expose NPOT. **Biggest single win in this comparison.** |
| 2 | no vertex-program extension advertised, but the ICD answers 512-instruction `GL_EXT_vertex_shader` limits (§3.3) | `VertexShaderVersion` = **`0xFFFE0101` = vs_1_1**, `MaxVertexShaderConst` = **256**, `D3DDEVCAPS_HWTRANSFORMANDLIGHT` and `D3DDEVCAPS_PUREDEVICE` set (`0x86c7a`, `0x87650`, `0x87678`) | **The part has a programmable vertex unit and hardware T&L; the Linux ICD implements it in full and then fences it off.** See §5.3 — resolved. |
| 3 | `GL_MTX_fragment_shader` only | `PixelShaderVersion` = **`0xFFFF0103` = ps_1_3** (`0x86c8a`), `PixelShader1xMaxValue` 1.0f | The MTX extension is the GL door onto a ps_1_3-class combiner; the limits in §3.4 should be read against `docs/parhelia-fragment-isa.md`. |
| 4 | `GL_ARB_texture_env_combine` / `dot3` | `TextureOpCaps` = 0x03FEFFFF: additionally **BUMPENVMAP, BUMPENVMAPLUMINANCE, MULTIPLYADD, LERP** (`.data 0x1d1e4c`) | Bump mapping and LRP/MAD are hardware but have no fixed-function GL path. Reachable through the fragment ISA (`MAD`, `LRP` are in the opcode table already). |
| 5 | `GL_POINT_SIZE_RANGE` / `GL_LINE_WIDTH_RANGE` 0.25–**10.0** | `MaxPointSize` = **2048.0f** (`0x86bda`) | The 10.0 is a *software* rasteriser limit, consistent with the capture's CPU-expanded wide-line path. `GL_ALIASED_*_RANGE` already reports 1–2048. |
| 6 | `GL_MAX_VIEWPORT_DIMS` 4096×4096 | guard band **±8160.0f** (`.data 0x1d1e30`–`0x1d1e3c`, also inline in the clipper at `0xd6cf6`) | The clip window is nearly ±8K; 4096 is the ICD's own choice. |
| 7 | `GL_MAX_ELEMENTS_VERTICES/INDICES` 100000 | `MaxPrimitiveCount` = `MaxVertexIndex` = **0x7FFFFFFF**, `MaxStreamStride` 65535, `MaxStreams` 8 | GL's are only hints, but the gap is real. |
| 8 | 4 texture units | `FVFCaps` TEXCOORDCOUNTMASK = **8** texcoord sets (`0x1d1e48` + `or 0x100000` at `0x86b94`), `MaxVertexBlendMatrices` 4, `MaxVertexBlendMatrixIndex` 31 | 8 texcoord *sets* can be delivered to 4 sampling stages; also 4-matrix vertex blending exists with no GL exposure (no `GL_ARB_vertex_blend`). |
| 9 | fog listed as CPU-side in `docs/parhelia-gl-driver.md` | `RasterCaps` 0x07F309A7 sets **FOGVERTEX, FOGTABLE, FOGRANGE, WFOG, ZFOG**, plus **SCISSORTEST, DEPTHBIAS, SLOPESCALEDEPTHBIAS, ANISOTROPY** (`0x1d1ee4`) | Fog, scissor and polygon offset are hardware. The live capture already shows fog enable and fog mode as emitted state, so the old "software fog / software polygon offset" note is wrong. |
| 10 | `GL_ARB_multisample` advertised but **a pure no-op** — the block is written once at construction and never again (§5.4) | multisample word rewritten per format at `0x86f90` from the `AAMode` registry setting: `0x0009`, `0x8001` or 0 | FAA exists and Windows drives it; the Linux GL stack advertises the extension and does nothing. A **real** under-declaration, and worse — a mis-declaration. |
| 11 | `GL_EXT_blend_color` is **hardware** — `glBlendColorEXT` emits a dedicated opcode-0x35 four-float record at `0x11506b` (§5.4) | `SrcBlendCaps`/`DestBlendCaps` = 0x1FFF, i.e. **`D3DPBLENDCAPS_BLENDFACTOR` is not set** | Gap runs the **other way**: the D3D driver under-declares. Resolved in GL's favour. |
| 12 | `GL_EXT_blend_func_separate` is **hardware** — `0x124820` validates four factors and the flush at `0x114c84`–`0x114dac` emits four separate records | `PrimitiveMiscCaps` = 0x00102CF3, **`SEPARATEALPHABLEND` (0x20000) clear** | Gap runs the **other way** again: four independent hardware factor slots exist and the D3D driver never declared them. Resolved in GL's favour. |
| 13 | advertises `GL_EXT_texture_lod_bias`, `GL_MAX_TEXTURE_LOD_BIAS_EXT` = 2, and really emits it (opcode 0x0e sub 0x13, `0x10ff14`) | `D3DPRASTERCAPS_MIPMAPLODBIAS` only when the `MipmapLodBias` registry value is non-zero (`0x8785d`), i.e. off by default | Hardware, but the ICD's emit indexes the per-unit array with stride `0x37d8` where the setter uses `0xb28` — **units 1–3 send garbage** (§5.4). A vendor bug, not a limit. |

Two more D3D scalars with no GL counterpart, worth having: **`MaxTextureRepeat`
= 255** (`.data 0x1d1e24`) — the texture-coordinate wrap range, which bounds how
far a `GL_REPEAT` coordinate may run before the addresser loses precision — and
**`MaxVertexW` = 0.0f** (`.data 0x1d1e58`), i.e. the driver declares no
w-range limit. `MaxTextureAspectRatio` is 2048, so 1×2048 textures are legal.

Two D3D declarations worth recording for a modeset/2D driver as well:
`dwDeviceRenderBitDepth` = 0x500 (16 and 32 bpp only) and
`dwDeviceZBufferBitDepth` = 0x400 (16 bpp) in the legacy DX7 desc, versus the
D3D8/9 depth format list D16 / D15S1 / D24X4S4 / D24S8 (`.data 0x1d14d8`).

---

## 5. Extensions: hardware, software, or absent

### 5.1 The extension string is used verbatim

**No filtering, no editing, no second master list [CONFIRMED].** `GL_EXTENSIONS`
is one load (`0x120e2b`) of a `.data.rel.ro` pointer that a single
`R_X86_64_RELATIVE` relocation initialises to `0x66e0e0`, and that pointer is
written nowhere in `.text`. The string body has zero code references — no
`strcpy`/`strcat`/`sprintf` reaches it. The 32-bit build carries a byte-identical
token set. Nor could a config file or environment variable change it: the ICD
imports **no `getenv`, no `fopen`/`access`/`dlopen`** (only `open`, `close`,
`ioctl`, `mmap64`, `munmap`, `getpagesize`), and its only path string is
`/dev/matrox`, with the minor number handed over by the X server in a GLX reply.
The only other extension list in the binary is the GLX one at file offset
0x3a11b0, and there are no per-extension name strings anywhere, so token-level
filtering is structurally impossible.

So the list is the vendor's final word, and everything missing from it is
missing on purpose — see §5.3.

### 5.2 What actually selects the software pipeline

The ICD builds **three** complete pipelines in the context constructor and picks
one per draw. `0x1bed70` computes a 32-bit "reasons for software" word into the
selector object at `ctx+0xbbe0`, and `0x1bf150` consumes it [CONFIRMED]:

```
if (reasons & 0x7fffffff) {
        if (bit31) install nothing;                  /* VS + software = give up */
        else       install SendState[sel+0x30], Primitive[sel+0x38];  /* SW TnL */
} else if (bit31) install SendState[sel+0x40], Primitive[sel+0x48];   /* HW TnL + VS */
else              install SendState[sel+0x20], Primitive[sel+0x28];   /* HW TnL */
```

| bit | reason | test |
|---|---|---|
| 0 | lines need software | `caps[0x1c] == 0` (`0x1bee63`) or polygon mode `GL_LINE` |
| 1 | **wide lines** | `caps[0x1d] == 0` (`0x1bedf1`) or width > 1.0f |
| 3 | **no hardware T&L** | `caps[0x00] == 0` (`0x1bf020`) |
| 4 | render mode ≠ `GL_RENDER` (selection / feedback) | `[ctx+0x78a8] != 0x1c00` |
| 5 | polygon mode point/line unsupported | `caps[0x02] == 0` (`0x1bf0b5`) |
| 8 | line smooth unsupported | `caps[0x1e] == 0` (`0x1bf064`) |
| 31 | vertex shader enabled | `[ctx+0x7b98] != 0` (`0x1bf031`) |

`caps[]` is the 40-byte capability blob the **kernel** returns (§6.3). On this
stack `caps[0x1c]` and `caps[0x1d]` are hard-coded **0**, which is precisely why
the hardware-confirmed wide-line path in `docs/parhelia-gl-feature-map.md` §3
runs on the CPU. It is a kernel policy decision, not a hardware limit.

### 5.3 `GL_EXT_vertex_shader` is fully implemented and switched off by one store

This is the biggest result in the document.

**The implementation is real, not stubs** [CONFIRMED]:

* Every `glShaderOp*EXT` / `glBeginVertexShaderEXT` / … slot in
  `GLStateManager`'s vtable (slots 0x520–0x578) holds a real function
  (`0x127cd0`, `0x127d10`, `0x127de0`, `0x127e20`, …). The
  `GL_INVALID_OPERATION` stubs (the 311 copies of
  `if(!err){err=1; code=0x502;}` in 0x11e090–0x122cf0) appear only in
  `GLStateInsideBegin`, `GLStateInvalid` and `GLFragmentShaderManager`, which is
  correct GL behaviour — **none is in `GLStateManager`**.
* A dedicated recording state class `GLStateInsideVSBegin` (vptr `0x6683b0`)
  exists with its own End/ShaderOp/Swizzle/WriteMask/Insert/Extract
  implementations (`0x120660`, `0x1205a0`…`0x120600`), and
  `glBeginVertexShaderEXT` (`0x127cd0`) really transitions into it via
  `pthread_setspecific`.
* `glShaderOp3EXT` → `0x161220` is a compiler: it jump-tables the destination on
  `res − 0x879A` over 0x24 entries (table `0x36cc2c`, the `GL_OUTPUT_*` block)
  and otherwise appends an instruction record to a `std::deque` at `[fn+0x80]`.
* There are **30 `GLVSInstruction_*` classes**, one per `GL_OP_*`
  (Add, Clamp, Cross, Dot3, Dot4, Exp2, Floor, Frac, Index, Log2, Madd,
  MatrixMultiply, Max, Min, Mov, Mul, Negate, Power, Recip, RecipSqrt, Round,
  SetGE, SetLT, Sub, Swizzle, WriteMask, ExtractComponent, InsertComponent …)
  and roughly **50 binding-descriptor classes** for `glBindParameterEXT`
  (`GLVSMVPMatrixDesc`, `GLVSLightPositionDesc`, `GLVSMaterialShininessDesc`,
  `GLVSTexGenEyePlaneDesc`, `GLVSFogDensityDesc`, `GLVSLocalConstantDesc`,
  `GLVSVariantDesc`, `GLVSInvariantDesc`, …).
* `glEndVertexShaderEXT` (`0x120660` → `0x180d30`) runs the compiler
  (`0x15fec0`), checks a success flag and raises `GL_INVALID_OPERATION` on
  failure, then runs an emitter (`0x180b10`).
* The front end lowers into the **D3D form** — `GLVertexShaderFunction` owns a
  `GLVertexShaderFunctionDX` at `+0x28`, reset from Begin (`0x16d630`) and
  finalised from End (`0x16ef40`). The GL and Windows vertex-shader paths share
  one compiler back end.
* The vendor's own `glext.h` declares the whole `GL_EXT_vertex_shader` token
  block and prototypes.

**The hardware pipeline behind it exists and is constructed unconditionally**
[CONFIRMED] — the GL context constructor builds all three pairs:

| pipeline | primitive object | send-state object |
|---|---|---|
| HW T&L | `GLPrimitiveHardware`, ctor `0xc25a0`, at `ctx+0x8e78` | `GLSendStateHardwareTnL`, ctor `0x115d80`, at `ctx+0x8e80` |
| SW T&L | `GLPrimitiveSoftware`, ctor `0xf0320`, at `ctx+0x90e0` | `GLSendStateSoftwareTnL`, ctor `0x11ac60`, at `ctx+0x90e8` |
| **HW T&L + VS** | **`GLPrimitiveHardwareVS`, ctor `0xe2100`, at `ctx+0x9350`** | **`GLSendStateHardwareTnLVS`, ctor `0x1181e0`, at `ctx+0x9358`** |

Those are exactly the selector's `[sel+0x20/0x28]`, `[sel+0x30/0x38]` and
`[sel+0x40/0x48]`. `GLPrimitiveHardwareVS` overrides most of the base vtable
(`0xe0110`–`0xe36a0`) and three overrides emit GPU commands:

* **`0xe0110`** — `reserve(op=0x05, 8, 1)` then payload **(index `0x12`, value
  `1`)**. The non-VS pipelines write **`0x12 = 0`** at the same point
  (`GLPrimitiveHardware` `0xc397c`, `GLPrimitiveSoftware` `0xf4613`), cached at
  `state+0x948`. **Bank-0x05 index 0x12 is the WARP vertex-shader mode enable** —
  a concrete new register fact for an open driver.
* `0xe01d0` and `0xe0450` — `reserve(op=0x50, …)`, the VS geometry-submission
  record.
* The VS manager also allocates GPU-side shader objects:
  `reserve(op=0x42,8,1)` + `reserve(op=0x41,8,1)` with handle `2*id+1`
  (`0x17c800`, loop `0x17c930`).

**And it is disabled by one store** [CONFIRMED]:

```
glEnable  impl 0x124fd0, case GL_VERTEX_SHADER_EXT (0x8780) @0x1255dc:
        lea rax,[rdi+0x7a60]
        mov BYTE PTR [rax+0x138], 0x00      <-- writes ZERO
        mov BYTE PTR [rax+0x139], 0x01
glDisable impl 0x125780, same case @0x125da5:  byte-identical, also 0x00
```

`ctx+0x7b98` (= vertex-shader manager `+0x138`) is exactly what `glIsEnabled`
reads (`0x127754`) and exactly what the selector tests for bit 31 (`0x1bf031`).
A whole-binary search finds **four** writers of that byte — `0x1255e8`,
`0x125db1`, `0x1801ff`, `0x1804cf` — **all storing 0**. Compare the symmetric,
correct handling of `GL_FRAGMENT_SHADER_MTX`: `glEnable` `0x125592` writes 1,
`glDisable` `0x125bac` writes 0.

So `GL_VERTEX_SHADER_EXT` can never be enabled, `GLPrimitiveHardwareVS` is never
selected, and advertising the extension would have been a lie — which is why it
is absent from an otherwise generous extension string. The vertex unit is not
missing; it is **fenced off**. Two further signs the combination was unfinished:
the selector deliberately installs *no* primitive (`0x1bf184`) if a shader is
active while any software reason is set, and `0x87CA` is the one vertex-shader
limit the ICD forgot to answer.

For an open driver this is the good news: the fence is one byte, and the code
behind it names the submission record (opcode 0x50) and the mode register
(bank 0x05 index 0x12 = 1). **Undetermined:** whether setting that register is
by itself sufficient to arm the WARP. One experiment settles it — patch the
`glEnable` site `0x1255e8` from `…,0x00` to `…,0x01` on the 0x0528 board under
the vendor stack and capture with the mmiotrace method of
`docs/parhelia-mmio-capture.md`.

### 5.4 Classification

Evidence codes: **[D3D]** the Windows driver declares it for this silicon (§4);
**[ICD]** the code path reaches `reserve()`/`commit`; **[KEY]** the pipeline-key
decode shows a real hardware attribute (§1); **[RTTI]** a `GLPrimitiveSoftware*`
class does the work on the CPU.

**Hardware**

| extension | evidence |
|---|---|
| `GL_ARB_multitexture`, `GL_SGIS_multitexture` | [ICD] `glActiveTexture` → `0x146a80`, hard bound `cmp eax,3; ja INVALID_ENUM` at **`0x146aa7`** → exactly 4 units; every hardware loop runs `unit = 0..3`; [D3D] `MaxSimultaneousTextures` 4 |
| `GL_ARB/EXT_texture_env_combine` | [ICD] `glTexEnvi` → `0x12e750` packs COMBINE/SOURCE/OPERAND/SCALE into the per-unit struct (stride 0xb28); lowered at `0x10e077` (jump table `0x36bac4`, env mode 6 = COMBINE → `0x10dcc0`) into 0x94-byte combiner records and uploaded verbatim as opcode 0x46 at `0x110250`. **No CPU texenv loop exists.** |
| `GL_ARB/EXT_texture_env_dot3` | [ICD] `GL_DOT3_RGB` → code 7 (`0x12f188`), `GL_DOT3_RGBA` → code 8 (`0x12f165`), through the same combiner emitter; [D3D] `TextureOpCaps` DOTPRODUCT3 |
| `GL_ARB/EXT_texture_env_add` | [D3D] `TextureOpCaps` ADD |
| `GL_ARB/EXT_texture_cube_map` | [ICD] cube = unit-enable byte +3, priority CUBE > 3D > 2D > 1D (`0x10f49d`); target 0x8513 and faces 0x8515–0x851A all reach `0x110043` and join the same sampler emit as 2D at `0x10f821`; 6-face validation `0x15422d` |
| `GL_EXT_texture3D` | [ICD] `glTexImage3D` → `0x14af50`; 3D = unit-enable byte +2, same sampler path (`0x10f810`/`0x10e166`); **no slice loop**; the hardware has a real W wrap field (`0x11c0[7:6]`) |
| `GL_ARB_texture_compression`, `GL_EXT_texture_compression_s3tc`, `GL_S3_s3tc` | [ICD] `glCompressedTexImage2D` → `0x1473f0`; block data is **`memcpy`'d straight into level storage** (`0x1478d3`) — no decompression on the normal path; DXT has first-class internal-format ids (`0x1cc300`). A CPU DXT *encoder* exists (`0x109830`) for apps that pass a compressed internalformat to `glTexImage2D`, which only makes sense if the GPU samples DXT natively. The CPU *decoder* (`0x108320`) is used only for readback and the DXT1 punch-through fix-up |
| `GL_EXT_texture_filter_anisotropic` | [ICD] clamped to 2.0f (`0x132062`); reaches hardware at `0x10f90e` — aniso > 1 forces per-unit subs 0x10/0x11 = 3 (filter enum "anisotropic"), ratio as sub 9 (`0x10fa53`) |
| `GL_ARB_texture_border_clamp`, `GL_EXT_texture_edge_clamp` | [ICD] `CLAMP_TO_BORDER` → wrap code 4 (`0x1320bf`); border colour as opcode-0x0e sub 0x0f (`0x10f834`, regs 0x11cc/0x11d0); [D3D] `TextureAddressCaps` 0x3F |
| `GL_SGIS_texture_lod` | [ICD] MIN/MAX_LOD, BASE/MAX_LEVEL (`0x131bc6`…) → per-unit opcode-0x06 subs 6/7/8 with GL-correct defaults 1000/−1000/1000 (`0x10b455`) |
| `GL_EXT_texture_lod_bias` | [ICD] parsed `0x12e75b`, emitted as opcode 0x0e `(unit, sub 0x13, float)` at `0x10ff14`. ⚠ **vendor bug**: that emit indexes with stride `0x37d8` while the setter uses `0xb28`, so units 1–3 read garbage. Do not replicate |
| `GL_EXT_blend_func_separate` | [ICD] `0x124820` validates four factors; flush `0x114c84`–`0x114dac` emits **four** opcode-0x05 records (idx 0x09/0x0a/0x0b/0x0c). Genuinely four hardware factor slots |
| `GL_EXT_blend_minmax`, `GL_EXT_blend_subtract` | [ICD] validator `0x11d700` accepts `mode−0x8006 ∈ {0,1,2,4,5}`; flush `0x114dac` emits opcode-0x0c **slot 0xab** = ADD 1 / SUB 2 / REV_SUB 3 / MIN 4 / MAX 5 |
| `GL_EXT_blend_color` | [ICD] `0x128bc0` clamps four floats to `ctx+0x1058`; flush `0x11506b` emits a **dedicated opcode-0x35, 0x10-byte record**. (D3D does not declare `BLENDFACTOR` — see §4.2 #11: the gap runs the other way) |
| `GL_EXT_blend_logic_op` | [ICD] `glLogicOp` → `0x11d730` (0x1500–0x150F → 1..16); flush `0x114e63` emits opcode-0x0c slot 0x0c. Whether the silicon honours ROP *while blending* is [INFERRED] |
| `GL_EXT_stencil_wrap` | [ICD] `glStencilOp` `0x123d00` explicitly accepts 0x8507/0x8508 → distinct hardware values 7/8, never clamped; flush `0x11458d`–`0x1147c9`, slots 0x34–0x3a |
| `GL_EXT_secondary_color`, `GL_EXT_separate_specular_color` | §5.5 [CONFIRMED]; colour sum also emitted twice (bank 0x05 idx 1 at `0x112312`, bank 0x0c slot 0x1d at `0x11411f`) |
| `GL_EXT_polygon_offset` and core polygon offset | [ICD] emitted as bank-0x05 idx 0x15/0x16/0x17 (`0x112570`). ⚠ `glPolygonOffsetEXT` and `glPolygonOffset` resolve to the **same** address `0x1ab8d0` — the EXT *bias* semantics are not implemented. Conformance bug |
| `GL_MTX_fragment_shader` | [ICD] the MTX compiler writes the **same 0x94-byte instruction records** as the fixed-function texenv generator, and the finished 0x664-byte blob goes out as opcode 0x46 at `0x110250`; `glEndFragShaderMTX` `0x1b6fd0` validates at `0x1b28f0`. §3.4 |
| `GL_KTX_buffer_region` | [ICD] **not a `memcpy`** — allocation `0x357d20` and copy `0x357e20` go through device escapes `cmd=0xc` and `cmd=0x10`; there is no `memcpy` and no `reserve` anywhere in `0x1bf3a0`–`0x1c0000`. That it is the 2D engine is [INFERRED] |
| `GL_ATI/EXT_vertex_array_object` | [ICD] `glNewObjectBufferATI` `0x1c8ee0` validates usage, allocates in **system** memory and registers it with the driver via escape `cmd=0x14`. Driver-pinned host buffers, not VRAM VBOs — real, not stubs |
| `GL_EXT_bgra`, `GL_EXT_packed_pixels` | effectively free: A8R8G8B8 is the native texel order, so `GL_BGRA`+`UNSIGNED_BYTE` hits the identity copy |

**Mixed / partial**

| extension | evidence |
|---|---|
| `GL_ARB/EXT_point_parameters` | point size, min and max **are** hardware (bank-0x0c slots 0x9a/0x9b/0xa6 at `0x1142aa`/`0x1143a1`/`0x1143ea`), but the **distance-attenuation coefficients are never emitted**: they set `ctx+0xad1` → `ctx+0x2688` (`0xfcdf1`), which turns on key bit 28 and a **CPU loop** fills the per-vertex point size (`0xf381e`–`0xf384b`). Same family as the known-software `…Clip_Point_to_Sizedpoint` |
| `GL_EXT_fog_coord` | the *source switch* is hardware — depth-sourced fog → bank-0x0c slot 0x23 (`0x114952`), coordinate-sourced → slot 0x23 zeroed and slot 0x8c written (`0x114b58`) — and the coordinate rides in the same 16-byte specular component (§5.5). `glFogCoordfEXT` is a real pointer-to-member |
| `GL_NV_texgen_reflection` | hardware in the HW-T&L path (texgen emitter `0x111e0b`–`0x1120a7`, opcode-0x06 subs 2–5, mode codes EYE_LINEAR 1, OBJECT_LINEAR 2, SPHERE_MAP 3, **REFLECTION_MAP 5, NORMAL_MAP 6** at `0x1124f5`/`0x112515`); a full CPU `r = u − 2n(n·u)` at `0xfd828`/`0xfd923` serves the SW-T&L path |

**Software / client-side**

| extension | evidence |
|---|---|
| `GL_EXT_compiled_vertex_array` | real CPU cache, not a GPU buffer: `glLockArraysEXT` `0x15cfa0` sets a flag that four draw paths (`0xd9a4f`, `0xda75f`, `0xdb47e`, `0xdbaed`) test to pre-convert the locked range once into the DMA heaps, 64-byte aligned (`0xd9ce7`) |
| `GL_ATI/EXT_element_array` | real client-side redirect: `glDrawElementArrayATI` `0xd8bc0` loads the stored pointer and **forwards to the ordinary `glDrawElements`** (`0xd8bfb`–`0xd8c27`). No GPU index buffer |
| `GL_EXT_multi_draw_arrays` | real entry points, but no batching record exists in the stream → a CPU loop over the single-draw path |
| `GL_EXT_rescale_normal` | real CPU flag (`0x156140` sets `ctx+0xf5` and ORs 0x100 into the matrix-invalidation masks); normals are CPU-transformed. No register |
| `GL_ARB_transpose_matrix` | CPU transpose into the ordinary matrix stack (`0x132c20`, `0x132b50`) |
| `GL_EXT_texture_object`, `GL_EXT_subtexture`, `GL_EXT_vertex_array`, `GL_EXT_draw_range_elements`, `GL_EXT_polygon_offset` API forms | client-side API shape only |
| wide lines, sized points, `glPolygonMode(GL_LINE/GL_POINT)`, selection, feedback | [RTTI] the `GLPrimitiveSoftwareClip_*_to_Wideline` / `_to_Sizedpoint` / `_Selection_*` / `_Feedback_*` family, reached through the §5.2 reason bits |

**No-op**

| extension | evidence |
|---|---|
| `GL_ARB_multisample` | **pure no-op [CONFIRMED].** The block constructor `0x1c7380` on `ctx+0x7c70` sets `+0x00 = 1`, **`+0x04 = 0` (`GL_SAMPLE_BUFFERS`)**, `+0x08 = 0x10` (`GL_SAMPLES` = 16 — self-contradictory and vestigial), `+0x10 = 1.0f`, and **no writer exists after construction**. `glSampleCoverageARB` `0x128ab0` only stores into the context and dirties a bit consumed solely by `glPushAttrib`/`glPopAttrib`. **No `reserve`, no register, anywhere in the multisample path.** This supersedes the "hardware when enabled" reading in §4.2 #10 for the *GL* stack — the FAA modes the Windows driver exposes are simply not wired up on Linux |
| `GL_Autodesk_valid_back_buffer_hint` | `glInitValidBackBufferHintAutodesk` `0x1afd70` = `mov eax,1; ret`; `glWindowBackBufferHint` `0x1afd80` = `ret`; `glValidBackBufferHint` `0x1afd90` = `xor eax,eax; ret` |
| `GL_EXT_clip_volume_hint` | `glHint` stores 0x80F0 at `ctx+0xa84` (`0x1be865`); no reader found on any render path. [INFERRED] — a negative search is not a proof |

### 5.5 One vertex slot serves specular, secondary colour and fog

The `GLPrimitiveSoftware` key builder allocates the bits[10:8] slot (an extra
4-float attribute, `stride += 0x10`) under a three-way condition at
`libGL64 0xf3163`–`0xf3192` [CONFIRMED]:

```
if (light_model_color_control == 0x81FA)          /* GL_SEPARATE_SPECULAR_COLOR */
        enable_secondary_slot();
else if (state[0xa20]                              /* GL_FOG enabled            */
         && (state[0xa21] || state[0xa3c] != 0x8452))   /* fog source != GL_FRAGMENT_DEPTH */
        enable_secondary_slot();
else if (state[0x9ac] == 0 && state[0xa54])        /* GL_COLOR_SUM_EXT          */
        enable_secondary_slot();
```

Those offsets are named by `glGetIntegerv` itself: `+0xa20` = `GL_FOG` (0x0B60),
`+0xa34` = `GL_FOG_MODE`, `+0xa3c` = `GL_FOG_COORDINATE_SOURCE` (0x8450),
`+0xa48`/`+0xa4c` = start/end, `+0xa54` = `GL_COLOR_SUM_EXT` (0x8458); `0x81FA`
is `GL_SEPARATE_SPECULAR_COLOR`.

So **one hardware vertex slot carries the specular colour, the secondary colour
and the fog coordinate** — the classic "specular.rgb plus fog in alpha" packing.
All three extensions cost the same 16 bytes of vertex, and an open driver gets
them from one attribute.

## 6. Per-variant differences

### 6.1 The chip-class word

`mtx_parhelia.o` has one classifier, **`mtx1916` (`0x40a70`)**, which maps
`(PCI device id, revision)` to a **one-hot class bit** written to `dev+0x20`
[CONFIRMED]. A second, structurally identical routine `mtx427` (`0x40b90`)
writes the same classification to `dev+0x68`.

| device | rev | class bit | project variant (`docs/parhelia-variants.md`) |
|---|---|---|---|
| 0x0525, 0x2527 | any | 0x00001 | pre-Parhelia G-series |
| 0x0527 | 0 / 1 / 2 / 3 / other | 0x00002 / 0x00004 / 0x00008 / 0x00020 / 0x00010 | **A — Parhelia-512** |
| 0x0528 | 1 / 2 / 6 / other | 0x00040 / 0x00080 / 0x00200 / 0x00100 | **B — Parhelia 8X / PCI** |
| 0x2537 | 1 / 2 / other | 0x00400 / 0x01000 / 0x00800 | **C — Parhelia-LX (Sundog)** |
| 0x2538 | 0 / 1 / other | 0x02000 / 0x04000 / 0x08000 | **D — Sunex** |
| 0x2539 | 0 / 1 / other | 0x10000 / 0x20000 / 0x40000 | **E — P690 generation** |
| anything else | | 0 | (0x0529 Phoenix is **not** classified — consistent with "stubbed") |

### 6.2 What actually branches on it

The Windows D3D driver's only chip-dependent caps decision is
`test dword [pDev+0x1c], 0x7FC00` (`0x86879` and `0x87bf7`), which selects the
35-entry format table at `.data 0x1d19a0` instead of the 34-entry table at
`0x1d1560` — the difference being the extra Matrox-private texture format
`0x102B0003`. Mask `0x7FC00` is bits 10–18, which by §6.1 is **exactly the
class-bit range of 0x2537 / 0x2538 / 0x2539** — i.e. the test is "is this an
LX-generation part?" [CONFIRMED by matching the mask to the classifier; the
Windows-side writer of `[pDev+0x1c]` lives in the display driver, not in
`MTXPARD.dll`].

Everything else that varies in the D3D caps is a **registry option**, not
silicon: `HwTransformLight` (default 1) gates
`D3DDEVCAPS_HWTRANSFORMANDLIGHT`, `HwPureDevice` (1) gates `PUREDEVICE`,
`VertexShader` (1) gates vs_1_1 versus 0xFFFE0000, `VertexBuffers` (1) gates
`MaxStreams` 8↔1 and the HW vertex/index buffer caps, `GuardBandClipping` (2)
zeroes the guard band when 0, `TEPipeSelect` (4) overwrites the DX7
`wMaxSimultaneousTextures` *after* CAPS8/9 are built, `np2Conditional` (0) adds
`NONPOW2CONDITIONAL`, `MipmapLodBias` (0) adds `MIPMAPLODBIAS`, and `AAMode`
(0x54) drives the multisample rewrite.

That is itself a finding: **on the Windows side the vendor made hardware T&L,
vertex shaders, guard-band clipping and NPOT-conditional textures switchable at
runtime, all defaulting to on.** None of those switches exists in the Linux GL
declaration.

### 6.3 The ICD does not branch on the variant at all — it asks the kernel

**No PCI device id appears anywhere in the ICD** — not as an instruction
immediate in either build, not as a 16- or 32-bit table entry in `.rodata` or
`.data.rel.ro`, and not in `mtx_drv.so` either [CONFIRMED]. The ICD never probes
PCI: it opens `/dev/matrox/mtx<minor>` with the minor supplied by the X server
through a GLX reply (`0x360cb5`) and uses exactly four ioctls —
`0x80304600` GET_MEMORY_INFO (`MtxDevice::Open` `0x3590d0`), `0xc038461f` the
ICD-link create, `0x4620` destroy and `0x4621` the doorbell.
`DRIVER_VALUE_REQUEST` (`0xc03c4603`) is never used, so the ICD cannot read
driver parameters by name.

Its entire hardware model is a **40-byte capability blob** copied out of the
`0xc038461f` reply (offsets +0x10…+0x37) into `ICDLinkImpl+0x10`, read back
through `GetCaps()` at **`0x364ec0`** (vtable slot +0x48; singleton at
`0x6a3570`):

| caps byte | read at | what it gates | value on this stack |
|---|---|---|---|
| +0x00 | `0x1bf020` | **hardware T&L**; 0 → software pipeline (reason bit 3) | module-parameter derived — `reg3DHwTransformLight` [INFERRED] |
| +0x01 | `0x1bef02` | which state-validation block runs | constant 1 |
| +0x02 | `0x1bf0b5` | polygon mode point/line (reason bit 5) | module-parameter derived |
| +0x03 | `0x1133ca`, `0x12841a` | a texture-stage pass computation | module-parameter derived |
| +0x04…+0x10 | `0xf0280`, `0xf0b00` | guard-band clip rect (`w = c[0x0c] − c[0x04]`) | 0 or ±8160.0f — the same guard band D3D declares (§4.2 #6) |
| +0x14 | `0xd24f8`, `0xe03d1` | single- versus multi-pass over texture stages | constant **8** |
| +0x18 | `0x15ca60`, `0x17cf8f` | max vertices per submitted batch | constant **0xffff** |
| +0x1c | `0x1bee63` | hardware line path (reason bit 0) | **constant 0** |
| +0x1d | `0x1bedf1` | hardware wide-line path (reason bit 1) | **constant 0** |
| +0x1e | `0x1bf064` | line-smooth path (reason bit 8) | **uninitialised — see below** |

**Correction, 2026-09-13: the kernel writes nothing past `+0x1d`.** The
ICD-link-create handler `mtx1775` (`mtx_parhelia.o` `.text+0x1a50`) copies the
caller's 56-byte packet onto an unzeroed stack frame, fills the blob at packet
`+0x10`, and copies all 56 bytes back. Its complete store list is `+0x00`
(`0x1b50`), `+0x01` (`0x1b4c`, constant 1), `+0x02` (`0x1b61`), `+0x03`
(`0x1b71`), `+0x04`/`+0x08` (`0x1b83`/`0x1b86`), `+0x0c`/`+0x10`
(`0x1b93`/`0x1b96`), `+0x14` (`0x1ba6`), `+0x18` (`0x1bb0`), `+0x1c` (`0x1b89`),
`+0x1d` (`0x1b8d`). **`+0x1e`, `+0x1f` and `+0x20..+0x27` are never written**,
and the ICD (`0x358c30`) does not zero its own 64-byte packet either — it
initialises only `+0x00` — so those ten bytes round-trip as the ICD's own stack
garbage. Whatever `0x1bf064` reads at `+0x1e`, it is not a kernel-supplied
capability. [CONFIRMED]

Two consequences worth acting on. **The wide-line software fallback is a kernel
policy, not a hardware limit** — `caps[0x1d]` is a hard-coded 0. And the
kernel's `caps[0x14] = 8` is *not* a texture-unit count (the ICD advertises and
enforces 4); it is an internal multi-pass threshold. Do not read it as 8 units.

The real tunables are **kernel module parameters** — `kernel/src/MtxParhlParms.c`
has 62 of them, including `reg3DHwTransformLight`, `reg3DHwPureDevice`,
`reg3DTrinityEnable`, `reg3DVertexShader`, `reg3DGuardBandClipping`,
`regOGLWideLines`, `regOGLLineStipple`, `reg3DUseTextiled`,
`reg3DTEMultiplePipes` — set at `modprobe` time. This is the Linux counterpart
of the Windows registry switches in §6.2, and it is why `GL_RENDERER` is one
fixed string with no P650/P750/QID/P690 variant anywhere.

The kernel *does* classify the board (§6.1) and tests the class 59 times as
`testl $0x70000` / `$0x7fc00`, but only inside its own DMA and engine paths —
**the class never reaches the caps blob and never reaches the ICD**
[CONFIRMED].

**One experiment closes the variant question for GL:** run the existing
`mtxtrace` harness on the 0x0528 board and on a P690 and diff those 40 bytes.

---

## 7. The state-validation path

### 7.1 Lazy validation by dispatch-table swap

The ICD does not check a dirty flag on every GL call. It **swaps the whole GL
entry-point table** in thread-local storage. The RTTI names survive in the
binary and name the three pipelines:

| class | RTTI name at file offset | vtable (`.data.rel.ro`) | first method |
|---|---|---|---|
| `GLSendStateHardwareTnL` | 3587376 | `0x467620` | `0x116320` |
| `GLSendStateHardwareTnLVS` | 3587408 | `0x467760` | `0x118790` |
| `GLSendStateSoftwareTnL` | 3587440 | `0x467880` | `0x11b330` |

Each of the ~30 virtual slots in these tables is a **shim**: it calls a single
validate routine, re-reads the (now updated) TLS dispatch pointer, and
tail-jumps to the corresponding slot of the real table. E.g. slot 0 at
`0x116320`: `call 0x116140` → `pthread_getspecific` → `jmp [[tls]+0x00]`
[CONFIRMED].

**`Validate()` = `libGL64 0x116140`** [CONFIRMED]:

```
if (check([this+0x250]))            /* 0x1bed70 */
        fast_path([this+0x250]);    /* 0x1bf150 — nothing to revalidate */
else {
        [this+0x258]->[0x70]->vtbl[0xa8]();      /* begin state emit           */
        validate_A([this+0x238] + 0x0c30);       /* 0x15d120                   */
        validate_tex([this+0x238] + 0x4160);     /* 0x14c2b0 — texture units   */
        if (!dev->vtbl[0x18]()) ... else dev->vtbl[0x20]();
        emit_state([this+0x248]);                /* 0x113b80                   */
        emit_state2([this+0x248]);               /* 0x1117c0 — the big one     */
        [this+0x258]->[0x70]->vtbl[0xb0]();      /* end state emit             */
        if ([this+0x240][0x10a8]) { 0x15af70(); [this+0x240][0x10a8] = 0; }
        finish([this+0x238]);                    /* 0x122e30                   */
        pthread_setspecific(key, [this+0x258]->[0x70]);   /* install valid tbl */
}
```

The last line is the mechanism: after validation the TLS pointer names the
*validated* dispatch table, so subsequent GL calls cost nothing extra. Any
state change that invalidates the pipeline re-installs the shim table. There is
a matching `GLStateInvalid` RTTI name (file offset 3587696) and
`GLStateInsideBegin` / `GLStateInsideVSBegin` (3587536 / 3587568) — the same
trick is used to swap in the inside-`glBegin` dispatch.

### 7.2 What is cached: per-item shadow compare

`emit_state2` (`0x1117c0`) is the model for the whole driver. For each state
item it compares the GL value against a **per-device shadow** and only emits if
it differs:

```
if ([state+0xaa8] != shadow[0x96c]) {
        shadow[0x96c] = value;
        p = reserve(buf, /*opcode*/ 0x05, /*elemsize*/ 8, /*count*/ 1);
        p[0] = 0x1b; p[1] = value;          /* (index, value) */
        commit(buf);
}
```

Every emission in the ICD has this shape. The shadows live in the device object
(e.g. `+0x96c`, `+0x274`, `+0x23c`, `+0x910`) and the buffer is
`dev+0xb08`. The PIPELINE-KEY has its own 6-dword shadow at `dev+0xb10`,
compared field by field before emitting (`0xc50ab`–`0xc50f0`, `0xf34c9`–
`0xf34fa`) [CONFIRMED].

### 7.3 The dirty mask and what forces which record

Above the per-item shadows sits a coarse dirty word, `[prim+0x238]`, and a
flush driver **`libGL64 0xc4dd0`** called as `Flush(this, src, mask)`:

| mask bit(s) | what it flushes | code |
|---|---|---|
| 0x0003 | **PIPELINE-KEY** — compare `src+0x30..0x44` against `dev+0xb10`, emit opcode 0x07 | `0xc4e0a` → `0xc4f90` → `0xc5113` |
| 0x000c | **vertex attribute declaration** — emit opcode 0x08, `count = [src+0x170]` elements of 0x20 bytes | `0xc4e17` → `0xc4fd6` → `0xc4d63` |
| 0x0070 | flush pending vertex data | `0xc4e2b` → `0xc5000` |
| 0x0080 | virtual `[this][0x120]` | `0xc4e38` → `0xc5030` |
| 0x0300 | texture/sampler revalidation (`0x1589c0`) | `0xc4e47` |
| 0x0c00 | a texgen/texture-stage check on `[..+0x77c..0x78c] == 7` | `0xc4e8e` |
| 0x1000 | emit opcode 0x48 (0x18 B) or opcode 0x50 (0x20 B) | `0xc4eff` / `0xc507a` |

The dirty word is initialised to `0xffffffff` (all dirty) at `0xc28c0` and
`0xc4cfa`. Individual GL setters mark bits; e.g. `0xf3530` sets bit 1 of
`[obj+0x25cc]` when a value changes.

The kernel keeps a **second** dirty layer of its own — three words at
`ctx+0x1bec/0x1bf0/0x1bf4` plus per-group bytes (`+0xc08`, `+0xc87`, `+0xdbb`,
`+0xd54`, `+0x1b44`, …). `mtx1008` (`0x18160`) marks everything dirty,
`mtx412` (`0x18720`) clears everything, and `mtx3712` (`0x188f0`) is the pass
that walks them and programs the hardware.

### 7.4 What forces a submit

Only seven call sites reach the flush `0x1c9be0` [CONFIRMED]:
`0x1c9d13` (reserve found the buffer full), `0x1c9d7c`, `0x1c9f06` (buffer
helpers), `0x1b21c6` and `0x1b2406` (vertex-heap helpers), `0x1ba5a2`, and
`0x35897e` (an explicit end-of-frame path in the GLX layer, which first calls
the device's `vtbl[0xa8]`). So a submission happens on **buffer exhaustion** or an explicit
end-of-frame/finish, not per draw. That matches the capture, where each
`glFinish` produced exactly one submission and `region0+0x230` held the whole
stream length.

`GLPrimitiveSoftware::WriteStoredVertex` also confirms how the *vertex* stream
is filled — `0xf37b0` walks the PIPELINE-KEY fields in order (position 16 B,
optional bit-28 dword 4 B, optional specular 16 B, diffuse 16 B, then each
enabled texture stage 16 B) and bumps `obj+0x25e8` [CONFIRMED]. The
fixed-function path uses the narrower per-field sizes of §1.2.

### 7.5 Which GL state is lazy

From the sweep and the code together: **everything is lazy.** No GL setter
writes a record; setters write the ICD's own state block and mark dirty bits,
and the first draw (or clear) after the change runs `Validate()`. That is why
`docs/parhelia-gl-feature-map.md` §3 sees each op's stream carrying the
*previous* op's teardown. Two consequences an open driver should copy:

* state is emitted in **validation order, not call order** — do not try to
  attribute a record to the immediately preceding GL call;
* an unchanged value is never re-emitted, so a capture will silently omit any
  state that happens to already match the shadow (this is exactly why
  `GL_ALWAYS` never appeared for the stencil compare function).

---

## 8. Summary for an open driver

Confirmed real hardware limits (both vendor stacks agree): 2048×2048 textures,
2048 volume extent, **4 texture units** (hard-checked at `0x146aa7`),
**anisotropy 2** (clamped at `0x132062`), 6 user clip planes, DXT1–DXT5, all
blend factors both sides, all 8 depth/alpha compares, the full stencil op set
including wrap, and **10 fragment-program colour instructions** (`0x1b6126`).

Confirmed *under*-declarations, in descending order of value:

1. **A programmable vertex unit that is complete and deliberately fenced off.**
   The ICD ships the compiler, 30 instruction classes, ~50 parameter bindings, a
   `GLPrimitiveHardwareVS` pipeline, a `GLSendStateHardwareTnLVS` state emitter,
   a selector branch and a hardware mode register — and then makes
   `glEnable(GL_VERTEX_SHADER_EXT)` write **0** (§5.3). Windows declares the same
   silicon as vs_1_1 with 256 constants. An open driver should plan for hardware
   vertex programs: submission is opcode 0x50, the arm is bank-0x05 index
   0x12 = 1.
2. **Non-power-of-two textures** — the hardware has them (2D, cube, volume,
   mipmapped, per `TextureCaps`); the ICD hard-rejects them because GL 1.3
   required POT and nobody added the extension.
3. **12 lights**, not 8 — and `GL_MAX_LIGHTS` is not answerable at all.
4. **Wide lines and sized points are software only because the kernel says so.**
   `caps[0x1c]` and `caps[0x1d]` are hard-coded 0 in the capability blob
   (§6.3), which is what forces the CPU line-expansion path the capture
   recorded. Nothing about the silicon is established by that fallback.
5. **`GL_ARB_multisample` is advertised and does nothing** (§5.4) while the
   Windows driver drives 4×/16× FAA. Mis-declared rather than under-declared.
6. **BUMPENVMAP / BUMPENVMAPLUMINANCE / MULTIPLYADD / LERP** texture ops, and
   **4-matrix vertex blending** and **8 texcoord sets**, all with no GL path.
7. **Guard band ±8160** versus a declared 4096 viewport — and the ICD is handed
   that same guard band in `caps[0x04..0x10]`.

Two vendor bugs not to reproduce: the LOD-bias stride mix-up at `0x10ff14`
(`0x37d8` versus `0xb28`, so units 1–3 get garbage) and `glPolygonOffsetEXT`
aliasing core `glPolygonOffset` (bias versus units).

And the structural lesson: the ICD→kernel stream is a **render-state token
stream**, not a register stream (§2.2) — the two banks 0x05 and 0x0c use
overlapping small indices, so `0x1000 + index*4` cannot be right for both, and
the kernel disassembly shows it is right for neither. The GL→state-id→value
mapping recovered here and in `docs/parhelia-gl-feature-map.md` is sound and
reusable; the state-id→MMIO-register half is the kernel's per-index jump tables
(`.rodata+0xba0` for bank 0x0c, `mtx1734`'s table for bank 0x05) plus the
programming pass `mtx3712`, and that is the piece an open driver must write for
itself.

## 9. Experiments that would close the remaining gaps

| question | experiment |
|---|---|
| Does bank-0x05 index 0x12 = 1 by itself arm the WARP vertex unit? | patch the `glEnable` site `0x1255e8` (`c6 80 38 01 00 00 00`) to store 1, run the vendor stack on the 0x0528 board, capture with the mmiotrace method of `docs/parhelia-mmio-capture.md` |
| Do the LX/P690 parts report a different capability blob? | run `mtxtrace` on the 0x0528 board and on the P690 and diff the 40 bytes at reply offsets +0x10…+0x37 |
| Which module parameters back `caps[0]`, `caps[2]`, `caps[3]`? | sweep `reg3DHwTransformLight` / `reg3DHwPureDevice` / `reg3DTEMultiplePipes` at `modprobe` time and diff the same blob |
| The opcode-0x0e / 0x06 sub-id → `0x11xx` register map (LOD bias sub 0x13, edge versus border clamp) | decode the sub-id switch in `mtx_parhelia.o`, or capture with a non-zero LOD bias |
| Is `GL_EXT_clip_volume_hint` really a no-op? | toggle the hint and diff the emitted stream (only a negative search supports it today) |
| Is the `GL_KTX_buffer_region` blit the 2D engine? | decode kernel escape `cmd=0x10` |
| Do the constant-colour blend factors and separate-alpha blending work on real silicon? | one capture each — they are the two places GL claims more than D3D (§4.2 #11, #12) |
