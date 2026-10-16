# M9 native control-flow lowering: static closure delta

> **Not on the driver's path.**  Neither native backend emits a branch.  The
> control flow the benchmarks actually reach the driver with -- an `if` on a
> uniform, a loop whose bound is a uniform -- is closed by specialising the
> program on the value the draw carries, which folds or unrolls it to one
> block (`parhelia-m9-uniform-specialisation.md`).  What follows is the
> recovered vendor-compiler closure, kept for the day a shader needs a branch
> that no uniform decides.

This note records the exact control-flow joins recovered from the saved Windows
Direct3D compiler and keeps them separate from the still-unknown external
linked-resource layout.  Addresses are virtual addresses in `MxMD.dll`.
`[BIN]` denotes a direct instruction/data observation and `[DER]` a conclusion
from those observations.

The pinned binary is
`xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll`, SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
`tools/m9-native-control-flow-evidence.py` verifies the binary identity, rel32
joins, expanded-operation tables, and packed slot templates below without
loading or executing vendor code.

## Three representations must remain distinct

The compiler uses three related representations [BIN][DER]:

1. D3D token lowerers build graph/scope nodes and lists.
2. Control builders create 0x20-byte fixup objects and 0x44-byte native
   instruction objects.  The fixup objects carry typed descriptors and must not
   be decoded with the native instruction packer.
3. Packer `0x1db840` turns each 0x44-byte instruction object into two dwords.

This explains why graph fields, `ctx+0x59c` serials, and native slot bits do not
form one fixed record header.  It also prevents the compiler's branch-fixup
serial from being mistaken for a hardware program counter.

## Exact D3D control-flow joins

The parser/lowerer calls establish these paths [BIN]:

| D3D construct | lowerer | native/control builder | exact join |
|---|---:|---:|---:|
| `REP` | `0x1a7970` | `0x1ba2c0` | `0x1a7aae` |
| `LOOP` | `0x1a7f20` | `0x1ba2c0` | `0x1a805e` |
| `ENDLOOP` | `0x1a80f0` | `0x1baa20` | `0x1a817c` |
| `ENDLOOP`, optional close path | `0x1a80f0` | `0x1bac40` | `0x1a81fe` |
| `ELSE` | `0x1a7780` | `0x1bbad0` | `0x1a780e` |
| `BREAK` | `0x1a8230` | `0x1bbd30` | `0x1a8651` |
| `BREAKP` | `0x1a8230` | `0x1bc0d0` | `0x1a82f3` |
| `BREAKC` | `0x1a8230` | `0x1bc4c0` | `0x1a8638` |

`REP` and `LOOP` use different scope maps but the same builder.  `REP` uses
scope serials at `ctx+0x2c0/+0x2c4` and map `ctx+0x274`; `LOOP` uses
`ctx+0x314/+0x318` and map `ctx+0x2c8`.  Both node constructors retain the
scope ID at node `+0x1c`, a register-derived 16-bit value at `+0x24`, two
context flags at `+0x18/+0x20`, and an optional predecessor value at `+0x28`
[BIN].  These are compiler graph fields, not native start/count fields [DER].

`ENDLOOP` checks node type 2, calls `0x1baa20` with node `+0x24`, pops the
current graph object through `0x1a2010`, finds the enclosing type-2 node, and
updates `ctx+0x314`.  The optional path passes the saved outer flag plus node
`+0x24/+0x28` and the node itself to `0x1bac40` [BIN].  This is the recovered
compiler join for a loop backedge, but the final physical target patch remains
inside fixup processing [DER].

The conditional builder dispatch is also exact.  `0x1a8230` sends opcode
`0x2c` to the `BREAK` builder, opcode `0x2d` to `BREAKC`, and opcode `0x60`
to `BREAKP`.  A break outside active `REP` or `LOOP` scope does not reach these
builders [BIN].

## CALL, LABEL, RET, and CALLNZ are graph operations

`LABEL` lowerer `0x1a7bd0` accepts `label+1 < 256`, uses `ctx+0x224` as its
label map with key storage at `ctx+0x270`, records the key at the mapped
object's `+0x1c`, and attaches that object through `0x1a1240` [BIN].

