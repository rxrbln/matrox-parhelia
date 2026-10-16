# M9 GL2 compiler and control-flow static closure

This note records the strongest clean-room implementation facts recoverable
from the saved M9 compiler binaries for GLSL/ARB features beyond the initial
arithmetic path. `tools/m9-gl2-compiler-control-evidence.py` verifies the exact
dispatcher table, call targets, macro-lowering counts, and compiler identities.
Run it from the repository root; `--json` emits the compact evidence matrix.

The pinned inputs are Windows `MSerX/MxMD.dll` SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`
and `MSerX/MxMGLX.dll` SHA-256
`d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86`.
Addresses below are preferred-image virtual addresses. `[BIN]` is direct byte
or control-flow evidence, `[DER]` is a deterministic decode, and `[CAP]` is an
explicit live-capture dependency.

## Exact frontend-to-backend routes

MxMD's Iris converter at `0x1a40b0` has one complete 55-entry jump table at
`0x1a5a3c`. The special operations normalize as follows [BIN][DER]:

| Iris operation | normalized backend operation | lowerer | exact static result |
|---|---:|---:|---|
| `KILL` 45 | 65 | `0x1c82d0` | custom `ESI=IR`, `EDI=context` ABI; splits active `xy` and `zw` pairs into comparison records |
| `TEXLD` 46 | 66 | `0x1bda50` | texture mode `(0,0,0,0)` |
| `DSX` 49 | 91 | `0x1c9300` | shared derivative wrapper, expanded third-operation marker `0x88` |
| `DSY` 50 | 92 | `0x1c9300` | shared derivative wrapper, expanded third-operation marker `0x89` |
| `TEXLDD` 51 | 93 | `0x1bda50` | texture mode `(0,0,0,1)` |
| `TEXLDL` 53 | 95 | `0x1bda50` | texture mode `(0,0,1,0)` |
| `TEXLDB` 54 | 66 | `0x1bda50` | texture mode `(1,0,0,0)` |
| `TEXLDP` 55 | 66 | `0x1bda50` | texture mode `(0,1,0,0)` |

The four mode values are distinct arguments 3 through 6 of the common texture
lowerer. The resource metadata pointer remains argument 7. This is an exact
compiler API for the texture family: bias and projection retain ordinary
backend operation 66 and select one mode bit, while derivatives and explicit
LOD have distinct backend operations plus their own mode bit. A new compiler
can represent this as one typed texture instruction with four mutually
exclusive modes. It must reject combinations until a source language and the
vendor compiler prove one legal [BIN][DER].

Zeroed resource metadata produces a three-slot TEXLD fixture, but it does not
activate the dimension-dependent branches. Texture unit, coordinate dimension,
resource class, and the resulting native slots remain a live differential
rather than safe constants [CAP].

## Matrix lowering is closed

The matrix operations are compiler macros with exact row expansion [BIN][DER]:

| Iris operation | backend operation | wrapper | primitive lowering |
|---|---:|---:|---|
| `M4x4` 18 | 20 | `0x1c74c0` | four DP4 calls |
| `M4x3` 19 | 21 | `0x1c7690` | three DP4 calls |
| `M3x4` 20 | 22 | `0x1c7820` | four DP3 calls |
| `M3x3` 21 | 23 | `0x1c79f0` | three DP3 calls |
| `M3x2` 22 | 24 | `0x1c7b80` | two DP3 calls |

Each wrapper clones the 0x88-byte IR for every result row, increments the
destination selector by two per row, selects successive matrix rows with
descriptor component fields, and invokes the already recovered DP3 or DP4
lowerer. If destination and source storage alias, it allocates a temporary and
appends a final MOV. Mesa should lower matrices to scalar row dot products
before register allocation and apply the same alias rule. No new native matrix
opcode is required. One live `mat4 * vec4` case is useful as a regression
check, but it no longer blocks implementation.

## Discard, derivatives, comparison, and shadow

`KILL` is not one generic MOV-family operation. `0x1c82d0` tests the four
source components as two pairs and emits a CMP-family record for each active
pair. It increments the program kill/result counter at context `+0x5a4` after
each emitted pair. Its pair-specific descriptor constants differ, so a driver
must lower discard through this dedicated path and must not substitute a raw
guessed opcode. The actual packed slots for unconditional and conditional
discard still require the linked instruction resource [CAP].

DSX and DSY share `0x1c9300`; their only semantic discriminator enters as
backend operation 91 or 92. The generic builder converts those to expanded
third-operation markers `0x88` and `0x89`. The hash-pinned lowerer corpus in
`parhelia-m9-native-derivative-re.md` closes the packed form: canonical DDX
and DDY each use two common setup slots plus one `xy` and one `zw` result slot,
and every derivative result changes only W1 bit 14 between the two axes.
All 15 masks plus swizzle, modifier, predicate, and conditional bases are
recorded. Destination/source aliasing uses the statically recovered temporary
MOV path [BIN][RUN][DER].

ARB `SLT` and `SGE` are accepted by the frontend, but their distinction is
synthesized in comparison control before the recovered CMP/SETP wrappers.
Directly feeding backend operations 12 and 13 through the arithmetic wrapper
does not preserve that distinction. The safe lowering is therefore CMP/SETP
with an explicit six-way predicate enum, kept disabled until an SLT/SGE A/B
capture fixes the nonzero comparison values [CAP].

Shadow comparison is already closed in sampler state rather than as a distinct
texture opcode. Sampler `+0x46` bit 8 together with sampler `+0x08` bit 13 sets
backend texture word bit 26, and comparison functions map into bits `29:27`:

```text
NEVER LESS EQUAL LEQUAL GREATER NOTEQUAL GEQUAL ALWAYS
    0    1     3      2       6        4     5      7
