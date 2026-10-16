# Optional VS component cost witness

The retained actual bump input at context0/job5 contains six fields and 274 C2
slots. The source audit joins both source-fed heads to the complete emitted
compiler body, with only the source-defined finish marker normalization. It
does not infer a match from opcodes or similar constants. The wireframe workload
has no continuation head and cannot benefit from this particular change.

The continuation roots two texture-coordinate XY pairs. NormalEye occupies the
other half of the same stored vec4 and is consumed by FIRST, not CONT. Current
whole-definition roots also emit those seven NormalEye definitions in CONT:
four two-slot multiply/add operations, five-slot dot3, eight-slot reciprocal
square root, and two-slot scale, totaling 23 slots. The retained actual C2
indices are129..151. The proof retains roots, graph, demanded masks, emission
ranges and exact body words.

The host prototype keeps every component read by a demanded real ALU and only
narrows vector/MOV alias dependencies. On the later paired-publication compiler,
FIRST65/CONT76/MIDDLE7/LAST31 becomes65/53/7/31 (179 to156). All references pass,
and intentionally removing a required output fails. A tested stream reorder
merely moved the same work between heads and gave no saving; it was not adopted.

The production implementation and broader alias-lifetime/paired-output negative
references live in the separate Mesa component-dependencies change. This RE
tool changes only isolated diagnostic copies and keeps the baseline compiler
unchanged. Its default baseline source is the older actual byte-matched
compiler; `--prototype-driver` selects the paired-publication compiler for the
prototype. It does not admit GPU output correctness or measured performance.

Reproduce with the existing Mesa build and retained whole-input capture:

```sh
python3 tools/m9-vs-head-cost-audit.py --mesa "$MESA_TREE" \
  --driver "$BYTE_MATCHED_DRIVER" --prototype-driver "$PAIRED_DRIVER" \
  --retained-inputs "$RETIRED_JOB_INPUTS" --loader-lib "$T2_LIB64" \
  --out /tmp/m9-vs-head-cost
```

The source pins, input identity and complete emitted witness are retained in
`tests/m9-vs-component-cost-20261007/proof.json`. The named GPU input is an
observation; no address or captured parameter is replayed in the optimization.
