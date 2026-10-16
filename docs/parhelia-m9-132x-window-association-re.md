# M9148 conditional `132x` range association

This note closes the conditional range-publication boundary left open by the
engine-to-execution audit. It joins the independent Linux and Windows vendor
implementations to the range allocator, while keeping that internal allocator
separate from the saved first-triangle render-resource domains. Recheck every
conclusion with:

```sh
python3 tools/m9-132x-window-association-evidence.py
```

The verifier only reads saved files. It never opens a PCI or DRM device.

## Result

The two triples are not two copies of one VRAM aperture and they are not a
choice between system memory and VRAM. They publish two entries of one
allocator family:

| role | direct MMIO family | indexed publication | presence |
| --- | --- | --- | --- |
| selected primary range | `132c,1330,1334` | `cb,cc,cd` with header `45cdcccb` | always |
| selected companion range | `1320,1324,1328` | `c8,c9,ca` with header `45cac9c8` | only when companion size is nonzero |

Each emitted triple is exactly `{base, base, base + size - 0x1000}`. The limit
is the address of the last 4-KiB page, inclusive. A zero companion size means
no companion publication at all. The vendor does not write zeroes to the
companion family in that case.

For the saved Linux M9148 descriptor, program the selected primary manager
range directly:

```text
132c = 00801000
1330 = 00801000
1334 = 00860000
```

The inputs are descriptor `C+0x18=0x00801000` and
`C+0x1c=0x00060000`, constructed by `m9x858` and copied into the selected
manager child by `m9x53`. Leave `1320/1324/1328` untouched because
`C+0x24=0`. The separate `0x10000000..0x1fffffff` allocator contains local
render resources; no instruction in this chain substitutes it for the
selected manager child.

## Exact range inputs

Linux proprietary-core `m9x421` at `.text+0x16410` receives manager `M` in
its first argument. It obtains the primary input as [BIN]:

```c
unsigned i = M->u32[0x30 / 4];
u32 main_base = M->u32[(i + 2) * 8 / 4];     /* M + 0x10 + 8*i */
u32 main_size = M->u32[(i + 2) * 8 / 4 + 1]; /* M + 0x14 + 8*i */
```

At `0x16656..0x1672d` it emits the primary indexed form. At
`0x16a32..0x16a8c` it writes the primary direct-MMIO form. Both use the same
base and size.

The optional input is [BIN]:

```c
unsigned j = M->u32[0x58 / 4];
u32 companion_base = M->u32[(j + 6) * 8 / 4 + 2]; /* M + 0x38 + 8*j */
u32 companion_size = M->u32[(j + 6) * 8 / 4 + 3]; /* M + 0x3c + 8*j */
```

Both implementations test `companion_size`, not the base or selector. Linux
does so at `0x1673a..0x16742` and `0x16a98..0x16aa0`. Windows `MxMD.dll` uses
the 32-bit equivalents `manager+0x28+8*j` and `manager+0x2c+8*j`, with the
selector at `manager+0x40`, at VAs `0x5ab27..0x5ab61` and
`0x5abe4..0x5ac57` [BIN].

The Windows primary offsets are `manager+0x0c+8*i` and
`manager+0x10+8*i`, with selector `manager+0x24`. The different byte offsets
come from the 32-bit object layout. Its formulas, condition, register order,
and packet headers are otherwise instruction-for-instruction equivalents of
the 64-bit Linux implementation [BIN].

## Where the ranges come from

`m9x53` constructs the Linux manager from descriptor `C`. Let
`N=C->u32[0x4c/4]`. If `N` is nonzero, the primary ranges are [BIN]:

```c
main_size = floor(C->u32[0x1c/4] / N);
main[k].base = C->u32[0x18/4] + k * main_size;
main[k].size = main_size;
```

The companion allocator exists only when both `N` and `C+0x24` are nonzero:

```c
after_main = C->u32[0x18/4] + N * main_size;
companion_size = floor(C->u32[0x24/4] / N);
companion[k].base = after_main + k * companion_size;
companion[k].size = companion_size;
```

This exact construction at `m9x53+0x4f1..+0x67a` resolves the important
association: the optional family is a contiguous extension following every
primary partition. On the saved target `N=1`, primary base is `0x00801000`,
primary size is `0x00060000`, and companion size is zero. Therefore the
selected child remains `0x00801000..0x00860fff`; neither captured address
domain supplies this initialization tuple [BIN][DER].

`m9x92` constructs this manager only when `System\\MmFlags` bit 3 is set. Both
saved defaults set that bit, so this manager is present on the tested profile:
Linux uses `0x000100e8`, while Windows uses `0x000101f8` [BIN]. Presence of the
manager does not imply presence of its optional companion range.

## Direct versus indexed publication