```

The remaining shadow blocker is the join from a GLSL shadow sampler to those
sampler flags and the sampled result channel in the linked program [CAP].

## Branches and loops

The compiler representation has exact conditional infrastructure: token
`+0x08` bit 0 is the conditional marker, bit 1 enables a predicate, and the
predicate descriptor reaches backend IR `+0x30`. MxMGLX maintains explicit
`PushConditionalStack`, `MaskConditionalStack`, and `PopConditionalStack`
operations. Iris `CALL` 23 and `BREAK` 41 have dedicated converter paths at
`0x1a4dd2` and `0x1a4dec` [BIN].

Static analysis does not close a native target field, relative displacement,
loop backedge, or dynamic reconvergence operation.  The subsequent special
capture preserves successful visible results and full linked resources for
constant and uniform `if` plus fixed-count and uniform-bound loops.  Those
results prove frontend and hardware execution but still do not distinguish
folding/unrolling from native flow or identify target/backedge fields.
Generated hardware control flow therefore remains gated until those records
are joined to the universal native grammar [CAP].

## Vertex texture fetch and stage resources

The linker enforces four vertex samplers and sixteen fragment samplers. The
texture operation family itself is stage-neutral at the recovered lowerer.
A subsequent vertex `texture2DLod` case compiles, links, executes, and preserves
its full linked resource.  Vertex texture fetch is therefore result-captured,
but its native instruction, constant/resource selector, and preprocessing
result route remain gated pending the linked-record grammar [CAP].

Program transport facts remain exact:

* native instructions are two dwords, grouped four per 32-byte upload line;
* index 69 binds the per-context immutable fixed-function
  vertex/declaration-conversion fragment database; the old address-directed
  `0x1b080` BAR snapshot is an opaque alias, not a linear database image, and
  the address translation remains unresolved;
* index 6a is a paired numeric control whose exact encoding remains open;
  index 6d is an optional companion control in the split form;
* index 6c points to a separate 0x80-aligned linked record and preserves only
  low tag bit 0;
* the compiler stream is count-delimited, the upload byte length is transport
  only, and zero slots are padding;
* terminal output is narrowed to endpoint selector `0x7e` or its complete
  final MOV form. There is no proven universal END bit.

New driver submission must treat index 69 and the linked record as separate
relocations with independent lifetimes. Arbitrary program
execution stays gated until a capture identifies native storage, both internal
stage entries, and the hardware stop condition.

## Implementation matrix

| feature | implement now | exact remaining blocker |
|---|---|---|
| matrix operations | DP3/DP4 row expansion | none; live sanity only |
| texture operation IR | typed one-hot B/P/L/DD modes | resource/unit-to-slot delta |
| KIL/discard | dedicated pairwise IR lowering | unconditional/conditional packed slots |
| DDX/DDY | distinct backend selectors 91/92 and 104 exact native variants | helper-lane edge conformance on new driver |
| SLT/SGE | represent six-way compare enum | comparison-control A/B values |
| shadow compare | exact sampler enable/function packing | GLSL sampler/result join |
| vertex texture | enforce four-sampler cap | stage resource and result route |
| static/dynamic branch | frontend IR only | folding, targets, reconvergence |
| static/dynamic loop | frontend IR only | unroll policy, backedge encoding |
| entry/resource bind | typed 69/6a/6c/6d resources | native storage and linked-record internal stage fields |
| program termination | count compiler stream; pad transport | endpoint versus complete-form stop rule |

This boundary is deliberate: compiler routing and macro expansion are complete
enough to implement typed lowering without inventing native meanings. Features
whose packed semantics remain unobserved must return a compile failure until
their minimal live probes close the listed field.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
