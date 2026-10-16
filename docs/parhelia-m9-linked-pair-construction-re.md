# M9 linked-pair construction, cache, and special native forms

This note continues the selector-`6c` investigation inside the pinned Linux
render driver and executes a small audited portion of the pinned Windows
shader compiler.  It closes allocation, cache reuse, copy-on-write publication,
KIL packing, a texture-special basis, sampler comparison fields, and all three
finalizer variants.  It does not invent a universal internal layout for the
heterogeneous record selected by `6c`.

`tools/m9-linked-pair-constructor-evidence.py` verifies every address and
native transcript below.  Its inputs are Linux `m9x_drv.so` SHA-256
`3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`
and Windows `MxMD.dll` SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.

## Whole-pipeline construction is private and commit-like

Linux cache orchestrator `m9x24062` at `0x452c90` first computes the desired
key, compares it with the active object's saved key, then searches an ordered
tree.  Tree comparisons use the dword at node `+0x18`; a hit obtains the
pipeline object from node `+0x20`.  This is associative key lookup, not an
allocation-ring cursor [BIN].

A miss calls `m9x21798` at `0x451a00`.  That routine increments owner
generation `+0x48`, requests exactly `0x3158` bytes, calls base constructor
`m9x5982`, then initializes ownership and generation with `m9x12124`.  The new
object is not active yet.  `m9x10394` copies desired state into it.  The clone
copies exactly `0x169` bytes at outer `+0x130`, ending at the distinct
outer-`+0x138` linked-object pointer, and calls the dirty reset afterward.
Linked resources are consequently rebuilt/reference-managed rather than
blindly copied [BIN][DER].

`m9x1587` validates/builds four private subobjects.  Its first call passes
pipeline `+0x150` to state compiler `m9x18211`; subsequent calls cover the
other large state blocks.  Only after every call succeeds does it collect
resource dependencies and clear the private dirty heads at outer
`+0x130/+0x138/+0x140/+0x148`.  Failure destroys the candidate and clears the
active pointer and saved key words [BIN].

Tree insertion `m9x19459` allocates a `0x30`-byte red-black node.  It stores
the parent at `+0x08`, lookup key at `+0x18`, pipeline pointer at `+0x20`, and
color at `+0x28`, then links/rebalances it and increments the tree's 64-bit
count.  Since the tree begins at owner `+0x70`, that count is owner `+0xf0`.
There is no finite linked-record ring or modulo wrap in this cache.  Before
creating a miss candidate, the orchestrator compares the count against
`0x3ff`; above that threshold it obtains the leftmost node's `+0x20` pipeline
and invokes virtual method `+0x08`.  This proves ordered reclamation after
1023 entries.  Whether the virtual call destroys immediately or releases a
reference is not visible at this call site [BIN][DER].

After candidate validation, `m9x15674` rebuilds two dependent resource trees,
then writes the active pointer at owner `+0x20` and snapshots the key and six
generation words at `+0x4c..+0x64`.  Only then do the final state promoters
run.  `m9x21983` transfers the two nested resource pairs, including the pair
ultimately selected through `6c`.  This is a concrete copy-on-write rule:
build and validate a private whole-pipeline object, insert/cache it by key,
publish it atomically, then promote its linked resources [BIN][DER].

A clean driver can copy this lifetime policy without duplicating the vendor's
tree: immutable linked-pair objects keyed by complete shader/fixed-state input,
private construction, publication after successful validation, and fence-held
references.  Key reuse replaces the previously proposed allocator-wrap rule.
The `0x3ff` maintenance callback must not be reinterpreted as a record-ring
wrap.

## Exact KIL packing

MxMD KIL lowerer `0x1c82d0` consumes IR in `ESI` and compiler context in
`EDI`.  It splits the source mask into `xy` and `zw`.  For each active pair it
constructs operation `0x58`, calls the CMP lowerer at `0x1c8e90`, and
increments context kill counter `+0x5a4`.  Its control descriptor is
`0x2000f9c2` for the first pair and `0x0044fff9` after a prior pair.  The
component selectors are `44/55` for `xy` and `aa/ee/ff` for `zw` [BIN].

The hash-pinned executable harness produces this complete nonempty pair basis
[RUN]:

| mask | packed slots `W0:W1` |
|---:|---|
| `x` | `e021f95c:b370800f` |
| `y` | `e021f95c:b37080cf` |
| `xy` | `e021f95c:b370804f` |
| `z` | `e061f95c:b370800f` |
| `w` | `e061f95c:b37080cf` |
| `zw` | `e061f95c:b370804f` |
| `xyzw` | `e021f95c:b370804f`, `3e618c5c:bf400140`, `ffc6791c:a2803f1f` |

