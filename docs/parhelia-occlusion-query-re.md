# Occlusion queries on Parhelia: there is no pixel counter

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This closes the `Occlusion query | **absent**` row of `parhelia-3d-coverage.md`,
which recorded that no vendor stack exposes the feature but explicitly flagged
that as *not* proof about the silicon.  It is now as close to proof as static
reverse engineering can get: **six vendor binaries across two operating systems
and two chip generations were swept, and not one of them reads a fragment
counter, names one, or leaves a hole where one could be.**  Two of them contain
an explicit refusal.

Addresses in `mtx_parhelia.o` are `.text+` offsets; addresses in `MTXPARD.dll`
and `mtxpserd.dll` are RVAs as `objdump` prints them (image base `0x10000`);
addresses in `libGL.so.1.3.0` are virtual addresses.

## Binaries swept

| file | role | chips bound |
|---|---|---|
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | Linux engine/3D HAL | all |
| `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0` | Linux GL ICD | all |
| `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` | Linux X/2D/DPS HAL | all |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | Windows D3D HAL | 0527 0528 2537 2538 |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll` | Windows GL ICD, v2.06.00.002 | 0527 0528 2537 2538 |
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll` | Windows D3D HAL (P690) | 2537 2538 2539 |
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpsogl.dll` | Windows GL ICD, v2.07.00.008 | 2537 2538 2539 |

The chip bindings are from `mtxpar.inf` and `mtxpser.inf`.

## 1. No register in the part is read as a counter  [CONFIRMED]

### 1.1 The Windows D3D HAL reads 27 distinct registers, and 12 of the 3D block

`MTXPARD.dll` routes every register access through two helpers:

```
0001b690  ReadReg(pDev, off, &out):  ecx = [pDev+0x2024]; out = *(ecx + off)
0001b6c0  WriteReg(pDev, off, val)
```

309 read call sites, of which 260 carry a literal offset.  The complete set of
literal read offsets is

`0x000`, `0x41c`, `0x450`, `0x8f0`, `0x908`, `0x914`, `0x924`, `0x940`,
`0x1220`, `0x1224`, `0x1230`, `0x1234`, `0x12b0`, `0x1304`, `0x1308`, `0x130c`,
`0x1314`, `0x1324`, `0x1328`, `0x132c`, `0x1c00`, `0x1c04`, `0x1c10`, `0x1c4c`,
`0x1cac`, `0x1cb0`, `0x1cb8`

and the 49 computed-offset sites are all either display registers
(`head*0x200 + 0x1830 / 0x19a8 / 0x19d4`) or the shadow-versus-hardware read
wrapper, whose offset is the caller's.  Every one of the twelve 3D-block reads
lives in one routine at `0x10df60`–`0x10e140`, and every one of them is an
identity read-modify-write — read into `[esp+0x6c]`, push it straight back to
`WriteReg` — i.e. an aperture touch, not a value anyone consumes.

`mtxpserd.dll` (the P690-generation HAL, read helper at `0x1b700`) has the
**identical** read set, 309 sites, same offsets.  Two chip generations, no
difference.

Widening the net to *any* call that is handed a 3D-block constant, not just the
register helpers, yields 66 distinct registers in `0x1000..0x13fc`.  None of the
44 registers in that block that no document has ever named appears at all:

```
0x107c 0x1134 0x1140 0x115c 0x11a0 0x11a4 0x11a8 0x11b0 0x11b4 0x11b8 0x11bc
0x11ec 0x11f8 0x1210 0x1214 0x1218 0x1254 0x1258 0x1294 0x1338 0x133c 0x1348
0x134c 0x1350 0x1354 0x135c 0x1360 0x1364 0x1368 0x1370 0x137c 0x1380 0x1384
0x1394 0x139c 0x13a4 0x13a8 0x13b4 0x13dc 0x13e0 0x13e4 0x13e8 0x13f4 0x13f8
```

### 1.2 The Linux kernel HAL reads 14 distinct registers

`mtx_parhelia.o` has no direct volatile loads off the register mapping; all
traffic goes through the two undefined OS symbols `memReadDword` (118 sites) and
`memWriteDword` (200 sites), with the register `MEMHANDLE` at device offset
`0x2030`.  The 118 read sites reduce to

| offset | what it is |
|---|---|
| `0x300` | option/config read-modify-write at init |
| `0x318` / `0x31c` | 2D/blit status, poll `(v & 0x57) == 0x50` |
| `0x41c` | DMA ring fetch pointer / DWGSYNC completion |
| `0x450` | indexed-FIFO-empty (bit 22) and context baseline compare |
| `0x8f0` | engine busy |
| `0x8f4` / `0x8fc` | IRQ status / mask |
| `0x908` | pipeline idle signature `(v & 0xfff7ffbf) == 0x63a78001` |
| `0x914` | WARP readback data port |
| `0x924` | WARP armed, `(v & 0xfff) == 0x100` |
| `0x1800 + head*0x200`, `0x1900+`, `0x1924+`, `0x19a8`, `0x1ba8`, `0x1dc4` | display |

**Reads in `0x1000..0x13ff`: zero.**  The instruction following each of the 118
reads was classified: every value is either compared against an immediate in a
poll loop, masked and written straight back, or copied to a caller's output word
for a self-test compare.  There is no `add`/`sub` into a struct field and no
two-sample differencing anywhere in the object.

The only display-side counter in the whole register map is
`0x1924 + head*0x200`, the CRTC **vertical line** counter, polled for vblank.

### 1.3 The readback port is the WARP constant file, not a statistics window

`0x0910`/`0x0938`/`0x0914` is addressed as
`0x0910 = (3 << 24) | (comp << 16) | (bank << 9) | slot`, four settling reads of
`0x0914` per component — `warp_read_constant()` in our own
`parhelia_warp.c:683`, matching the vendor's `mtx514` (`.text+0x8f7c0`).  Its
only vendor caller is `mtx612` (`0xa0120`), the `PerformCRegCheck` self-test,
which always passes `kind = 3`.  The port therefore addresses a **vertex-unit**
resource by (kind, component, bank, slot); it is not a window onto engine
status, and the kinds the vendor never uses take a different internal path and
lock the engine when swept (`parhelia-warp-readback.md`).  Nothing reachable
through it counts fragments.

### 1.4 Nothing names a counter

* `mtx_drv.so` exports 2418 **unobfuscated** C++/HAL symbols — `HSLPAR*`,
  `HWPAR*`, `CDPS*`.  Class list: `CDHMatrix CDHProcampState CDHVector
  CDPSAntiFlicker CDPSBackBuffersMgr CDPSHwManager CDPSManager CDPSPixelShader
  CDPSSurface CDPSTextureEngine CDPSTextureSetup CFeatureMgr`.  Every
  `*Count*`/`*Stat*`/`*Perf*` symbol in it is display or PLL related.
* `TEDLFCounters` is the one counter-sounding string in the whole corpus.  It is
  a **write-only** texture-engine watermark configuration register, MMIO `0x0bc0`
  (`0x001e4087` on LX/P690), written once at engine init.  It is never read.
* The 62 Linux module parameters (`kernel/src/MtxParhlParms.c`) and the ~120
  Windows registry switch names contain no counter, statistics, occlusion,
  z-pass or performance-monitor entry.
* `MTX_IOCTL_DRIVER_VALUE_REQUEST` (nr 3) has no key table: it concatenates the
  caller's string onto an empty prefix and does `mtx_find_symbol()`, then a
  single 32-bit get/set on that module variable.  The accepted key set is exactly
  the 62 parameter names.  No statistics channel exists in the ABI.
* The PInS/BIOS-driven power-on register block is `0xf00..0xff8`, 32 registers,
  all memory-controller (`pinsDatasConvertor`, `mtx_drv.so` VMA `0x34b5a0`).

### 1.5 The capability blob stops at +0x1d  [CONFIRMED — corrects `parhelia-gl-limits.md` §6.3]

The kernel's ICD-link-create handler `mtx1775` (`mtx_parhelia.o` `.text+0x1a50`)
copies the user's 56-byte packet in, fills the 40-byte caps blob at packet
`+0x10`, and copies all 56 bytes back.  Every store into the blob:

| blob off | site | value |
|---|---|---|
| `+0x00` | `0x1b50` | hardware T&L, virtual call on `*(r13+0x4d8)` |
| `+0x01` | `0x1b4c` | constant `1` |
| `+0x02` | `0x1b61` | virtual call on `*(r13+0x5c8)` |
| `+0x03` | `0x1b71` | virtual call on `*(r13+0x5c0)` |
| `+0x04`,`+0x08` | `0x1b83`/`0x1b86` | `0xC5FF0000` = −8160.0f, or 0 |
| `+0x0c`,`+0x10` | `0x1b93`/`0x1b96` | `0x45FF0000` = +8160.0f, or 0 |
| `+0x14` | `0x1ba6` | `8` |
| `+0x18` | `0x1bb0` | `0xffff` |
| `+0x1c` | `0x1b89` | `0` |
| `+0x1d` | `0x1b8d` | `0` |

**`+0x1e`, `+0x1f` and `+0x20..+0x27` are never written.**  The stack frame is
not zeroed, so those ten bytes are a verbatim echo of whatever the ICD sent —
and the ICD (`libGL.so.1.3.0` `0x358c30`) does not zero its 64-byte packet
either, initialising only `+0x00`.  So `caps+0x1e`, which `parhelia-gl-limits.md`
§6.3 lists as "line-smooth path, undetermined", is **uninitialised stack
garbage**, not a kernel-supplied capability.  No occlusion capability hides in
the unused tail, because the tail carries no kernel-authored information at all.

## 2. The Direct3D drivers refuse queries in three separate places  [CONFIRMED]

`MTXPARD.dll` is a full DX9 XDDM driver — it builds a real `D3DCAPS9`
(`0x86ca0`, 0x130 bytes at capsobj `+0x578`) and reports DDI version 4.

### 2.1 `GetDriverInfo2` answers the query enumeration with DDERR_CURRENTLYNOTAVAIL

`DdGetDriverInfo` is `0x3e770`; its GUID ladder tests 21 GUIDs whose constants
live at `.data 0x19f228..0x19f378`, 16-byte stride.  `GUID_GetDriverInfo2`
aliases `GUID_DDStereoMode` `{F828169C-A8E8-11D2-A1F2-00A0C983EAF6}`
(`0x19f238`), which is why it does not appear as a GUID of its own.  At
`0x3eb7e` the handler tests `lpvData[+4] == 0xFFFFFFFF` (`D3DGDI2_MAGIC`) and
calls the real dispatcher at `0x3e500`:

```
3e500  edi = lpvData; esi = [edi+8]        ; dwType
       dec esi; cmp esi,0x22; ja 0x3e705   ; out of range -> default
       byte index table @0x3e740 (0x23 entries) -> jump table @0x3e718 (10 entries)
