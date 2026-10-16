# M9 first-triangle system constant constructor

This note closes the initialized bytes selected by indices `8d/8e` in the
bounded 320x240 fixed-function triangle. It also separates those ranges from
the selector-`6c` record, which carries a second viewport/depth prefix.
`[CAP]` denotes saved vendor capture evidence, `[BIN]` a pinned vendor binary,
and `[DER]` a value determined from that evidence and the specified OpenGL
transform.

Reproduce the hashes, decode the selected ranges, and generate the final image
with:

```sh
python3 tools/m9-first-triangle-system-evidence.py
```

The checker reads saved files only. It does not execute vendor code, access a
DRM node, or submit GPU commands.

## Why zero is not a valid image

The captured first triangle binds these end-exclusive system ranges:

```text
preprocess  8d=system+0x10|1  8e=system+0xc0|1
raster      8d=system+0x00|1  8e=system+0x10|1
```

The corresponding vendor allocations contain fixed-function program
constants. They are read-only for the saved submissions: all mapped images
before and after submission are byte-identical. A clean implementation must
therefore initialize bytes `[0,0xc0)` before the draw; an all-zero allocation
sends zero transform and viewport inputs to the selected programs `[CAP]`.

The baseline source is:

```text
captures/m9x-linux-fixed-state-20260914/pairs/case-37/
  draw-a-resource.bin
SHA-256 a6977921a784564b81ae3f41db6300e0316a203ec38ba2a1a0839b2e148586da
first 0xc0 SHA-256
  45262f42ea5b7506b839d97a22f5596aca2f4a98d1296796e34b5df62415ad1f
```

That fixture uses a 256x256 drawable, `glViewport(0,0,256,256)`,
`glDepthRange(0,1)`, and `glOrtho(0,256,0,256,-1,1)` `[CAP]`.

## Append-selected viewport proof

The system allocation is append-only during the controlled state pairs. The
first `0xc0` bytes remain the old A record after state B is compiled, so
examining only the allocation prefix gives a stale result. In extended case
67, setup B writes these selectors:

```text
8d: 40c3a611 40c3a681 40c3a681 40c3a601
8e: 40c3a681 40c3a681 40c3a6e9 40c3a611
6c: 40c3aa01 repeated four times
```

The active B constants therefore begin at system offset `0x600`; the linked
record is independently selected at `0xa00`. The pinned files are:

```text
extended/case-67/setup-b-bm.bin
  548078032b846d750c39f9676f0c980fa4e69444a505c2d378f25102e085f8b7
extended/case-67/setup-b-execute.bin
  a128c11f42d50ddfeddb317313caacf2ee72ca782e908e4320f153240a0fcede
extended/case-67/draw-b-resource.bin
  9ee562e534516030879985889dcde42716103e5db854b36b2096a1327f69b5f5
selected [0x600,0x6c0) SHA-256
  267910a22340dfb6695f6a0faa6ddc44ff787a13144be1b51ae1097069c11dec
```

Case 67 changes only the viewport to `(13,19,171,149)`. Its selected record
contains these values relative to the record:

```text
+0x80  { 85.5, -74.5, 0.5, 1.0 }
+0x90  { 98.5, 162.5, 0.5, 149.0 }
+0xb0  { 13.0, 88.0, 1.0, 0.5 }
```

For drawable height `D`, viewport `(x,y,w,h)`, and depth range `(n,f)`, these
three rows prove the constructor `[CAP][DER]`:

```text
+0x80 = { w/2, -h/2, (f-n)/2, 1 }
+0x90 = { x+w/2, D-y-h/2, (f+n)/2, h }
+0xb0 = { x, D-y-h, 1, 0.5 }
```

Extended case 68 independently changes only `glDepthRange(.25,.75)`. Its
selected record changes `+0x88` from `0.5` to `0.25` while `+0x98` remains
`0.5`, confirming the depth terms. The full resource and selected-record
hashes are:

```text
a21ca0d7816d2d6ff88139ce254ff85d778e9e0d2236ed7471049d1b3a7f6b8d
02a7b776cdcb8099a6c74cff19c1692178235959731935c403ad33177f8bd5fc
```

The projection terms at `+0x24` and `+0x38` are `2/W` and `2/H`. The baseline
contains `2/256` in both positions. A second, nonsquare vendor packet fixes
which coefficient is x and which is y rather than relying on that square
baseline.

