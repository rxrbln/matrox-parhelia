# M9 five-sampler schedule and scratch-pass contract

This note closes the packet architecture exposed by the normalized
five-dependent-sampler capture. It distinguishes exact capture facts
(`[CAP]`), static binary facts (`[BIN]`), deterministic joins (`[DER]`), and
remaining hypotheses. Run the reproducer from the repository root:

```sh
python3 tools/m9-five-sampler-schedule-evidence.py
```

The checker hash-pins the compact capture, Windows `MxMD.dll`, and Linux
`m9x_drv.so`; decodes the complete ExecuteDma stream; verifies all pass
boundaries, resources, route records, constant ranges, render state, and
completion; and pins the producer/serializer instructions cited below.

## Result

The vendor does **framebuffer/scratch render multipass inside one DMA
submission**. It is not merely a sequence of texture instructions retaining
all intermediate values in fragment registers. Four raster draws bind
`0x10ec1000` as RT0 through four rotating internal-format views. Each following
draw samples the immediately preceding view. A fifth raster draw binds the
application color target, samples three views of the scratch surface, binds
the fifth application texture, and produces the exact expected pixel. Every
block contains complete raster state, generated vertex inputs, a constant
range, texture state, program/resource state, and an H03 draw `[0,3)` [CAP].

The fixture is a dependency chain:

```glsl
a = texture2D(s0, uv);
b = texture2D(s1, uv + a.rg * 0.03125);
c = texture2D(s2, uv + b.gb * 0.03125);
d = texture2D(s3, uv + c.br * 0.03125);
e = texture2D(s4, uv + d.ra * 0.03125);
gl_FragColor = vec4(a.r + d.r, b.g + e.g, c.b, 1.0);
```

All five 2x2 RGBA8 textures have distinct solid sentinels. The direct vendor
context reports fragment/combined sampler limits 16/20 and returns
`903040ff`, full-frame FNV-1a `41031dc5`, with `GL_NO_ERROR` [CAP]. Thus the
fifth sampler is neither rejected nor aliased.

## Submission and exact block boundaries

ExecuteDma sequence 25 on `bm2` has three logical ranges [CAP]:

```text
0000..0260  one setup/preprocessing draw [0,4)
0260..0fa4  five texture/raster blocks
0fa4..0fd0  completion
```

Accounting for each twelve-byte H03 draw record, the five blocks are [CAP]:

| pass | block bytes | draw record | logical texture | output |
|---:|---:|---:|---:|---|
| 0 | `260..528` | `51c`, `[0,3)` | `11f01000` | scratch view `ee190010` |
| 1 | `528..7e0` | `7d4`, `[0,3)` | `11f09000` | scratch view `ee190810` |
| 2 | `7e0..a58` | `a4c`, `[0,3)` | `11f11000` | scratch view `ee190410` |
| 3 | `a58..cd0` | `cc4`, `[0,3)` | `11f19000` | scratch view `ee190c10` |
| 4 | `cd0..fa0` | `f94`, `[0,3)` | `11f21000` | user RT `11ee9000/cc010040` |

The setup draw is at `25c`, `[0,4)`. This is the ordinary M9 vertex
preprocessing/raster distinction plus an additional five-way fragment
schedule: one preprocess draw feeds five full raster draws [CAP][DER].

All five raster blocks use the same 64x64 clip:

```text
f5=00000000 f6=00400040 c4=00000000 c5=00400040
```

The first four use the same generated input descriptors. The final block
changes the input selection because it consumes the accumulated scratch
results. `c2=11f29000` remains constant while `c3` advances
`0,5,0b,11,17`. These are exact record selectors; their unit is not named by
the current static join [CAP].

## One physical full sampler is time-sliced

Every pass uses full hardware slot zero at index `70`; indices `71..73` are
explicitly zero. The control and resource sequence is [CAP]:

| pass | index `70` | `78` application root | `79` | `7a` | `7c` | `7d` |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | `00040118` | `11f01000` | `c8010002` | `00010002` | `00009200` | `0ff80000` |
| 1 | `0004111c` | `11f09000` | same | same | same | same |
| 2 | `0004111c` | `11f11000` | same | same | same | same |
| 3 | `0004111c` | `11f19000` | same | same | same | same |
| 4 | `0004111c` | `11f21000` | same | same | same | same |

Both index-70 values set bits 3, 4, 8, and 18. Passes 1--4 additionally set
bits 2 and 12; their exact delta is `0x1004`. The statically recovered
operation/resource-dimension field `70[11:8]` is 1 in both forms, the ordinary
2D operation family. The meanings of bits 2, 12, and 18 are not statically
closed. A clean implementation must preserve the two captured words for this
schedule and must not label the `0x1004` delta as a cache, dependency, or LOD
mode without an independent differential [CAP][BIN].

