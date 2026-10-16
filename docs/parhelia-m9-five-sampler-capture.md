# M9 five dependent fragment samplers

The Linux vendor ICD advertises 16 fragment and 20 combined texture image
units.  Earlier captures exposed four complete hardware texture-state slots at
indices `70..73`, leaving unclear whether a fifth sampler would be rejected,
aliased, or compiled into additional work.  The bounded live fixture
`tools/m9x-five-sampler-probe.c` distinguishes those outcomes.

The GLSL 1.10 fragment shader declares five unique `sampler2D` uniforms.  Each
of five 2x2 textures has a different solid sentinel value.  Sample B depends
on A's result for coordinates, C on B, D on C, and E on D.  The final color
uses channels from A through E, so optimizing away or aliasing any sampler
changes the expected `903040ff` output.

On the M9148 vendor stack:

```text
limits fragment-samplers=16 combined-samplers=20
vertex compile=1
fragment compile=1
link=1
sampler locations=0,1,2,3,4; units=0,1,2,3,4
pixel=903040ff frame=41031dc5 error=0x0
RESULT=PASS
```

## Hardware schedule

The selected draw is sequence 25 on `bm2`, with three ranges:

```text
0x000..0x260  setup/preprocessing
0x260..0xfa4  four scratch RT passes plus the final RT pass
0xfa4..0xfd0  completion
```

The five nonzero index-70 control words are:

```text
00040118 0004111c 0004111c 0004111c 0004111c
```

The matching index-78 texture resources are:

```text
11f01000 11f09000 11f11000 11f19000 11f21000
```

Each is a separate type-0x10, placement-4, 0x800-byte allocation aligned to
0x80.  Every block emits `79=c8010002`, `7a=00010002`, `7c=00009200`, and
`7d=0ff80000`, describing a 2x2 RGBA8 nearest-sampled resource.  Indices
`71..73` remain zero throughout.  The texture routing sequence at index 74 is:

```text
0000010c 00000100 00ff0110 00ff0910 00ff0510 00ff0d10
```

Later passes also update indices 75 and 76 and bind intermediate descriptors
at index 78 from `10ec1000`.  All six program/state blocks select the same
linked record `6c=40c3a281`.  The complete 0x21000-byte resource image is
unchanged across submission.

**Update (2026-09-17).**  This is the *dependent* case.  With independent
fetches the compiler packs four into a single pass using all four slots
`70..73`, and a continuation pass carries three plus the previous pass's
scratch result, so `N` independent fetches cost one pass for `N <= 4` and
`ceil(N/3)` above that.  The one-pass-per-sampler behaviour below is a data
dependency, not a slot limit.  Measured to sixteen samplers and to 49 taps of
one sampler in `parhelia-m9-openbits-gl-capture.md`.

The vendor compiler therefore implements this five-dependent-sampler shader
as framebuffer multipass inside one ExecuteDma submission.  Passes 0--3 write
four rotating views of scratch surface `0x10ec1000`; the final pass samples
three scratch views plus the fifth application texture and writes the user
target.  All application textures time-slice full slot `70`, while generic
routes `74..76` carry scratch feedback.  The fifth logical sampler works, but
it does not expose a fifth simultaneous hardware state slot.  A new compiler
can initially cap native single-pass texture scheduling at the recovered
physical limit and add this lowering later.  Advertising more units requires
equivalent scratch allocation/lifetime plus correct depth, blend, stencil,
discard, query, MRT and MSAA behavior.  The exact schedule is in
`parhelia-m9-five-sampler-schedule-re.md`.

The compact evidence and verifier are under
`captures/m9x-linux-five-sampler-20260914` and
`tools/m9-five-sampler-evidence.py`.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
