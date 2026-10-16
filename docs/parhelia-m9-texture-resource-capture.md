# M9 live texture and resource matrix

This experiment exercises the saved Linux M9 vendor ICD on the M9148 LP
(`102b:0540`) with direct OpenGL 2.0 rendering.  It joins visible sampling,
OpenGL errors, vendor allocations, and the exact indexed state emitted for
each target.  Three normalized phases are under
`captures/m9x-linux-texture-resources-20260914`; its self-test is
`tools/m9-texture-resource-evidence.py --self-test`.

## Visible and API results

| case | operation | live result |
|---:|---|---|
| 0 | compressed-format inventory | count reports 4, but a sentinel-filled four-entry array remains byte-for-byte unchanged |
| 1 | 13x7 NPOT, complete 13x7/6x3/3x1/1x1 chain | all four selected levels sampled correctly |
| 2 | 13x7 rectangle, unnormalized coordinates | samples correctly; level 1 is `GL_INVALID_VALUE`; repeat and mip filter are `GL_INVALID_ENUM` |
| 3 | six 8x8 cube faces with S/T gradient | every face and orientation check passes |
| 4 | 7x5x4 3-D texture | all slices and R-repeat selections pass |
| 5 | 8x8 depth texture | raw 0.25/0.75 and LEQUAL fail/pass outcomes pass |
| 6 | clamp-to-border with `(0.125,0.25,0.75,1)` | inside and outside samples pass |
| 7 | LOD bias `-2,+100,-100`, min 3, max 2 | all selected mip colors pass |
| 8 | `GL_RGB10_A2` packed upload | upload/readback calls accepted and sampling passes; packed readback differs |
| 9/10 | direct DXT1/DXT5 tokens | both rejected with `GL_INVALID_ENUM`, consistent with absent S3TC extension |
| 11 | odd 13x1 1-D texture | samples correctly; descriptor height is one |
| 12 | red upload, draw, green `glTexSubImage2D`, draw | both outcomes pass in one context; the descriptor remains stable |
| 13 | framebuffer `glCopyTexSubImage2D`, then sample | copy and sampled result pass; sequence 17 is a distinct resource-transfer stream |
| 14 | base/max-level selection | level 1 and level 2 pass and relocate from `11f81000` to `11f91000` |
| 15 | incomplete mip chain | defined white outcome passes; draw submission has no texture bind/state packet |
| 16--20 | RGB565, RGBA4, RGB5A1, RGB8, RGBA8 | uploads, exact API readbacks, and visible sampling pass |
| 21--24 | L8, A8, LA8, I8 | component expansion and visible sampling pass |
| 25/26 | D16/D32 raw sampling | both return the expected 1/17 through 16/17 gradient and visible result |

Framebuffer FNV-1a checksums are asserted by the verifier.  The vendor reports
`GL_ARB_texture_compression` but no `GL_EXT_texture_compression_s3tc`.  The
first inventory run reported count four but used a zero-initialized result
array.  The follow-up initialized all four entries to distinct `dead000N`
sentinels.  `glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, ...)` returned
`GL_NO_ERROR` and wrote none of them.  Passing each unchanged sentinel to
`glCompressedTexImage2D` produced `GL_INVALID_ENUM`.  The Linux vendor ICD has
a count/list reporting defect: there are no validated compressed-format tokens.

## Target and descriptor words

The six values below are the last writes to indices `70,78,79,7a,7c,7d` in
the selected draw.  Index `78` is the relocated texture base.  Indices `79`
and `7a` are its descriptor dimensions/format fields; `7c` contains sampler
wrap/filter state and `7d` contains LOD controls.

| target/use | `70` | `78` | `79` | `7a` | `7c` | `7d` |
|---|---:|---:|---:|---:|---:|---:|
| NPOT level 0, 13x7 | `000c0118` | `11f81000` | `c801000d` | `00010007` | `00009200` | `00000000` |
| NPOT level 1, 6x3 | `000c0118` | `11f91000` | `c8010006` | `00010003` | `00009200` | `00000000` |
| NPOT level 2, 3x1 | `000c0118` | `11f91000` | `c8010003` | `00010001` | `00009200` | `00000000` |
| NPOT level 3, 1x1 | `000c0118` | `11f91000` | `c8010001` | `00010001` | `00009200` | `00000000` |
| rectangle 13x7 | `00040118` | `11f81000` | `c801000d` | `00010007` | `12409200` | `0ff80000` |
| cube 8x8 | `00040318` | `11f81000` | `c8090008` | `00010008` | `02409200` | `0ff80000` |
| volume 7x5x4 | `00040218` | `11f81000` | `c8110007` | `00040005` | `02409200` | `0ff80000` |
| depth raw 8x8 | `00040118` | `11f81000` | `fa010008` | `00010008` | `00009200` | `0ff80000` |
| depth compare | `14040218` | `11f81000` | `fa010008` | `00010008` | `00009200` | `0ff80000` |

