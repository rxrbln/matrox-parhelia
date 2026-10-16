# M9 native lowerer execution corpus

This note records a controlled execution of the saved Windows M9 native
lowerers.  It complements the static packer analysis: the static code proves
each field and call path, while this corpus gives exact packed slots for
specific compiler inputs without requiring an M9 board or an MMIO capture.

## Isolation and pinned input

`tools/m9-native-lower-run.py` accepts only the saved `MxMD.dll` with SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
It builds a non-PIE i386 helper that maps the PE at its preferred base, supplies
the allocator vtable used by the audited compiler functions, and constructs
the exact nested output-vector ownership chain at compiler context `+0x41c`.
The helper exposes only these audited entry points [BIN][RUN]:

| source operation | lowerer | calling convention | packed slots |
|---:|---:|---|---:|
| MOV `1` | `0x1b5390` | two stack arguments | 2 |
| ADD `2`, SUB `3` | `0x1c4c40` | two stack arguments | 6 |
| MAD `4` | `0x1c53e0` | two stack arguments | 8 |
| MUL `5` | `0x1c50e0` | two stack arguments | 6 |
| RCP `6` | `0x1b0840` | context in `ecx`, IR on stack | 6 |
| RSQ `7` | `0x1b1210` | context in `ecx`, IR on stack | 7 |
| DP3 `8` | `0x1c5a00` | two stack arguments | 8 |
| DP4 `9` | `0x1c6160` | two stack arguments | 8 |
| MIN `10`, MAX `11` | `0x1c4d50` | two stack arguments | 6 |
| EXP `14` | `0x1b1ba0` | context in `ecx`, IR on stack | 6 |
| LOG `15` | `0x1b2550` | two stack arguments | 8 |
| Iris TEXLD / D3D operation `66` | `0x1bda50` | eight stack arguments | 3 |
| Iris CMP / D3D operation `88` | `0x1c8e90` | two stack arguments | 8 |
| Iris DDX / D3D operation `91` | `0x1c9300` | two stack arguments | 4 |
| Iris DDY / D3D operation `92` | `0x1c9300` | two stack arguments | 4 |
| Iris SETP / D3D operation `94` | `0x1c9430` | two stack arguments | 6 |
| Iris FLOOR `0x8003` | `0x1c96d0` | three stack arguments, third zero | 6 |

The arithmetic entries are the operation-family wrappers called after the
front end has selected a source operation.  Static audit shows `ret 8` for all
of them, with `(context, IR)` on the stack.  They perform alias checks and
temporary allocation before calling the native builders.  The FLOOR wrapper
has `ret 0xc`, takes `(context, IR, mode)`, and the observed ordinary path uses
mode zero.  On the canonical paths below, the only external ownership state
reached is the same allocator and output vector already supplied by the
harness; zero initialization is valid for the remaining context fields.

The Iris TEXLD dispatcher at `0x1a538b` passes eight stack arguments to
`0x1bda50`: `(context, IR, 0, 0, 0, 0, resource_metadata, 0)`.  Static audit
shows `ret 0x20` and mandatory indexed reads through `resource_metadata`, plus
a flag read at `+0x120`.  The harness therefore supplies a mapped, zeroed
`0x200`-byte table rather than a null pointer.  `IR+0x20 == 0` selects its first
entry.  This is an exact zero-metadata compiler fixture; the metadata fields
for a declared 2-D Quake3 sampler still require a differential matrix.

The i386 helper must be built with `-O0`: these old routines use mixed custom
calling conventions and depend on the caller's literal stack layout.  The
launcher enforces that build mode.

## Canonical input

Every run starts with a zeroed `0x88`-byte backend IR record and sets:

```text
IR +0x0c = source operation
IR +0x10 = 0x00040000       FLOOR only: xy component pair
IR +0x14 = 0x000f0000       full xyzw destination mask
IR +0x1c = 0x10e40000       identity-swizzled source 0
IR +0x24 = 0x10e40002       identity-swizzled source 1
IR +0x2c = 0x10e40004       identity-swizzled source 2
IR +0x34 = 0x10e40006       identity-swizzled source 3
context +0x1f4 = 1          live Windows mode-6 selection
```

