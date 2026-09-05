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
| [10:8] | **secondary colour / specular / fog coordinate** — one shared slot, see §5.1 | 0 = absent; 1 (fixed-function) / 5 (software) = present | 0 / 16 B |
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
`GL_SAMPLE_BUFFERS` / `GL_SAMPLES` are read from `ctx+0x7c74` / `ctx+0x7c78`.

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
| 2 | no vertex-program extension advertised, but the ICD answers 512-instruction `GL_EXT_vertex_shader` limits (§3.3) | `VertexShaderVersion` = **`0xFFFE0101` = vs_1_1**, `MaxVertexShaderConst` = **256**, `D3DDEVCAPS_HWTRANSFORMANDLIGHT` and `D3DDEVCAPS_PUREDEVICE` set (`0x86c7a`, `0x87650`, `0x87678`) | **The part has a programmable vertex unit and hardware T&L, and the Linux ICD does not advertise it.** See §5.3 item 1. |
| 3 | `GL_MTX_fragment_shader` only | `PixelShaderVersion` = **`0xFFFF0103` = ps_1_3** (`0x86c8a`), `PixelShader1xMaxValue` 1.0f | The MTX extension is the GL door onto a ps_1_3-class combiner; the limits in §3.4 should be read against `docs/parhelia-fragment-isa.md`. |
| 4 | `GL_ARB_texture_env_combine` / `dot3` | `TextureOpCaps` = 0x03FEFFFF: additionally **BUMPENVMAP, BUMPENVMAPLUMINANCE, MULTIPLYADD, LERP** (`.data 0x1d1e4c`) | Bump mapping and LRP/MAD are hardware but have no fixed-function GL path. Reachable through the fragment ISA (`MAD`, `LRP` are in the opcode table already). |
| 5 | `GL_POINT_SIZE_RANGE` / `GL_LINE_WIDTH_RANGE` 0.25–**10.0** | `MaxPointSize` = **2048.0f** (`0x86bda`) | The 10.0 is a *software* rasteriser limit, consistent with the capture's CPU-expanded wide-line path. `GL_ALIASED_*_RANGE` already reports 1–2048. |
| 6 | `GL_MAX_VIEWPORT_DIMS` 4096×4096 | guard band **±8160.0f** (`.data 0x1d1e30`–`0x1d1e3c`, also inline in the clipper at `0xd6cf6`) | The clip window is nearly ±8K; 4096 is the ICD's own choice. |
| 7 | `GL_MAX_ELEMENTS_VERTICES/INDICES` 100000 | `MaxPrimitiveCount` = `MaxVertexIndex` = **0x7FFFFFFF**, `MaxStreamStride` 65535, `MaxStreams` 8 | GL's are only hints, but the gap is real. |
| 8 | 4 texture units | `FVFCaps` TEXCOORDCOUNTMASK = **8** texcoord sets (`0x1d1e48` + `or 0x100000` at `0x86b94`), `MaxVertexBlendMatrices` 4, `MaxVertexBlendMatrixIndex` 31 | 8 texcoord *sets* can be delivered to 4 sampling stages; also 4-matrix vertex blending exists with no GL exposure (no `GL_ARB_vertex_blend`). |
| 9 | fog listed as CPU-side in `docs/parhelia-gl-driver.md` | `RasterCaps` 0x07F309A7 sets **FOGVERTEX, FOGTABLE, FOGRANGE, WFOG, ZFOG**, plus **SCISSORTEST, DEPTHBIAS, SLOPESCALEDEPTHBIAS, ANISOTROPY** (`0x1d1ee4`) | Fog, scissor and polygon offset are hardware. The live capture already shows fog enable and fog mode as emitted state, so the old "software fog / software polygon offset" note is wrong. |
| 10 | `GL_ARB_multisample` advertised, `GL_SAMPLES` read from the drawable | multisample word rewritten per format at `0x86f90` from the `AAMode` registry setting: `0x0009`, `0x8001` or 0 | FAA is a driver/mode option, not a fixed cap. |
| 11 | advertises `GL_EXT_blend_color`, and `docs/parhelia-gl-driver.md` lists `CONSTANT_COLOR`=0xe … `1−CONSTANT_ALPHA`=0x11 in the factor enum | `SrcBlendCaps`/`DestBlendCaps` = 0x1FFF, i.e. **`D3DPBLENDCAPS_BLENDFACTOR` is not set** | Direction of the gap is **reversed** here — GL claims more than D3D. Either the constant-colour factors are a GL-side emulation or the D3D driver under-declares. [AMBIGUOUS] — settle it with one `glBlendColorEXT` + `glBlendFunc(GL_CONSTANT_COLOR, …)` capture. |
| 12 | advertises `GL_EXT_blend_func_separate`, and the capture shows **four** independent factor state ids (src RGB / src A / dst RGB / dst A) | `PrimitiveMiscCaps` = 0x00102CF3, **`SEPARATEALPHABLEND` (0x20000) clear** | Same reversed direction. The four separate factor slots in the live stream are strong evidence the hardware has it and the D3D driver simply never declared it. [AMBIGUOUS] |
| 13 | advertises `GL_EXT_texture_lod_bias`, `GL_MAX_TEXTURE_LOD_BIAS_EXT` = 2 | `D3DPRASTERCAPS_MIPMAPLODBIAS` is added **only** when the `MipmapLodBias` registry value is non-zero (`0x8785d`), i.e. off by default | The Windows driver treats LOD bias as opt-in; the GL driver exposes it unconditionally. Worth a look before relying on it. |

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