```

Nine `dwType`s are handled: `GETD3DCAPS8`(1), `GETFORMATCOUNT`(2),
`GETFORMAT`(3), `DXVERSION`(4), `GETD3DCAPS9`(0x10),
`GETEXTENDEDMODECOUNT`(0x11), `GETEXTENDEDMODE`(0x12), `GETADAPTERGROUP`(0x13),
`GETDDIVERSION`(0x23, writes 4 = DX9).  Everything else takes index 9 →
`0x3e705`:

```
3e705  mov DWORD PTR [pdgdi+0x28],0x88760028   ; DDERR_CURRENTLYNOTAVAIL
```

The index table at `0x3e740` reads

```
00 01 02 03 09 09 09 09 09 09 09 09 09 09 09 04
05 06 07 09 09 09 09 09 09 09 09 09 09 09 09 09
09 09 08
```

so the entries for `GETD3DQUERYCOUNT` (0x21) and `GETD3DQUERY` (0x22) — at
`0x3e740+0x20` and `+0x21`, the `dwType` having been decremented — are both
`0x09`, the default arm.  The refusal tests no capability bit; it is
unconditional for `dwType` 5–15 and 20–34, i.e. a compile-time decision, and the
same `0x88760028` is pre-seeded at `0x3e7f2` for any unrecognised GUID.  The
`dwType` numbering is anchored independently by `GETDDIVERSION = 0x23` writing 4
and by `GETD3DCAPS9 = 0x10` copying exactly `sizeof(D3DCAPS9)`.

**The driver reports zero supported
`D3DQUERYTYPE`s**, so `D3DQUERYTYPE_OCCLUSION` (9) never appears in any list and
`IDirect3DDevice9::CreateQuery` fails in the runtime before reaching the driver.
`mtxpserd.dll` is byte-identical here (dispatcher `0x3f800`, jump table
`0x3fa18`, index table `0x3fa40`).

### 2.2 The DP2 query opcodes are no-op skip stubs

`D3dDrawPrimitives2 = 0xffd20`, installed at `0x87ad2`
(`mov DWORD PTR [ebx+0xbc],0xffd20`) inside the callbacks builder at `0x87a70`;
`D3DHAL_CALLBACKS3.dwFlags = 0xc` at `0x87ae6`, `ValidateTextureStageState =
0xffe50`.  *(This supersedes `parhelia-indexed-draws.md` §9, which recorded both
the DP2 entry point and its opcode dispatch as not found.)*

The opcode dispatch is at `0x16b681`:

```
16b681  mov  eax,[ebx+0x10]
        cmp  eax,0x100
        jge  <error>
        call DWORD PTR [eax*4+0x1efff0]