The publication transport is a separate predicate. With a command builder
available, both vendor implementations use indexed packets exactly when
[BIN]:

```c
indexed = (MmFlags & BIT(9)) ||
          ((MmFlags & BIT(3)) && (MmFlags & BIT(24)));
```

Without a command builder, Linux falls directly into the MMIO path. The saved
Linux default `0x100e8` and Windows default `0x101f8` have bit 3 set and bits 9
and 24 clear. They therefore select direct MMIO. The implementation rule for
the tested M9148 is:

```c
write32(mmio + 0x132c, main_base);
write32(mmio + 0x1330, main_base);
write32(mmio + 0x1334, main_base + main_size - 0x1000);

if (companion_size != 0) {
        write32(mmio + 0x1320, companion_base);
        write32(mmio + 0x1324, companion_base);
        write32(mmio + 0x1328,
                companion_base + companion_size - 0x1000);
}
```

For another configuration, use the predicate rather than publishing through
both transports. The indexed headers encode the same MMIO indices because
`0x1000 + 4*0xcb == 0x132c` and `0x1000 + 4*0xc8 == 0x1320`. They are an
alternate transport, not shadow registers requiring duplicate programming.

These addresses are reused by ordinary 2D/3D state later: captured draws use
index `ca`/MMIO `1328` and index `cb`/MMIO `132c` for scissor state. The range
publication is an initialization transition before the vendor's later mode
and state packets. It is not evidence that `132x` remains a permanent MMU
range while a triangle runs. A clean-room implementation may issue the
captured later scissor writes after completing this publication transition.

## Join to the other `m9x421` windows

The primary range is positively joined to a second register family in the
same function. `m9x421` reloads the identical `M+0x30` selected base and size
at `0x168ac..0x16905` and writes [BIN]:

```text
f74 = main_base
f78 = main_base
f7c = main_base + main_size - 1000
```

Thus `f74/f78/f7c` and the primary `132c/1330/1334` publication describe the
same selected range. The companion range has no corresponding `f74` mirror.

The other `m9x421` ranges come from different descriptor fields and must stay
separate [BIN]:

| registers | manager group | original descriptor input |
| --- | --- | --- |
| `360/364` base and limit-plus-offset | `M+0x70` plus selected offset at `M+0x98` | `C+0x28/+0x2c`, offset range from `C+0x30/+0x34` |
| `f64/f68` base and inclusive limit | `M+0xc0` | `C+0x38/+0x3c` |
| `f74/f78/f7c` | primary group at `M+0x10` | `C+0x18/+0x1c`, partitioned by `C+0x4c` |

Do not populate `360/364` or `f64/f68` from the local-VRAM primary tuple just
because a previous broad recipe used identical values. Their descriptor roles
are independent and require the actual first-job inputs recovered for those
groups.

## Separation from first-triangle resources

The saved M9148 allocation capture contains two disjoint GPU domains [CAP]:

```text
placement 4 / local VRAM:  10000000..1fffffff
placement 1 / system:      40000000..4fffffff
```

Every captured selector-8 color target is local. The window color target is
`0x11ee9000`; the FBO target is `0x11f41000`. Depth, textures, and the private
BE/scratch surface used by the first-triangle path are likewise local resources
in this separate render-resource allocator [CAP].

The command pages, fences, canonical vertex arena (`0x40f52000`), and linked
program/resource arena (`0x40c3a000`) are system resources. They retain their
captured low address tags and resource-specific requester bindings. `m9x421`
also emits the separate indexed transition `e6=0x40000000`, but it never feeds
those per-job system addresses or the local `0x1xxxxxxx` resource addresses
into either conditional `132x` range [BIN][CAP].

The implementation boundary for the first flat/Gouraud triangle is therefore:

1. validate and pin all local color/depth/texture/BE resources inside
   `0x10000000..0x1fffffff`;
2. validate system command, vertex, linked-record, and fence resources in the
   `0x40000000..0x4fffffff` domain;
3. during `m9x421` initialization, publish the internal
   `0x00801000..0x00860fff` primary tuple at `132c/1330/1334` and its
   identical `f74/f78/f7c` tuple;
4. set `companion_size=0` and leave `1320/1324/1328` untouched because no
   nonzero companion extent is validated for the clean-room M9148 profile;
5. take the distinct `360/364` and `f64/f68` inputs from their other descriptor
   fields; do not derive them from first-job render resources;
6. continue with the already recovered requester arm, sequence 0, current
   context record, and draw state.

This supersedes the unconditional six-register table in
`parhelia-m9-vram-render-target-join.md` and the provisional “separate
capability/resource branches” wording in
`parhelia-m9-engine-execution-gate-re.md`. The static evidence now identifies
the exact capability predicate, resource presence predicate, range inputs,
transport, and primary/companion association.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