## Direct 320x240 transform and viewport packet

The older vendor live-ring capture uses the same
`glOrtho(0,320,0,240,-1,1)` and full-window viewport as `tools/gltri.c`.
Section `19dc0daa` contains an eight-row MVP/normal block at constant slot zero
and a two-row viewport block at slot `0x160` `[CAP]`:

```text
captures/live-ring-0528/secs.txt
SHA-256 ad91fff9110e313928c23b3e313f6f4cf3564ce88946fc968bf5322402b7201a
section 19dc0daa, 400 dwords, packed SHA-256
  339bf844349d73dab4b512e6709793f79d42919668acffb9e4c9c95410770d7f

c[0]   3bcccccd 00000000 00000000 bf800000
c[1]   00000000 3c088889 00000000 bf800000
c[2]   00000000 00000000 bf800000 00000000
c[3]   00000000 00000000 00000000 3f800000
c[160] 43200000 c2f00000 3f000000 3f800000
c[161] 43200000 42f00000 3f000000 43700000
```

Thus the vendor's x row directly supplies `0x3bcccccd = 2/320`, its y row
directly supplies `0x3c088889 = 2/240`, and its viewport supplies
`{160,-120,.5,1}` / `{160,120,.5,240}`. Joining those rows to the M9 record
layout fixes `+0x24` to the x coefficient, `+0x38` to the y coefficient, and
`+0x80..+0x9f` to the two viewport rows. This join is checked word for word by
the evidence tool `[CAP][DER]`.

The arithmetic is also present in two saved common-driver implementations.
Linux `mtx2926` converts width and height, multiplies them by `+0.5` and
`-0.5`, adds the viewport origins, copies the depth terms, and marks constant
slots `0x160/0x161`. The corresponding Windows routine performs the same
operations and marks the same slots `[BIN]`:

```text
matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
  SHA-256 5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47
  mtx2926 .text+0x87220, 1005-byte SHA-256
    abac0e058e48515f0d29138ec113b8eeee375fa66a821ba43448649618b313ad
xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
  SHA-256 9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf
  viewport prefix VA 0x119290..0x11944a SHA-256
    d1cbec39f9c80259d6af601a811bf64b07b947c89ffa2167f484126b1c301a4c
```

The other `-1`, identity, clamp, and guard-band words remain invariant in the
M9 records `[CAP]`.

## Exact 320x240 image

For a 320x240 drawable, full viewport, depth range `(0,1)`, and
`glOrtho(0,320,0,240,-1,1)`, bytes `[0,0xc0)` are the following little-endian
u32 words:

```text
00: 00000000 3f800000 3f800000 3f000000
10: 00000000 bf800000 00000000 3f800000
20: 00000000 3bcccccd 00000000 3f800000
30: 00000000 bf800000 3c088889 00000000
40: 00000000 00000000 bf800000 00000000
50: 00000000 00000000 00000000 00000000
60: 00000000 3f800000 00000000 3f800000
70: 00000000 00000000 3f800000 00000000
80: 43200000 c2f00000 3f000000 3f800000
90: 43200000 42f00000 3f000000 43700000
a0: 00000000 40000000 c67f8000 467f7c00
b0: 00000000 00000000 3f800000 3f000000
```

The exact hashes are:

```text
SHA-256  feafcc5aa2c71a45a09616f3625190b3afe6753ba526f2f822ebd3fbe3dabd46
FNV-1a64 ab2075c3793cfdc5
```

Initialize these bytes once after allocating the system BO and before
submitting the first draw. The raster pass reads the first 16 bytes and
preprocessing reads the following 176 bytes. The
selector-`6c` record may share the allocation, but it is a distinct object at
the selected aligned offset and must be constructed separately.

The original 320x240 wrapper did not save the system mapping, so the final
M9 allocation itself is not a direct 320x240 byte snapshot. Its invariant
words and destination layout are pinned by M9 captures; its variable viewport
and depth layout is checked against controlled M9 appended records; and every
320x240-specific matrix and viewport word is present directly in the vendor
320x240 packet above. The checker also parses the current
`m9_triangle_system_prefix`, compares all 48 dwords with this image, verifies
its FNV self-test value, and confirms the prefix is copied into the system BO.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