`CALL` lowerer `0x1a7ce0` applies the same `label+1 < 256` rule and looks up or
creates the object in `ctx+0x224`.  It appends the referenced object from map
entry `+0x10` to the current list, then initializes that object's `+0x18` only
if it was zero [BIN].  `RET` lowerer `0x1a7b40` validates the active mapped
object against the current graph object and pops it through `0x1a2010` [BIN].
None of these three lowerers calls the native instruction setter or packer
directly [BIN].  A CALL target is therefore a graph edge at this boundary, not
an immediate native address [DER].

`CALLNZ` has no separate native operation.  Lowerer `0x1a7e50` creates a
temporary conditional token with opcode `0x28`, copies the source descriptor,
then calls [BIN]:

```text
0x1a7ebb -> IF     (0x1a7040)
0x1a7ed6 -> CALL   (0x1a7ce0)
0x1a7edf -> ENDIF  (0x1a76a0)
```

Thus `CALLNZ(p, label)` is an exact compiler macro expansion to
`IF(p); CALL(label); ENDIF` [DER].  A clean compiler does not need a guessed
standalone CALLNZ native opcode.

## Recovered expanded native operations

Native instruction constructor `0x1da2c0` initializes a 0x44-byte object.
Setter `0x1dbe50` writes the operation field in object dword `+0x08` bits
25:21.  Setter `0x1dc040` writes its low-five-bit operation field to dword
`+0x08` bits 18:14 and uses table-selected modifier bits to preserve expanded
operation identities [BIN].

For the control operations used here, its table resolves as follows [BIN]:

| expanded operation | low field | modifier action |
|---:|---:|---|
| `0x81` | 1 | no modifier |
| `0x82` | 2 | clear dword `+0x08` bit 26 |
| `0x83` | 3 | clear bit 26 |
| `0x182` | 2 | set bit 26 |
| `0x183` | 3 | set bit 26 |

The numeric operation is reconstructed exactly from the setter and its inverse
decoder.  Its semantic name is constrained by its caller, rather than proved
as a global ISA mnemonic: for example, `0x183` is the operation chosen by the
unconditional BREAK builder, but the number may be reused by another control
template [DER].

The recovered builder sites are [BIN]:

| builder use | operation load | setter call |
|---|---:|---:|
| optional ENDLOOP control slot | `0x1bae3a`: `0x81` | `0x1bae3f` |
| ELSE slot 0 | `0x1bbb92`: `0x81` | `0x1bbb97` |
| ELSE slot 1 | `0x1bbc3f`: `0x83` | `0x1bbc44` |
| BREAK first slot | `0x1bbe43`: `0x183` | `0x1bbe48` |
| BREAKP first slot | `0x1bc243`: `0x182` | `0x1bc248` |

## Exact fixed slot templates

Applying packer `0x1db840` to the final object fields written by ELSE builder
`0x1bbad0` gives [BIN][DER]:

```text
ELSE slot 0: W0=fffffc1c W1=80004c1f   (expanded operation 0x81)
ELSE slot 1: W0=9ff4ffdc W1=9000cc1f   (expanded operation 0x83)
```

The helper tests its second argument and emits these only when that flag is
nonzero.  Lowerer `0x1a7780` reaches it only on the `ctx+0x428 != 0` path
[BIN].  The pair is therefore a proved conditional codegen template, not a
claim that every ELSE token always adds two hardware slots [DER].

Unconditional BREAK builder `0x1bbd30` always constructs its first native
object before testing the optional follow-up flag.  The final object has
expanded operation `0x183` and packs to [BIN][DER]:

```text
BREAK slot 0: W0=ffffffbc W1=8c00cc1f
```

`BREAKP` builder `0x1bc0d0` maps replicated condition selector bytes
`00/55/aa/ff` to condition indices 0/1/2/3.  The predicate descriptor built by
the lowerer has class `0x6`, selecting the builder's bit-13 form.  Its first
native slot is therefore the following exact family [BIN][DER]:

```text
condition 0: W0=ffffff1c W1=8400ac1f
condition 1: W0=ffffff3c W1=8400ac1f
condition 2: W0=ffffff5c W1=8400ac1f
condition 3: W0=ffffff7c W1=8400ac1f
```

These templates deliberately exclude subsequent typed-control objects and any
address-dependent companion slot.  In `0x1bbd30`, a nonzero follow-up flag
increments `ctx+0x59c`, forms descriptor `0xb4000000 + old_serial`, stores it
in a 0x20-byte fixup object at `+0x1c`, and references it from the following
native object.  `BREAKP` has the parallel mechanism [BIN].  This proves a
typed relocation identity and lifetime join.  Resolver/serializer recovery in
`parhelia-m9-shader-transport-static-closure.md` now proves that the marker is
replaced by an ordered resolved descriptor before packing.  The serializer
does no PC-relative arithmetic on the low serial [BIN].

The physical replacement producer is now joined in
`parhelia-m9-linked-pair-target-placement-re.md`. Compiler pass `0x1d1d40`
pairs every source descriptor with the output of converter `0x1df900`, then
publishes the pair as dwords 0 and 1 of the 0x1c-byte mapping entry [BIN]. The
`94`, `b4` and `c4` families allocate finite selectors through descending
`+0x20`, four-slot `+4` and 23-slot `+8` namespaces respectively. Direct
serializer `0x1dc8b0` inserts their resolved low selector bits into
`W1[20:15]`, `W0[5:0]` and `W1[13:9]` according to the object gates [BIN].

## Relationship to program start, stop, and resource lifetime

The new control templates close native operation bits for selected branches;
they do not change the established final-program boundaries:

* `docs/parhelia-m9-program-stage-entry-re.md` proves two 16-bit starts in the
  fixed-chain inline image and the `69/6a/6d` bind path.  Those starts belong
  only to that inline path; the external programmable `6c` path has no public
  per-stage entry or count fields.
* `docs/parhelia-m9-shader-termination-boundary.md` proves the common one-output
  terminal slot `fffc7e1d a2803f0f` and that neither compiler slot count nor
  aligned upload byte count is sent as an execution bound.  Static evidence
  still cannot distinguish endpoint-selector termination from full terminal
  operation semantics.
* `docs/parhelia-m9-linked-record-grammar-re.md` proves that external selector
  `6c` addresses a heterogeneous, 0x80-aligned linked render-resource record.
  The BO and referenced resources must survive until the submission fence;
  the record is an immutable whole-pair selection replaced atomically on state
  change.  No universal record-relative code start/count/stage split exists in
  the reachable static serializer.

Accordingly, the known fixed slots are enough to reproduce these compiler
fragments, but not enough to submit an arbitrary branch graph through selector
`6c`.  The unresolved implementation fields are [DER]:

1. the frontend semantic rules that choose every source anchor and typed
   descriptor family for arbitrary D3D forward branches, backedges, CALL,
   BREAK and nesting. The structural multi-root scheduler, reconvergence
   release, interval bounds, finite allocation and retry behavior are now
   exact in `parhelia-m9-cfg-linked-image-re.md`;
2. the opaque final byte join that combines completed native images and
   resources inside a new pair.  Private 0x3158-byte allocation, keyed cache
   reuse, >1023-entry reclamation and COW publication are exact, as is one
   coherent 0x100-byte image at linked object `+0xd9`;
3. endpoint-only versus full-operation hardware stop semantics.  Result-only,
   kill-only and combined finalizer tails are exact and have no MRT-index
   input.

Do not encode speculative PC-relative fields from the compiler serial.  A
from-scratch driver can safely use the exact fixed templates only with a
recovered or independently designed fixup/linker path that produces a bounded
native image and retains every referenced BO through completion.

## Reproduction

From the repository root:

```sh
python3 tools/m9-native-control-flow-evidence.py
```

The verifier uses only Python's standard library.  It checks the pinned DLL
hash, all eight direct builder joins, the CALLNZ macro calls, expanded-operation
table actions, and the three fixed templates plus four BREAKP variants.
`python3 tools/m9-shader-transport-closure-evidence.py` additionally checks the
typed resolver, one-slot/four-slot serializer footprints, and inline bind.