The target dimension changes the combined operation/resource word at `70` and
the layer/depth field at `7a`: cube uses `79=c809...`, while the 3-D texture
uses `79=c811...` and places four in `7a[31:16]`.  Enabling depth compare changes
`70` from `00040118` to `14040218` without changing the depth resource
descriptor.  These are implementation-ready live joins for target selection
and shadow compare.

The live words also close two static format joins.  `c8 >> 1` is hardware code
`0x64`, which the saved format table assigns to `A8B8G8R8`/the byte-compatible
32-bit RGB family.  An OpenGL `GL_RGBA/GL_UNSIGNED_BYTE` array is RGBA in
increasing little-endian addresses, so this code is consistent with that
physical byte order.  `fa >> 1` is `0x7d`, exactly the statically recovered D24
family.  In every sampled case `79[15:0]` is the exact logical width and
`7a[15:0]` the exact logical height, including the odd NPOT levels.  Cube adds
`0x00080000` in `79`; volume adds `0x00100000` and puts its depth in
`7a[31:16]`.  The static notes name the two descriptor outputs `idx78/idx79`
before the relocation is prepended; on the final indexed stream they appear at
hardware indices `79/7a` because index `78` carries the resource address.

Border mode emits `7c=0fc09200` plus
`7e=40002000, 7f=ff00bf00, 80=ffffffff, 81=00000000`.  The requested normalized
RGBA border was `(0.125,0.25,0.75,1)`; the four packed words must be copied as
the measured hardware representation until their conversion is independently
decoded.

For a 64x64 complete mip chain, index `7d` was:

| request | `7d` |
|---|---:|
| LOD bias -2 | `0ff803c0` |
| LOD bias +100, saturated | `0ff801ff` |
| LOD bias -100, saturated | `0ff80200` |
| min LOD 3, max LOD 4 | `04018000` |
| max LOD 2 | `02000000` |

This validates the static signed ten-bit bias and two nine-bit bound fields in
`parhelia-m9-texture-state.md` on live hardware.

## Allocation classes

Every ordinary resource used allocation type `0x10`, placement 4.  Rectangle
alone used type `0x11`, placement 4.  Small 2-D, rectangle, depth, border, and
RGB10_A2 requests received an `0x800` byte allocation aligned to `0x80`.
The 7x5x4 volume received `0x2000`.  Cube and 64x64 mip-chain resources each
received `0x2c0000` aligned to `0x16000`.  These unexpectedly large allocations
are vendor layout policy and should not be copied into a new allocator without
mapped-content evidence.

The odd NPOT updates show a further implementation boundary.  Level zero used
base `0x11f81000`; each later `glTexImage2D` call obtained type `0x10`, base
`0x11f91000`, size `0x800`, and the selected descriptor dimensions shrank for
the requested base level.  The capture does not yet show whether those repeated
responses refer to one reused backing or replacement allocations.

## RGB10_A2 precision boundary

The exact standardized API upload words, in texel order, were:

```text
021fa011 456ee44e 88be288b cc0d6cc8
0f5cb105 52abf542 95fb397f d94a7dbc
1c99c1f9 5fe90636 a3384a73 e6878eb0
29d6d2ed 6d26172a b0755b67 f3c49fa4
```

Their byte FNV-1a is `3dde8c3c`.  `glGetTexImage` returned successfully but
produced hash `155aedce`, and the rendered sample matched the requested color.
The emitted sample descriptor is exactly the same as the ordinary RGBA8 4x4
case: `70=00040118, 79=c8010004, 7a=00010004, 7c=00009200`; its allocation is
also the same type, size, and alignment.  This proves that this vendor OpenGL
path aliases the sample resource to its ordinary 8-bit hardware format.  It
does not by itself name the internal byte/channel order.  The packed
`glGetTexImage` values are an API conversion result and cannot be used as a
raw-storage dump.  The focused follow-up preserves both those returned dwords
and the VRAM bytes so the conversion and hardware order can be described
separately.

The saved hashes are sufficient to identify the API quantization exactly.  Of
the standard truncate/round/bit-replicate models, only round-to-nearest from
10 to 8 bits followed by round-to-nearest from 8 to 10 reproduces
`155aedce`; A2 is unchanged.  That model yields:

```text
020f9c10 454eec4c 88ce2c8c cc1d6cc9
0f5ca905 529bf941 95db397d d95a79bd
1c99c9f9 5fd90a36 a3284a72 e6a786ae
29e6d6ee 6d26172a b0655767 f3a4a7a3
```