The destination and sources are deliberately synthetic internal descriptors.
The corpus proves the backend's output for this exact input.  It must not be
treated as a claim that every mask, descriptor class, predicate, or modifier
uses the same sequence.

## Exact native sequences

Each pair is `(W0, W1)` in execution order [RUN].

```text
MOV:
  ffc0001c a2803f1f
  ffc0811c a2803f1f

ADD:
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a800f
  ffff801f 0000000f
  dfc4001c ac800d00
  ffc4011c ac800d00

SUB:
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a800f
  ffff801f 0000000f
  dfc4001c ac800dc0
  ffc4011c ac800dc0

MAD:
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a803f
  ffff801f 0000000f
  c205001c ac400d10
  e205811c ac400d10

MUL:
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a800f
  ffff801f 0000000f
  c23f801c ac200100
  e23f811c ac200100

RCP:
  1fff801c ae203000
  000301bc 8c26ac10
  0040801c ae728300
  000301bc 8c26ac10
  0040801c ae728300
  0040811c ae728300

RSQ:
  ffc0001d a2803c0f
  002600bc 9e26b090
  003f811c ac200d00
  004080fd a2727f00
  003f811c ac200d00
  004080fc a2727f00
  004081fc a2727f00

DP3:
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a800f
  ffff801f 0000000f
  c203011e adb08000
  e200811e ac400c30
  ffc0801c a2803f3f
  ffc0811d a2803f3f

DP4:
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a800f
  ffff801f 0000000f
  c203011e adb08000
  e200811e ae003030
  ffc0801c a2803f3f
  ffc0811d a2803f3f

MIN:
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a800f
  ffff801f 0000000f
  c203005c af000d10
  e203815c af000d10

MAX:
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a800f
  ffff801f 0000000f
  c203007c af000d10
  e203817c af000d10

EXP:
  1fff801c 9e851d40
  0027011c 982082d0
  0040811e a273ccf0
  0040811e a2740cf0
  007f801c ac2000c0
  007f811c ac2000c0

LOG:
  ffc0001d a2803c0f
  1fff801c 96852ec0
  1fc3001c ac800d10
  0029011c 944041e0
  0040821d a274c8a0
  0041021d a27508a0
  0041021d a27548a0
  0080011c 9c451f00

TEXLD (D3D operation 66, zeroed resource metadata):
  ffff8000 0000000f
  ffc0001c a2803f0f
  ffc0011c a2803f0f

CMP (D3D operation 88):
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a803f
  ffff801f 0000000f
  c205005c af400d10
  e205815c af400d10

DDX (D3D operation 91):
  e004001d b05a800f
  ffff801f 0000000f
  dfff801c 8c020d10
  ffff811c 8c020d10

DDY (D3D operation 92):
  e004001d b05a800f
  ffff801f 0000000f
  dfff801c 8c024d10
  ffff811c 8c024d10

SETP (D3D operation 94, zero comparison-control field):
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a800f
  ffff801f 0000000f
  c23f83dc afc00100
  e23f8cdc afc00100

FLOOR (Iris operation 0x8003):
  e004001d b05a800f
  ffff801f 0000000f
  e004001d b05a800f
  ffff801f 0000000f
  c23f807c 9c014100
  e23f817c 9c014100
```

This closes the raw-byte question for the core arithmetic family, four complex
scalar lowerings, ordinary MOV, and one FLOOR form under one canonical
descriptor configuration.  A first Mesa backend can use these results as
differential fixtures while its descriptor builder is developed.  Driver
emission should still be generated from typed lowering rules rather than
copying these literal sequences for arbitrary IR.

## Remaining compiler boundary

The direct generic-lowerer experiment is an important negative boundary.  With
operations 2 through 11 placed in the otherwise canonical input, `0x1b5390`
returns the same two placeholder slots for every operation.  Opcode semantics
must therefore be carried through the audited family wrappers; treating the
generic entry as an arithmetic dispatcher silently produces wrong code.