```

The 256-entry table at `.data 0x1efff0` is `rep stos`-filled by `0x16da40` /
`0x16dd60` with the default handler `0x169640`, which forwards to
`D3DParseUnknownCommandCallback` (capsobj `+0x7f0`).  The query opcodes have
entries, but they are skip stubs:

| opcode | | handler | behaviour |
|---|---|---|---|
| 84 | `D3DDP2OP_CREATEQUERY` | `0x16a4c0` | advance 8 bytes per record, zero `wStateCount`, return |
| 90 | `D3DDP2OP_DELETEQUERY` | `0x16a3e0` | advance 4 bytes per record |
| 91 | `D3DDP2OP_ISSUEQUERY` | `0x16a4c0` | advance 8 bytes per record |

The record sizes match the DDI structures exactly, which confirms the mapping.
The stubs touch no state, allocate nothing, and never emit a
`D3DDP2OP_RESPONSEQUERY` reply — a hand-crafted `ISSUEQUERY` would return no
data.  Highest opcode with a real handler is 96 `SURFACEBLT` (`0x16d7b0`).
`mtxpserd.dll` again identical (table `0x1f1ff0`, default `0x16b600`, stubs
`0x16c3a0`/`0x16c3e0`).

### 2.3 The DX8 `GetInfo` channel is a four-instruction E_NOTIMPL

`DD_MISCELLANEOUS2CALLBACKS` is built at `0x87af0`–`0x87b1b`, `dwFlags = 0xe`:
`CreateSurfaceEx = 0x100310`, `GetDriverState = 0xff990`,
`DestroyDDLocal = 0xffa40`.  `D3dGetDriverState` is the DDI behind
`IDirect3DDevice8::GetInfo` and every `D3DDEVINFOID_*` — `RESOURCEMANAGER`,
`VCACHE`, `D3DVERTEXSTATS`, `TEXTUREMANAGER`:

```
000ff990  mov  eax,DWORD PTR [esp+0x4]          ; DD_GETDRIVERSTATEDATA*
          mov  DWORD PTR [eax+0x10],0x80004001  ; ddRVal = E_NOTIMPL
          mov  eax,0x1                          ; DDHAL_DRIVER_HANDLED
          ret  0x4