The five application resources are independent type-`0x10`, placement-4,
`0x800`-byte allocations aligned to `0x80`. Their common descriptors are 2x2
RGBA8 nearest-sampled state. This establishes descriptor rebinding on every
pass rather than a fifth physical state register [CAP].

## Scratch result backing and route records

Passes 0--3 bind the same address as RT0 with four descriptor values [CAP]:

```text
b0=10ec1000
b1=ee190010, ee190810, ee190410, ee190c10
```

For each later pass, descriptor-only index `74` samples the immediately
preceding view at the same address [CAP]:

```text
pass 1: 74=00ff0110  78=10ec1000 79=ee090010
pass 2: 74=00ff0910  78=10ec1000 79=ee090810
pass 3: 74=00ff0510  78=10ec1000 79=ee090410
pass 4: 74=00ff0d10  78=10ec1000 79=ee090c10
```

Each producer/next-consumer descriptor pair differs by exactly bit 20
(`0x00100000`). That numeric relationship is exact. Bit 20 is not safely named
as a read/write bit: the final pass also samples two `ee19...` descriptor
forms through descriptor-only indices 75 and 76 [CAP][DER].

The complete route sequence, including the setup draw, is [CAP]:

```text
index 74: 0000010c, 00000100, 00ff0110, 00ff0910,
          00ff0510, 00ff0d10
```

The final pass adds [CAP]:

```text
75=0706030c  78=10ec1000 79=ee190210
76=0f0e0b0c  78=10ec1000 79=ee190a10
77=00000100
```

Earlier passes set `75=76=77=00000100`. The final pass therefore has three
descriptor-only scratch inputs through 74--76 plus application texture 4
through full slot zero. The route words' little-endian byte sequences are
`10 0d ff 00`, `0c 03 06 07`, and `0c 0b 0e 0f`; these exact maps must be
treated as opaque until a route-component differential assigns source and
channel names [CAP].

All scratch descriptors have format field `descriptor[31:25]=0x77`, an
internal code not present among the ordinary public render formats. The
address does not appear in the captured process allocation results, so the
scratch surface predates this fixture and belongs to vendor workspace. Its
allocation extent, alignment, physical layout, reuse rule, and simultaneous
context ownership are not in this capture. A new driver must allocate and
fence an equivalent private surface; it must not assume that address or infer
an extent from the 64x64 draw alone [CAP].

The cross-pass value carrier is therefore the scratch backing. Native
temporaries may still hold values within each individual pass, but no
register-only interpretation can explain four RT0 writes followed by sampled
reads of the same address [DER].

## Constant slices and the shared `6c` record

The five raster blocks bind adjacent ranges in the system allocation
`0x40c3a000` [CAP]:

| pass | `8c` | `8d` | `8e` | saved contents |
|---:|---:|---:|---:|---|
| 0 | `00500100` | `40c3a001` | `40c3a009` | two `0.03125f` |
| 1 | `00500100` | `40c3a009` | `40c3a011` | two `0.03125f` |
| 2 | `00500100` | `40c3a011` | `40c3a019` | two `0.03125f` |
| 3 | `00500100` | `40c3a019` | `40c3a021` | two `0.03125f` |
| 4 | `00500300` | `40c3a021` | `40c3a039` | `1,1,0,1,1,0.5` |

This matches the four coordinate perturbations and final color expression.
As in the uniform captures, `8c/8d/8e` are constant-range selection. The low
address tag and the low `8c` fields remain unnamed [CAP][DER].

Every setup/raster block binds exactly [CAP]:

```text
69=11e42000 6a=25010000 68=00000000 6b=00001030 6c=40c3a281
```

Clearing the proved low tag from `6c` selects system-resource offset `0x280`.
Its first eight dwords are [CAP]:

```text
42000000 c2000000 3f000000 00000000
42000000 42000000 3f000000 00000000
```

They decode to `(32,-32,0.5,0)` and `(32,32,0.5,0)`, geometry/setup data. The
next `0x60` bytes through offset `0x300` are zero. The entire 0x21000-byte
resource is byte-identical before and after ExecuteDma, proving CPU-side
construction before submission [CAP].

No pass boundary, per-pass entry, texture descriptor, or route word changes
inside this selected `6c` record. `6c` is a shared linked render-resource
binding here, not the five-pass selector. Pass identity is carried by the
serialized command blocks, constant ranges, scratch routes, generated-input
selector, and texture rebinding [CAP][DER]. The record may own additional
dependencies outside the visible prefix in other shaders; this one sparse
instance cannot define a universal record extent or grammar.

