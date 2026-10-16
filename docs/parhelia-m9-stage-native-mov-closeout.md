# M9 live-stage native image and minimal-MOV closeout

This note closes two static-analysis gaps left in the M9 shader notes: how the
Linux private `0x59/0x67` route reaches compiled native instructions, and which
native form an ordinary `MOV` takes in each saved compiler.  Addresses below
refer to Linux build 98 `xserver/1_13_0/m9x_drv.so` unless named as Windows.
The conclusions were checked against all 16 saved 64-bit X-module variants and
all 16 corresponding `lib64/libGL.so.2.0.0` copies.

The reference hashes are:

| binary | SHA-256 |
| --- | --- |
| Linux `xserver/1_13_0/m9x_drv.so` | `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273` |
| Windows `MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |

## Linux stage handles reach a native descriptor

The established private route sends `0x59` stage flag zero to fragment wrapper
`m9x31318` (`0x3b8a50`) and flag one to vertex wrapper `m9x15495`
(`0x3b8d60`).  Both set request/install selector 1 and enter the parser/compiler
route through `m9x2504`.  Opcode `0x67` remains a fragment-owned cross-stage
association at backend `+0x11c`; it is not a second program upload.

The live bind setters extend that path [BIN]:

| stage | setter | current handle | lower-device tree | immediate consumer |
| --- | --- | --- | --- | --- |
| fragment | `m9x386` `0x3b8980` | backend `+0x10e` | `+0xb0` | `m9x21774` `0x431900` |
| vertex | `m9x4968` `0x3b8cd0` | backend `+0x1f1` | `+0x1d0` | `m9x21099` `0x4314f0` |

Each setter passes the unchanged handle into its consumer.  The consumer walks
the indicated ordered tree and obtains the compiled object from tree node
`+0x20`.  Its `+0x130/+0x140` descendants supply constants, resources, and
other binding metadata.

The decisive native-image join occurs in the paired pipeline-key builders
[BIN]:

```text
m9x12953
  0x3b0db2 -> m9x16389 (fragment key)
      backend +0x10e -> lower device +0xb0 tree -> node +0x20
      -> compiled object +0x128 -> m9x31492
  0x3b0e21 -> m9x17156 (vertex key)
      backend +0x1f1 -> lower device +0x1d0 tree -> node +0x20
      -> compiled object +0x128 -> m9x6969 or m9x31492
```

Compiler-result packager `m9x10572` (`0x431fe0`) allocates the compiled object,
copies the compiler result at object `+0x148`, and stores the resulting native
descriptor pointer at object `+0x128` (`0x432087`) [BIN].  `m9x31492`
(`0x421760`) treats the descriptor as native code: byte `+0x25 & 0x7f` is its
instruction count, 8-byte slots begin at `+0x28`, and the low five bits of the
slot dword are tested as operation numbers (including `0x0a` and `0x01`).
`m9x6969` handles the alternate vertex descriptor form by iterating its
subrecords [BIN].  This proves that both typed current handles reach compiled
native program representations rather than ending at source or reflection
metadata [DER].

These functions derive pipeline-key masks and hashes.  They do not populate the
final native-bind state.  The Linux native emitter `m9x11728` (`0x4b9340`) has
one direct call, at `0x4ba919` in dirty-state dispatcher `m9x26001`.  The inline
image builder `m9x24483` (`0x44ba40`) has one direct call, at `0x44c803`, in the
fixed vertex/declaration conversion route.  Neither programmable key builder
calls either function [BIN].  No shared field reference found in the 32 saved
images joins compiled object `+0x128` to emitter external controls `+0xb0/+0xd0`
or inline fields `+0xbd0/+0xbd4/+0xbd8/+0xbe0` [BIN].

Thus static linkage now ends later and more precisely.  The exact fixed-chain
constructor and binding path proves that external index `69` selects the
fixed-fragment database.  The old address-directed BAR snapshot is only an
opaque alias, with translation unresolved, so the programmable association
must be sought in the system record selected by `6c`:

```text
typed stage handle -> stage tree -> compiled object -> native descriptor +0x128
  -> [unrecovered native descriptor/resource to final bind-state association]
  -> external 6c-selected record and still-unresolved stage fields,
     or inline image and starts