```

It does not read `dwFlags`.  There is no capability bit to name: the entire
driver-state channel is refused wholesale, unconditionally.

## 3. No OpenGL ICD for the family has any query entry point  [CONFIRMED]

The Linux ICD's extension string (`libGL.so.1.3.0` `0x46e0e0`, the only such
blob in the file) lists 49 extensions and contains no `GL_HP_occlusion_test`,
`GL_NV_occlusion_query` or `GL_ARB_occlusion_query`.  `nm -D` shows no
`glGenQueries*` / `glBeginQuery*` / `glEndQuery*` / `glGetQueryObject*` under
any suffix.  There is not one `imm32` reference in `.text` to
`GL_OCCLUSION_TEST_HP` (0x8165), `GL_OCCLUSION_TEST_RESULT_HP` (0x8166),
`GL_SAMPLES_PASSED` (0x8914) or the 0x8864..0x8867 query enums — against
8 sites for `GL_DEPTH_TEST` and 20 for `GL_BLEND` as a positive control.

The **Windows** ICDs matter more, because they are far richer than the Linux one
— `MTXOGL.dll` and `mtxpsogl.dll` advertise `GL_ARB_texture_non_power_of_two`,
`GL_ARB_shadow`, `GL_ARB_depth_texture`, `GL_ARB_pbuffer`, `GL_ARB_pixel_format`
and `GL_EXT_vertex_shader`, none of which the Linux ICD exposes.  Neither
advertises any occlusion extension, and **neither contains a single query entry
point name, not even as dead code**.

The decisive comparison is in the same installer.  `MSerX/MxMGLX.dll`, which
`MSerX.inf` binds to `DEV_0540` **only** — the M-Series, a different render
engine, out of scope for this driver — advertises `GL_ARB_occlusion_query` and
contains all eight entry points (`glGenQueries`, `glBeginQuery`, `glEndQuery`,
`glGetQueryObjectuiv`, `glIsQuery`, …, each in plain and `ARB` spelling) plus the
runtime messages *"cannot use because occlusion query is active"*.  Matrox
shipped occlusion query on 0x0540 and on no Parhelia-family part, in one
release, on one disc.  (The two ICDs are separate codebases — 1.04.x versus
2.0x, and only the M-Series one carries `GLFrameBuffer::` symbols — so this is
evidence about which silicon got the feature, not about a shared driver
omission.)

## 4. Verdict

**(a) There is no pixel or fragment counter.**  No vendor binary reads one, the
entire read set of both Windows HALs and of the Linux kernel HAL is enumerated
above and is exhausted by status polls and display registers, no symbol or
string in the unobfuscated HAL names one, no capability structure reports one,
and the two Direct3D drivers contain three separate explicit refusals of the
query path.  Nothing short of a hardware scan can make this airtight, but every
instrument available to static RE points the same way.  Treat the feature as
**absent from the silicon**.

**What would close the remaining gap**, if anyone wants it airtight: read the 44
never-named registers of §1.1 (plus the unattributed parts of `0x0000..0x0fff`)
before and after a draw of exactly known pixel count — the wide-point sweep of
`parhelia-point-size-re.md` §12 already produces draws whose coverage is known
to the pixel, `(2r)^2` — and look for one that advances by that count.  Plain
BAR1 dword reads are what both Windows HALs do at arbitrary offsets, so the scan
itself is low-risk; **do not** extend it to the `0x0910` port with kinds other
than 3, which locks the engine.

## 5. If it had to be emulated anyway

Ranked by cost.  Every option pays one unavoidable price first: a query result
cannot be read before the draw that produced it has retired.  Our submission
path has 16 asynchronous slots (`PARHELIA_ASYNC_SLOTS`, `parhelia_drv.h:298`),
and `DRM_IOCTL_PARHELIA_DOWNLOAD_RENDER` calls `parhelia_submit_idle_lock()` and
then copies through a single 8 KiB aperture window, retargeting it per chunk
(`parhelia_gem.c:883`).  **One readback therefore drains all 16 slots.**  Mesa
has no other CPU read path — `ph_resource_shadow_transfer()` is the only one.

0. **Do not implement it.**  `PIPE_CAP_OCCLUSION_QUERY` is already 0 by default
   and the driver installs no query hooks, so Mesa does not expose
   `GL_ARB_occlusion_query` and applications fall back.  Cost: zero.  This is the
   recommendation.

1. **Boolean predicate, `PIPE_QUERY_OCCLUSION_PREDICATE` only, deferred.**  Draw
   the proxy with `0x12a0 = 0` replaced by red-only, `0x1038 = 0` (no depth
   write), depth test unchanged, into a scratch 32 bpp target cleared to 0, with
   `0x1328`/`0x132c` scissored to the proxy's screen-space bounding box.  Record
   the DWGSYNC tag written to `0x12bc` with the query object.  Answer
   `GL_QUERY_RESULT_AVAILABLE` by comparing `0x041c` against that tag — **no
   stall** — and only download the scissor box when the application actually asks
   for the result.  Cost: one extra pass over the proxy, plus, on the blocking
   path, one full queue drain and `4·w·h` bytes of uncached VRAM read.  A
   100×100 proxy is 40 KiB.
2. **Exact sample count, stencil.**  Same draw with stencil enabled,
   `0x10d0 = 1`, func `ALWAYS`, `0x10dc = 7` (`INCR_WRAP`, native hardware — see
   `parhelia-fragment-ops.md` §5.1), then sum the stencil bytes of the scissor
   box on the CPU.  Two blockers: the S8Z24 format code in `0x1234` is still
   **[OPEN]** (`parhelia-rendertarget.md` §2.1), and the depth surface is tiled,
   so the readback needs the tile decoder.  Overdraw beyond 255 per pixel wraps.
3. **Exact sample count, colour accumulation.**  Scratch 32 bpp target, blend
   `ADD` with `ONE/ONE` (`0x12e8`), fragment writes `1/255` in one channel, CPU
   sums.  Avoids the depth-format blocker, but saturates at 255 overdraw per
   pixel and costs the same full-box readback.
4. **GPU reduction instead of a CPU sum.**  `MAX` is a real hardware blend
   equation (`PBE_MAX = 6`, `0x12e8` bits [22:20]/[19:17]) and render-to-texture
   works, so a `log4` pyramid of textured quads can reduce the box to one texel
   and cut the readback to 4 bytes — but only for the *predicate*.  `MAX` cannot
   sum, and bilinear averaging at 8 bits per stage destroys a count long before
   the pyramid bottoms out.  Roughly 10 passes for a 1024×768 box, still one
   queue drain at the end, so it only pays off when the proxy is large.

Option 1 is the only one worth building, and only if something real needs it.
Note that its asynchronous half is free: the DWGSYNC tag mechanism maps exactly
onto `GL_QUERY_RESULT_AVAILABLE`, so a well-behaved application that polls never
stalls the engine at all.  It is `GL_QUERY_RESULT` that costs 16 slots.