## Ordering and completion

The five blocks emit index-`43` dependency values [CAP]:

```text
0450, 0470, 0490, 04b0, 04d0
```

They appear before their respective draws and advance monotonically by
`0x20`. Existing static analysis proves index `43` is the maximum class-1
render-producer dependency requested by sampled resources. The same-address
FBO capture separately proves render-to-texture visibility without an extra
texture-specific cache packet. For this schedule, no additional candidate
invalidate occurs between the scratch producer and consumer [BIN][CAP][DER].

The footer writes the second fence address through index `1b`, emits
`e6=3,e6=2,e3=000004f1`, and restores `43=000004f0`. Submission order, the
resource dependency, and final coherent completion must remain in one kernel
job. All five application textures, the scratch surface, generated-input
resource, depth/auxiliary surfaces, user target, system constants, and linked
record must remain resident until that completion retires [CAP][DER].

## Depth, blending, coverage, and side effects

This fixture has depth testing disabled. Pass 0 emits
`a8=0038038a`; passes 1--4 emit `a8=00380388`. Static lowering assigns `a8`
bit 0 to depth-test enable and bit 1 to depth-write enable, so the scheduler
preserves the original write bit only on the first raster pass and clears it
on every later pass [BIN][CAP]. All five passes bind the same depth
`11ef1000/fa010040` and auxiliary `11ef9000/f0010040` resources.

That is an exact rule for the captured depth-disabled state. It does not prove
how an enabled LESS/EQUAL test is transformed after pass 0 writes depth. A
driver must not enable general multipass depth from this one state: repeated
LESS draws after the first update would reject the surviving fragments unless
the compiler changes comparison/coverage routing.

Other exact deltas are [CAP]:

```text
passes 0..3: a9=88ffff00      final: a9=08ffff00
pass 0:      ae=32444442      later: ae=22444442
passes 0..3: user RT unbound  final: b0/b1=11ee9000/cc010040
```

Passes 2 and 3 inherit `ab/ac/ae` from pass 1; the final pass re-emits
`ab=646cffff, ac=00002b0e, ae=22444442`. These are useful exact state
requirements, but their coverage/output subfields are not all named.

The capture used ordinary replacement color, no query, no discard, no
fragment depth output, and one render target. It therefore does not prove the
following semantic rules:

* blending must read the user destination and execute exactly once on the
  final target pass; the vendor's blend path is native program generation, so
  copying blend fragments into scratch passes would be incorrect;
* depth and stencil tests/updates must preserve one logical-fragment result,
  with updates occurring at most once;
* discard in an early texture phase must suppress every downstream scratch
  write and the final result;
* implicit derivatives and LOD must be computed from the original quad before
  values cross a scratch boundary; solid 2x2 textures cannot validate this;
* an occlusion query must count application samples/fragments once rather than
  count all five raster draws;
* MRT outputs and color masks must apply to final application targets only;
* multisample coverage/sample identity must survive the scratch format.

These are correctness gates, not evidence against the mechanism. They explain
why reproducing the five packet blocks alone is insufficient for a general
GLSL lowering pass.

## Static producer and serializer boundaries

Linux GL stage transport is exact [BIN]:

```text
m9x23579  0x6af060  emits private opcode 0x59 and stage flag
  -> m9x18200 0x5b6690
     -> fragment m9x31318 0x3b8a50
        -> compiler/install 0x4360c0 using device+0x2560
```

The fragment current handle later reaches `m9x386 0x3b8980`, tree lookup
`m9x21774 0x431900`, compiled-object packager `m9x10572 0x431fe0`, and native
descriptor consumer `m9x31492 0x421760`. Paired-pipeline key construction is
owned by `m9x12953 0x3b03f0`; its fragment key call at `0x3b0db2` enters
`m9x16389 0x3c4560` [BIN].

Final Linux state dispatch is separately exact [BIN]:

```text
m9x15197 0x4b6c40
  -> m9x25088 0x889a60
     -> m9x26001 0x4ba3a0
        -> m9x2427  0x4b99d0  (68/6b/6c resource group)
        -> m9x11728 0x4b9340  (69/6a/6d or inline program group)
```

Static ownership still ends between compiled pipeline/native descriptors and
the private object which expands one logical draw into the five final raster
blocks. No direct call or uniquely typed record joins that scheduler producer
to the leaves above. The Linux configuration constructor `m9x10575 0x32ee60`
registers `AllowMultipass` at `0x32f30f`, passes value/default 1, and stores
the option object at configuration `+0x60`; its duplicate is `m9x19713
0x331040`. A consumer of that field has not been joined to this schedule.
Windows registers the similarly named option with default zero. The name is
corroboration, not the missing call edge [BIN].