This closes the canonical packed discard form.  Conditional/predicate
composition outside this basis still has to use the recovered comparison
control rules and should remain rejected if the exact combination is absent
from the native lowering matrix.

## Texture modes and metadata-special expansions

The common lowerer at `0x1bda50` receives one-hot mode arguments for TEXLDB,
TEXLDP, TEXLDL, and TEXLDD.  Under the minimal zero-resource fixture, ordinary,
bias, projection, and explicit-LOD modes all produce the same three local
slots:

```text
ffff8000:0000000f
ffc0001c:a2803f0f
ffc0011c:a2803f0f
```

This is a useful negative result.  The one-hot API arguments are exact, but B,
P, and L are not recoverable from these local slots; their distinguishing
coordinate/resource state lives elsewhere in the linked pipeline.  They must
not be represented by guessed bits in the three-slot sequence.

TEXLDD is locally distinct and expands to nine exact slots [RUN]:

```text
e004021d:b05a800f  ffff801f:0000000f
e0040e1d:b05a803f  ffff801f:0000000f
ffca0a1c:a2803f1f  ffc00c1c:a2803f0f
ffff8000:0000000f  ffce001c:a2803f0f  ffce011c:a2803f0f
```

The lowerer indexes an eight-byte sampler metadata record by IR `+0x20`.  It
extracts bits `28:19` as a count clamped to at least three.  Independent low
metadata flags `1,2,4,8,10,20,40,80,100,4000` each have deterministic native
expansions in the evidence tool.  Their internal semantic names are not
present in the binary, so the verifier records the exact words without naming
them as projection, compare, normalization, or format conversion.  This
ten-flag basis is safe for differential compiler testing; it is not yet a
Gallium texture-mode enum.

Shadow comparison is separate final sampler state.  Sampler `+0x46` bit 8 and
sampler `+0x08` bit 13 set backend texture word bit 26.  Compare functions map
to bits `29:27` exactly [BIN]:

```text
NEVER LESS EQUAL LEQUAL GREATER NOTEQUAL GEQUAL ALWAYS
    0    1     3      2       6        4     5      7
```

## The three terminal forms are result/kill combinations

Finalizer `0x1c8590` tests context `+0x5a4`, the counter incremented by KIL,
and `+0x5a8`, incremented when frontend lowering encounters the special
result descriptor `0x0000fff8`.  Its three paths are [BIN][RUN]:

| condition | endpoint IR | packed terminal |
|---|---|---|
| result only: `+5a4 == 0`, `+5a8 > 0` | MOV `fff8 -> 3001fffe` | `fffc7e1d:a2803f0f` |
| kill only: `+5a4 > 0`, `+5a8 == 0` | ADD/bridge to `3002fffe` | `3ffcfe1e:ac800c3f` |
| kill plus result: both positive | combine, MOV `44fff8 -> 3003fffe` | `3ffcf81e:ac800c3f`, `fffc7e1c:a2803f1f` |

The executable transcript runs all three complete finalizer paths; the full
finalizer hash pins the preceding selection logic.  These suffixes
do not encode MRT target 0 through 3.  The finalizer reads only the two
boolean-positive counters, has no render-target loop index or declaration-slot
input, and collapses all positive counts.  Native MRT export remains a separate
open join.

## Control-flow placement boundary

The earlier control-flow and transport work remains the exact construction
contract: builders allocate typed fixup objects, serials name ordered
descriptor entries, the resolver substitutes resolved descriptors before
packing, and the serializer performs no PC-relative arithmetic.  The cache
path above shows where the completed pipeline is privately validated and
published. `parhelia-m9-cfg-linked-image-re.md` now closes the graph's
multi-root scheduling, reconvergence release, interval bounds, physical
selector retirement/quarantine and reset. It also recovers the exact
zero-first 0x100-byte host image at linked object `+0xd9`. The remaining
control-flow problem is the frontend semantic choice of typed descriptors,
while the remaining selector-`6c` problem is the final byte join from host
state and resource objects into the 0x80-aligned GPU allocation. Both must be
implemented and tested before arbitrary graph submission is enabled.

Run the deterministic checker from the repository root:

```sh
python3 tools/m9-linked-pair-constructor-evidence.py
```

It builds the minimal 32-bit harness and invokes it through `qemu-i386`; it
does not touch the kernel, Mesa, or a live GPU.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