Direct D3D SLT and SGE remain outside the exact corpus.  Sending operations 12 and 13
through the audited two-source arithmetic wrapper produces identical slots,
showing that their distinguishing comparison control is synthesized upstream
rather than represented by the operation field alone.  The GL compiler's
normalized CMP and SETP paths are now exact for zeroed control fields, but the
six comparison predicates still need controlled deltas.  Texture resource
metadata, dimensions and units, and controlled mask, swizzle, modifier,
predicate, destination-class, and coordinate-dimension matrices remain
required for a complete semantic ISA table.

## September 2026 operand/control basis

`tools/m9-native-lower-matrix.py` now runs 195 hash-pinned cases
through the same saved lowerers and records every packed native word in
`captures/m9-native-lower-matrix-20260914/matrix.json`. The matrix is fully
deterministic and can be regenerated offline with `--write` or checked with
`--check`. The harness accepts explicit inputs through its `M9_IR_*` and
`M9_TEX_*` environment variables; its original defaults and the 17-operation
golden corpus remain unchanged.

The MOV destination-mask matrix exhausts all 15 nonempty xyzw masks. It proves
that xy and zw are independently emitted pairs, omits an empty pair, and uses
the already identified low operations `1d`, `1e`, and `1c` for first, second,
and both components. The JSON fixture gives the complete exact sequence for
every mask.

The swizzle basis independently toggles each two-bit field and includes
identity `e4`, BGRA `c6`, reverse `1b`, and YXWZ `b1`. All 17 cases produce
distinct native streams. Nontrivial cross-pair swizzles expand from two slots
to three or four, proving that a new compiler must retain the vendor's
decomposition rather than merely permute one packed selector. The basis spans
every bit position in the eight-bit source swizzle; arbitrary composition
remains an optimizer validation target.

The complete 0..15 D3D source-modifier nibble has five observed equivalence
classes. The GL-relevant cases are exact:

| source modifier | native W1 change from ordinary MOV |
|---|---|
| none `0` | `a2803f1f` |
| negate `1` | `a2a03f1f` |
| absolute `11` | `a2803c1f` |
| absolute-negate `12` | `a2a03c1f` |

Raw values 2..10 and 13..15 collapse to `a0000c1f` in this direct backend
fixture. Since this bypasses frontend validation, that equivalence is not a
claim that those source forms are legal for this MOV path.

All 16 destination routing nibbles yield the statically predicted seven
classes: 0; direct 1, 2, 3; collapsed 4..12 to 0; and direct 13, 14, 15.
Temporary and varying sources share the same low physical selector for equal
indices; constant `c0/c1` select distinct gated W0/W1 forms.

Predicate-enable changes full-mask MOV low operations from `1c` to `00` for
both emitted pairs. Predicate NOT polarity changes those to `10`. Raw p0..p5
indices produce identical packed words in this fixture, so the register index
is not encoded in these two MOV slots and must remain external predicate state
until another path proves otherwise. Conditional mode independently changes
W1 from `a2803f1f` to `42803f1f`; combining conditional and predicate-enable
preserves both deltas. The context-selected MOV modes are also exact: mode 6
uses `a2803f1f`, while guarded mode 10 uses `ae800f1f`.

TEXLD resource dimension values 0, 3, and 4 produce the same three-slot stream
under the zeroed resource/link fixture. This is a useful negative result only:
the tested metadata is insufficient to activate a dimension-specific branch,
so texture resource metadata remains unresolved rather than invariant.

The added 104-case derivative basis covers every nonempty destination mask and
the existing swizzle bit basis, plus GL-relevant modifiers, all six positive
and negative predicate names, and conditional/predicate combinations for both
axes.  Corresponding DDX/DDY sequences differ only by W1 bit 14 in derivative
result slots.  Cross-pair swizzles can split a logical derivative into as many
as four result slots.  Predicate polarity remains in the low W0 operation,
conditional execution remains W1 bit 19, and predicate number remains external
to these slots.  `parhelia-m9-native-derivative-re.md` records the static
wrapper and marker join that assigns these streams.