The Windows final serializer does close the hardware record shapes [BIN]:

```text
0xfadf0 draw serializer
  0xfae0e -> 0xfb920  surfaces
  0xfae25 -> 0xf4f50  common/texture state
  0xfae32 -> 0xf6f20  program/resource state
  0xfae3f -> 0xfb260  generated inputs
```

Within `0xf4f50`, full count `+0x380` and pointers `+0x384..+0x390` feed
`0xf4350` at call site `0xf55e0`. `0xf4350` takes the unit from backend
`+0x0c`, emits `70+unit` from `+0x10`, and can emit the complete
address/descriptor/filter/LOD state. Descriptor-only count `+0x394` and
pointers from `+0x398` are serialized first through `0xf4630` at `0xf5532`.

The descriptor-only record used by `0xf4630` is now exact [BIN]:

| record field | final use |
|---:|---|
| `+0x00` | dirty word, cleared after emission |
| `+0x08` bit 1 | resource-reference registration |
| `+0x0c` | unit, selecting `74+unit` at `0xf46d6` |
| `+0x10` | route/control word emitted at `74+unit` |
| `+0x14/+0x18` | tracked/relocated backing pair |
| `+0x1c` | descriptor word emitted at index `79` |

When `+0x10 & 3` is zero, it emits the relocated index-78 root and index-79
descriptor pair at `0xf472c..0xf475e`; otherwise it can emit index 79 alone.
This corrects the narrower interpretation that 74--77 are exclusively vertex
texture entries. They are descriptor/route operands and the Linux fragment
multipass scheduler demonstrably uses them for scratch feedback. Their lack of
full sampler controls remains unchanged [BIN][CAP][DER].

## Limits and implementation rule

Linux linker/resource validator `m9x20565 0x6bbb40` checks at most four vertex
samplers at `0x6bbb87` and at most sixteen fragment samplers at `0x6bbbd8`.
The context advertises 20 combined units [BIN][CAP]. These are logical compiler
limits. The final packet architecture has four full slots `70..73` plus four
descriptor-only operands `74..77`; this capture uses only full slot zero and
three descriptor-only scratch operands in its last pass.

The implementation rule supported now is:

1. A native phase may bind at most four complete sampler states. Partition a
   shader only where all live cross-phase values and coverage can be preserved.
2. For this exact five-dependent chain, emit one preprocessing draw and five
   raster phases in dependency order.
3. In phases 0--3, bind one logical texture through full slot zero, bind the
   corresponding constant slice, route the prior result when present, and
   write the captured rotating view of a fenced private `0x77` scratch surface.
4. In phase 4, bind the user target, route the three captured scratch views
   through 74--76, bind logical texture 4 through slot zero, disable depth
   update as captured, and draw once.
5. Emit the maximum scratch producer dependency through 43 before each
   consumer and retain every referenced backing through final completion.
6. Apply API-visible color/depth/stencil/query/MRT side effects once according
   to the gates above. Reject a shader when the compiler cannot prove this.

The capture proves scheduling capacity **at least five**, not sixteen. It does
not prove scratch capacity, compiler success, register pressure, or semantic
correctness for N=6..16. A new driver should expose four fragment samplers
until it has general lowering; exposing five is justified only for a compiler
which implements and tests this exact class; exposing sixteen requires
validated schedules through the logical boundary. The 20 combined cap also
requires a VS4+FS16 coexistence test, not addition of packet slot counts.

## Minimal remaining live capture

No further static scan can assign the scratch allocator or side-effect rules
from this one state. The smallest final capture set is one process and one
normalized submission per case:

1. dependent and independent chains at N=`4,5,8,16,17`, plus VS4+FS16;
2. five samplers with depth test/write off, test on/write off, and test/write
   on, saving color and depth before/after;
3. five samplers with destination-dependent blend, stencil increment,
   conditional discard, `gl_FragDepth`, an occlusion query, and a mipmapped
   gradient texture using `dFdx/dFdy`;
4. RT0 and two-target MRT variants with distinct sentinels, stopping before
   the already known unsafe vendor teardown path;
5. tracing from ICD/X initialization early enough to capture allocation and
   retirement of the surface which later appears at `0x10ec1000`.

Save every intermediate scratch view and the full command/resource images.
The decisive comparison is whether only the first or final phase owns each
API side effect and how surviving coverage is represented. Until that matrix
exists, multipass is suitable for an experimental compiler gate, not a default
claim of all 16 fragment samplers.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