```

The remaining gap is entry association and resource ownership, not compilation
or stage identity.

## Windows live ordinary MOV is always operation 0x15, mode 6

MxMD lowerer `0x1b5390` examines a normal source at `0x1b5560` [BIN]:

```text
if (ctx->field_1f4 == NULL && ctx->field_1f0 >= 0x400)
        operation = 0x14, mode = 10;
else
        operation = 0x15, mode = 6;
```

The live compile routine at `0x455c0` copies request record `+0x04` to
`ctx+0x1e8`, outer `+0x4f8` to `ctx+0x1f4`, and outer `+0x4f4` to `ctx+0x204`
at `0x4569f..0x456ca` [BIN].  Installer `0x4dfe0` copies installed object
`+0x1c` to `ctx+0x1f0` only for request kinds 2 and 5; kinds 0 and 6 allocate an
auxiliary object and seed it from `ctx+0x1f4` [BIN].

The formerly unknown producer of outer `+0x4f8` is now resolved.  Constructor
`0x44b90` stores argument `+0x10` there at `0x44c52`.  Its only direct live
caller, `0x19799`, passes the prior object at `[esi+8]` as that argument and
accepts the result only when the constructor returns success status 1 [BIN].
Initialization `0x450c0` passes outer `+0x4f8` to required helper `0x26830`; a
false return takes the failure path.  After the next required allocation,
`0x45238..0x4523e` reloads outer `+0x4f8` and unconditionally dereferences
`[pointer+0x70]`.  A null `+0x4f8` therefore cannot produce a successfully
initialized live compiler object: it either fails the helper or faults before
success [DER].

Consequently every successful live compile has non-null `ctx+0x1f4`, so its
ordinary, normal-source `MOV` takes operation `0x15`, mode 6.  The
operation-`0x14`, mode-10 arm is dead for this Windows construction path.
Request-kind-dependent `ctx+0x1f0` never changes that selection [DER].

## Linux live ordinary MOV is operation 0x14, mode 6

The Linux lowerer `m9x2880` (`0x4f7340`) kept a related but nonidentical
decision.  Its normal-source special arm requires [BIN]:

```text
ctx+0x2b0 == 0 && ctx+0x2ac > 0x3ff
```

That arm selects operation `0x14` with the distributed control tuple encoding
mode 10.  The ordinary fallback also selects operation `0x14`, but its tuple
encodes mode 6.  Helper `m9x5460` (`0x515600`) proves the operation value by
writing `argument & 0x1f` into native word `+0x0e` bits `9:5` [BIN].

Parser `m9x23695` (`0x50f250`), reached by the private stage install route,
always writes `ctx+0x2ac = 0x300` and the token's low 16 bits to `ctx+0x2b0`
at `0x50f2c1..0x50f2d7` [BIN].  Since `0x300 <= 0x3ff`, the special predicate
cannot hold for an accepted live token.  Linux therefore uses operation
`0x14`, mode 6 for a live ordinary `MOV`; it does not use the Windows
operation-`0x15` fallback [DER].  This difference must be preserved when using
the Linux compiler as the implementation reference.

## Updated smallest remaining capture

The exact `RsFragPtrsBuffer` constructor supersedes the earlier
address-directed interpretation of index `69`: it is a per-context immutable
fixed-fragment database binding, while the captured `0x1b080` BAR span is an
opaque alias rather than a linear database image or proved MMU/root structure;
translation remains unresolved [BIN/CAP/DER].  Instrument the resource
allocator and binding path, then dump compiled object `+0x128`: record
count byte `+0x25`, the 8-byte slots beginning at `+0x28`, and any alternate
vertex subrecords.  At `m9x11728` entry retain the numeric fields:

```text
+0xa0  relocation/resource descriptor for fixed-fragment DB index 69
+0xb0  external 6a control
+0xd0  external 6d control
+0xbd0/+0xbd4  inline starts
+0xbd8/+0xbe0  inline image length/pointer
```

Run two pairs: hold the vertex shader fixed while changing a fragment ALU
operation, then hold fragment fixed while changing a vertex ALU operation.
Dump the complete `6c`-selected system record and its surrounding allocation.
Those two first-draw records can identify the changing stage subrecord.  The
reconciled address evidence is in
`parhelia-m9-program-resource-reconciliation.md`.

Run `python3 tools/m9-stage-mov-boundary-evidence.py` to verify the Windows
control-flow bytes, the Linux rel32 joins, native descriptor parsers, MOV
predicate/producers, emitter boundary, and all 32 saved Linux variants.