### 5.1 What the secondary-colour slot really is

Before the table, one result that settles three extensions at once. The
`GLPrimitiveSoftware` key builder allocates the bits[10:8] slot (an extra 4-float
vertex attribute, `stride += 0x10`) under a three-way condition at
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

Those state offsets are named by `glGetIntegerv` itself: `+0xa20` = `GL_FOG`
(0x0B60), `+0xa34` = `GL_FOG_MODE`, `+0xa3c` = `GL_FOG_COORDINATE_SOURCE`
(0x8450), `+0xa48/+0xa4c` = start/end, `+0xa54` = `GL_COLOR_SUM_EXT` (0x8458),
and `0x81FA` is `GL_SEPARATE_SPECULAR_COLOR`.

So **one hardware vertex slot serves the specular colour, the secondary colour
and the fog coordinate**, and all three of `GL_EXT_separate_specular_color`,
`GL_EXT_secondary_color` and `GL_EXT_fog_coord` are hardware features that cost
16 bytes of vertex each. An open driver gets them from the same attribute.

### 5.2 Classification

Evidence codes: **[D3D]** the Windows driver declares the capability for this
silicon (§4); **[KEY]** the pipeline-key/vertex-layout decode shows a real
hardware attribute (§1); **[CAP]** the live sweep shows emitted state;
**[RTTI]** a `GLPrimitiveSoftware*` class does the work on the CPU.