The verifier regenerates these words from the exact upload and asserts both
hashes.  They describe the observable `glGetTexImage` conversion.  The focused
run reports actual internal format `GL_RGBA8` (`0x8058`), so the request is
explicitly downgraded rather than sampled from native 10-bit storage.  Its
RGBA byte readback hash is `3ac4ea5d`.

## Format descriptors and allocation sizing

All phase-three format draws use `70=00040118`, base `78=11f81000`, logical
size 4x4, sampler `7c=00009200`, and LOD `7d=0ff80000`, except that D16 adds
`7c[31]`.  Their measured index-79 high bytes are:

| API internal format | `79` | hardware code `79 >> 25` | allocation size/alignment |
|---|---:|---:|---:|
| RGB565 | `9d010004` | `4e` | `400/40` |
| RGBA4 | `8c010004` | `46` | `400/40` |
| RGB5A1 | `94010004` | `4a` | `400/40` |
| RGB8 | `c9010004` | `64` | `800/80` |
| RGBA8 | `c8010004` | `64` | `800/80` |
| L8 | `61010004` | `30` | `200/20` |
| A8 | `66010004` | `33` | `200/20` |
| LA8 | `a2010004` | `51` | `400/40` |
| I8 | `a2010004` | `51` | `400/40` |
| D16 | `b8010004` | `5c` | `400/40` |
| D32 | `f8010004` | `7c` | `800/80` |

This validates every static format-code prior.  Bit 24 distinguishes RGB from
RGBA for the shared 32-bit code and similarly appears for RGB565 and L8.
Intensity uses the LA8 storage code and relies on component routing.  D16 and
D32 are distinct hardware families even though this input gradient produces
identical API readback and framebuffer hashes.

The object at index `78` begins with the following 0x80-byte record, repeated
to fill each small allocation:

```text
0000: 11ee0008 11ee1008 00000000 ...
0020: 11f88008 11f89008 11f8a008 ...
```

This is repeated PTE-shaped allocation/translation metadata, not linear
pixels.  Static analysis now proves the matching 40-bit PTE grammar: bit 3 is
valid, bits 31:12 supply physical bits 31:12, and bits 11:4 supply physical
bits 39:32.  The 0x800-byte images are nevertheless identical across
RGB10_A2, updates, copy, and narrow formats while visible pixels and
descriptors differ.  The capture therefore does not prove that this particular
record is the texture's page-table root or that its decoded pages own the
texture.  Join the request root and decoded physical pages to coordinate-coded
texels before assigning that role.  The exact boundary and all class-3 address
calculators are recorded in `parhelia-m9-backing-layout-closure.md`.

## Update, copy, and mip-selection streams

The same-context subimage draws are sequences 12 and 14.  Both emit an 8x8
RGBA8 bind at `11f81000`, and both visible outcomes pass.  The update occurs
outside those selected draw submissions; the resource object is stable before
and after each draw.  This proves the vendor stack orders the update before
reuse, although the selected snapshot does not expose the texel upload or its
actual backing allocation.

Framebuffer-to-texture copy sequence 17 is an 0x11a4-byte, two-range transfer
stream.  It programs destination `b0=11f81000`, `b1=c8010020`, then sequence
18 samples a 16x16 RGBA8 descriptor at the same resource address.  The result
checksum `c6909dc5` passes.  This is an implementation basis for a GPU copy
path and its ordering point.

Base/max selection sequence 12 binds level 1 as 4x4 at `11f81000` with
`7d=01000000`; sequence 16 binds level 2 as 2x2 at `11f91000` with `7d=0`.
Their linked-resource snapshots differ in 85 bytes from offsets `606` through
`73a`.  The incomplete-chain case produces white but its submission contains
only fence/progress writes and no index `70` or `78` bind.

## Capture boundaries

Every selected draw retains the complete 0x21000-byte system-resource mapping,
the active BM image, ExecuteDma input, allocation tuples, and an image at the
index-78 resource address.  The cube sequence additionally retains a complete
0x2c0000-byte range.  Both long snapshots completed, but the 30-second tracer
timeout killed that process before its result line was flushed; an independent
map-only run produced the expected visible checksum.  The LOD long capture
completed normally.  Changes in their large post-draw ranges overlap other
backing/render allocations and are not evidence of a linear texture layout.

One earlier X session ended when the controlling `startx` SSH shell received
SIGHUP.  Its log says `xinit: unexpected signal 1`; there was no kernel or GPU
fault.  Starting through `nohup setsid` keeps the reconstructed vendor session
independent of SSH.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
