# M9 native DDX/DDY lowering

This note closes the native packed-instruction boundary for the GLSL 1.10
`dFdx` and `dFdy` operations.  It uses the saved Windows compiler
`MSerX/MxMD.dll`, SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
No GPU or vendor kernel component is involved.  `[BIN]` denotes direct static
binary evidence, `[RUN]` denotes the controlled hash-pinned compiler-lowerer
corpus, and `[DER]` is a conclusion limited to those observations.

## Static operation and wrapper join

The Iris dispatcher sends `DSX` operation 49 and `DSY` operation 50 to the
same wrapper at `0x1c9300`, after normalizing them to backend operations 91
and 92.  The complete `0x11e`-byte wrapper has SHA-256
`0b087dc355cc25bfed788f1a80db4b09594427be625fffae5839fd249d05ce1e`
[BIN].  It has the ordinary `stdcall(context, IR)` ABI and ends in `ret 8`.

The wrapper first runs the one-source/destination normalization helpers at
`0x1aaf00`, `0x1aad10`, `0x1aab30`, and `0x1aa8e0`.  It compares destination
and source descriptor identities.  If their register identity or source
modifier requires an intermediate, it reserves a two-register temporary,
copies the 0x88-byte IR record, emits the derivative to the temporary, and
then emits an ordinary MOV to the original destination.  Otherwise it emits
directly.  Before calling the generic native builder at `0x1b5390`, it clears
the source's modifier byte from the derivative record [BIN][DER].

The generic builder reads the low 16 bits of IR `+0x0c`.  Its exact selection
is [BIN]:

```text
backend operation 0x5b / 91 (DDX): internal operation = 0, marker = 0x88
backend operation 0x5c / 92 (DDY): internal operation = 0, marker = 0x89
```

The common wrapper therefore owns aliasing, source decomposition, masks, and
temporary movement.  Marker `0x88` versus `0x89` is the sole derivative-axis
choice reaching the final native builder [BIN][DER].

## Exact canonical native sequences

The controlled corpus uses a full-mask destination `0x000f0000` and an
identity-swizzled varying source `0x10e40000`.  Each tuple is `(W0, W1)` and
appears in execution order [RUN]:

```text
DDX:
  e004001d b05a800f
  ffff801f 0000000f
  dfff801c 8c020d10
  ffff811c 8c020d10

DDY:
  e004001d b05a800f
  ffff801f 0000000f
  dfff801c 8c024d10
  ffff811c 8c024d10
```

The first two slots are the shared one-source setup.  The low and high
component-pair result slots have identical W0 values for X and Y.  Changing
only the axis toggles W1 bit 14 (`0x00004000`) in every derivative result slot
and nowhere else [RUN][DER].  This is an exact compiler encoding fact, not a
name inferred from an arbitrary capture: the static 91/92-to-0x88/0x89 join
assigns the two corpus streams [BIN][RUN].

## Operand and control basis

The checked fixture now contains 104 derivative variants in addition to the
existing native matrix, for 195 cases total [RUN]:

| group | cases | exact result |
| --- | ---: | --- |
| destination mask | 30 | all 15 nonempty masks for each axis; one result slot for `xy` or `zw`, two when both pairs are active |
| source swizzle | 34 | 17 bit-basis and representative composed swizzles per axis; cross-pair forms expand to as many as four result slots |
| source modifier | 8 | none, negate, absolute, and absolute-negate per axis; nontrivial modifiers use the wrapper's temporary MOV |
| predicate | 24 | `p0..p5` and `!p0..!p5` per axis |
| conditional/predicate control | 8 | all four off/on combinations per axis |

Every paired X/Y case has the same slot count and identical W0 values.  W1 is
also identical except for bit 14 in one to four result slots.  Predicate
enable changes result W0 low operation `0x1c` to `0x00`; NOT polarity changes
it to `0x10`.  Predicate indices 0 through 5 do not change these slots, which
confirms that the index remains in the already documented external predicate
state.  Conditional execution independently toggles W1 bit 19
(`0x00080000`) in result slots [RUN][DER].

The corpus records exact sequences for all listed operands.  A new compiler
should implement the static wrapper algorithm and existing descriptor packer,
then compare its output against these sequences.  It should not copy one
canonical literal stream for arbitrary masks or swizzles.

## Driver boundary after this result

The native encoding needed for ordinary GLSL 1.10 derivatives is closed for
the recorded operand/control basis.  Saved live draws already establish
distinct nonconstant `dFdx` and `dFdy` API results and their program resources.
The earlier request for one-operation linked-record captures is no longer a
compiler or ISA gate [DER].

This result does not solve the common programmable-stage transport boundary:
arbitrary generated programs still require a verified stage-entry assignment,
terminal rule, and resource lifetime.  Helper-lane behavior at primitive and
viewport edges also remains a conformance-validation item.  Those issues are
separate from selecting and packing DDX versus DDY.

## Reproduction

The static and recorded-evidence checker runs without loading vendor code:

```sh
python3 tools/m9-derivative-native-evidence.py --self-test
```

To regenerate and byte-compare all 195 cases on a host that can execute i386
binaries:

```sh
python3 tools/m9-native-lower-matrix.py --check
python3 tools/m9-native-lower-run.py --check
```

Both launchers reject any `MxMD.dll` whose hash differs from the value above.
The harness exposes only audited lowerer entry points and the one-slot packer.