| extension | verdict | evidence |
|---|---|---|
| `GL_ARB_multitexture`, `GL_SGIS_multitexture` | **hardware**, 4 stages | [D3D] `MaxSimultaneousTextures` 4; [KEY] four texcoord fields; the ICD's reset emits 4 stages × 28 texture-stage states |
| `GL_ARB_texture_env_add`, `GL_EXT_texture_env_add` | **hardware** | [D3D] `TextureOpCaps` ADD |
| `GL_ARB/EXT_texture_env_combine` | **hardware** | [D3D] `TextureOpCaps` 0x03FEFFFF (all MODULATE/ADD/BLEND forms); [CAP] fragment programs uploaded per env mode |
| `GL_ARB/EXT_texture_env_dot3` | **hardware** | [D3D] `TextureOpCaps` DOTPRODUCT3; `DP3` is in the fragment ISA opcode table |
| `GL_ARB/EXT_texture_cube_map` | **hardware** | [D3D] `TextureCaps` CUBEMAP + MIPCUBEMAP; `GL_MAX_CUBE_MAP_TEXTURE_SIZE` 2048 |
| `GL_EXT_texture3D` | **hardware** | [D3D] `TextureCaps` VOLUMEMAP + MIPVOLUMEMAP, `MaxVolumeExtent` 2048; the ICD validates 3D dims against 2048/POT like 2D |
| `GL_ARB_texture_compression`, `GL_EXT_texture_compression_s3tc`, `GL_S3_s3tc` | **hardware** | [D3D] DXT1–DXT5 declared as texture, volume **and** cube |
| `GL_EXT_texture_filter_anisotropic` | **hardware**, max 2 | [D3D] `RasterCaps` ANISOTROPY + `TextureFilterCaps` MINF/MAGFANISOTROPIC; the ICD clamps to 2.0f at `0x132062` |
| `GL_ARB_texture_border_clamp`, `GL_EXT_texture_edge_clamp` | **hardware** | [D3D] `TextureAddressCaps` 0x3F = WRAP\|MIRROR\|CLAMP\|BORDER\|INDEPENDENTUV\|MIRRORONCE |
| `GL_EXT_blend_minmax`, `GL_EXT_blend_subtract` | **hardware** | [D3D] `PrimitiveMiscCaps` BLENDOP |
| `GL_EXT_blend_func_separate` | **hardware** [INFERRED] | [CAP] four independent factor state ids in the live stream; [D3D] does **not** declare it (§4.2 #12) |
| `GL_EXT_blend_logic_op` | **hardware** [INFERRED] | [D3D] `RasterCaps` ROP2 + XOR |
| `GL_EXT_blend_color` | **[AMBIGUOUS]** | [D3D] `BLENDFACTOR` bit clear (§4.2 #11) |
| `GL_EXT_stencil_wrap` | **hardware** | [D3D] `StencilCaps` includes INCR/DECR, the wrapping ops |
| `GL_EXT_secondary_color`, `GL_EXT_separate_specular_color`, `GL_EXT_fog_coord` | **hardware**, one shared vertex slot | §5.1 [CONFIRMED]; [D3D] `ShadeCaps` SPECULARGOURAUDRGB, `RasterCaps` FOGVERTEX/FOGTABLE/FOGRANGE/WFOG/ZFOG |
| `GL_ARB_point_parameters`, `GL_EXT_point_parameters` | **mixed** — the per-vertex point size is a real attribute, the sized-point *rasterisation* is CPU | [KEY] key bit 28 (§1.2); [D3D] `FVFCaps` PSIZE; [RTTI] `GLPrimitiveSoftwareClip_*_to_Sizedpoint` |
| `GL_EXT_polygon_offset` (and core polygon offset) | **hardware** | [D3D] `RasterCaps` DEPTHBIAS + SLOPESCALEDEPTHBIAS — this contradicts the "software polygon offset" note in `docs/parhelia-gl-driver.md` |
| `GL_NV_texgen_reflection` | **hardware** | [D3D] `VertexProcessingCaps` TEXGEN; `TextureCaps` PROJECTED |
| `GL_EXT_rescale_normal` | **hardware** [INFERRED] | part of fixed-function T&L, which is hardware ([D3D] HWTRANSFORMANDLIGHT) |
| `GL_SGIS_texture_lod` | **hardware** [INFERRED] | per-texture LOD clamp is texture-stage state |
| `GL_EXT_texture_lod_bias` | **[AMBIGUOUS]** | §4.2 #13 — D3D gates `MIPMAPLODBIAS` behind a registry option |
| `GL_ARB_multisample` | **hardware when enabled** | [D3D] the per-format multisample word is rewritten from the `AAMode` setting (FAA); `GL_SAMPLES`/`GL_SAMPLE_BUFFERS` come from the drawable, not a constant |
| `GL_MTX_fragment_shader` | **hardware** — the ps_1_3-class combiner | §3.4 limits; program objects uploaded with opcodes 0x45/0x46; `docs/parhelia-fragment-isa.md` |
| `GL_EXT_bgra`, `GL_EXT_packed_pixels` | **free** — B,G,R,A *is* the native texel order | [D3D] the format list is A8R8G8B8 / X8R8G8B8 / R5G6B5 / A1R5G5B5 / A4R4G4B4 / A2R10G10B10 |
| `GL_EXT_texture_object`, `GL_EXT_subtexture`, `GL_EXT_vertex_array`, `GL_EXT_compiled_vertex_array`, `GL_EXT_draw_range_elements`, `GL_EXT_multi_draw_arrays`, `GL_ATI_element_array`, `GL_EXT_element_array`, `GL_ATI_vertex_array_object`, `GL_EXT_vertex_array_object`, `GL_ARB_transpose_matrix`, `GL_EXT_clip_volume_hint`, `GL_Autodesk_valid_back_buffer_hint`, `GL_KTX_buffer_region` | **client-side API only** — no pipeline state exists for them; they change how the ICD packs and submits, not what the GPU does | mechanical: they have no counterpart in any cap word and nothing in §2.1's opcode set |
| wide lines, sized points, `glPolygonMode(GL_LINE/GL_POINT)`, selection and feedback | **CPU** | [RTTI] the `GLPrimitiveSoftwareClip_*_to_Wideline` / `_to_Sizedpoint` / `_Selection_*` / `_Feedback_*` family; [CAP] the wide-line op expands one line into four pre-transformed 1-pixel lines |

### 5.3 Still open

Three questions this pass did not settle, with what would settle them:

1. **Are the exported `GL_EXT_vertex_shader` entry points real?** The ICD
   exports the complete set (`glBeginVertexShaderEXT`, `glEndVertexShaderEXT`,
   `glGenVertexShadersEXT`, `glBindVertexShaderEXT`, `glShaderOp1/2/3EXT`,
   `glSetInvariant*EXT`, …), answers all its limit enums (§3.3), and carries the
   classes `GLVertexShaderFunction` / `GLVertexShaderDeclaration` /
   `GLVertexShaderFunctionDX` / `GLVertexShaderDeclarationDX` and a whole third
   pipeline `GLPrimitiveHardwareVS` / `GLSendStateHardwareTnLVS` — yet does not
   advertise the extension. D3D declares vs_1_1 with 256 constants for the same
   silicon. Following `glBeginVertexShaderEXT` and `glShaderOp3EXT` into the
   real code, and finding whether anything ever constructs
   `GLPrimitiveHardwareVS` (vtable at `.data.rel.ro` file offset 0x465ac8,
   typeinfo name "21GLPrimitiveHardwareVS" at file offset 3581376), decides
   whether the Parhelia's programmable vertex unit is merely under-declared.
   **This is the single highest-value follow-up.**
2. **Names for the `GL_MTX_fragment_shader` enums 0x8A81–0x8A8D** (§3.4). The
   ICD contains a run of `"Error in Matrox Fragment Shader - …"` strings from
   file offset 3595720 ("Too many color instructions", "Too few instructions",
   "Do not meet the minimum number of texture coordinates required", …); each
   raise site is checking exactly one of these limits, so matching value to
   check names the enum. Cross-check against
   `docs/parhelia-fragment-isa.md` §9.3.
3. **ICD-side variant branching.** A byte search of `libGL.so.1.3.0` finds no
   `vendor:device` dword (`0x0527102b`, `0x0528102b`, `0x2537102b`,
   `0x2539102b` all absent), so the ICD does not appear to switch on the PCI id
   directly; if it varies at all it must be through a capability word obtained
   from the kernel module (whose classifier is §6.1). Undetermined.

---

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

*(ICD-side variant branching: see §5.3 item 3 — undetermined, but no PCI-id
constant appears in the ICD at all.)*

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
2048 volume extent, **4 texture units**, **anisotropy 2**, 6 user clip planes,
DXT1–DXT5, all blend factors both sides, all 8 depth/alpha compares, the full
stencil op set including wrap.

Confirmed *under*-declarations in the Linux GL stack, in descending order of
value:

1. **Non-power-of-two textures** — the hardware has them (2D, cube, volume,
   mipmapped); the ICD hard-rejects them.
2. **A programmable vertex unit with hardware T&L** — declared as vs_1_1 with
   256 constants on Windows; on Linux the ICD ships the whole
   `GL_EXT_vertex_shader` entry-point set, its limit enums and a third
   `…HardwareTnLVS` pipeline, but never advertises the extension. Whether those
   entry points are functional is the one thing this pass did not settle
   (§5.3 item 1) — verify before relying on it.
3. **12 lights**, not 8, and `GL_MAX_LIGHTS` is not answerable at all.
4. **BUMPENVMAP / BUMPENVMAPLUMINANCE / MULTIPLYADD / LERP** texture ops with
   no fixed-function GL path.
5. **Guard band ±8160** versus a declared 4096 viewport.
6. **4-matrix vertex blending**, **8 texcoord sets**, and hardware
   fog / scissor / slope-scaled depth bias.

And the structural lesson: the ICD→kernel stream is a **render-state token
stream**, not a register stream (§2.2). The GL→state-id→value mapping already
recovered in `docs/parhelia-gl-feature-map.md` is sound and reusable; the
state-id→MMIO-register half is the kernel's per-index jump tables
(`.rodata+0xba0` for bank 0x0c, `mtx1734`'s table for bank 0x05) plus the
programming pass `mtx3712`, and that is the piece an open driver must write for
itself.
