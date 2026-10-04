# Fragment colour-matrix lowering investigation

2026-10-05. Source inspected: compact Mesa db0ba87,
`parhelia_fs_compile.c`, especially `ph_fs_assemble`,
`ph_fs_affine_group`, and `ph_fs_schedule`.

The recorded Piglit `glsl-fs-color-matrix` failure is a link failure:
“more instructions than the free slots”. It samples RGB, multiplies a
uniform mat4 by vec4(sample.rgb, 1), and preserves sampled alpha.
The test uses a channel permutation, but lowering must support changing
uniform matrices rather than specialize its initial values.

Current assembly groups separate RGB results, then recognizes an affine
expression only when it contains one non-uniform atom. A matrix row
contains three sampled-channel atoms; `ph_fs_affine_group` leaves its
`x` at -1. Consequently each row is lowered separately and masked into
the final vector. The scheduler then rejects the resulting instruction
count. This identifies a lowering limitation; native capacity for the
general matrix has not yet been proved.

A candidate general lowering collects matching sampled-channel atoms
across all three output polynomials, preshades their three uniform
coefficients into RGB vectors, and emits column-wise accumulation:
`offset + sample.r * column0 + sample.g * column1 + sample.b * column2`.
This should share sampled atoms and avoid separate row-mask instructions.
It must preserve sampled alpha independently and retain the existing
range handling, constant allocation constraints, and rejection paths.
Three multiply-adds are a candidate instruction count, not a hardware
support claim: coefficient range, intermediate saturation, constant
banks, and RGB-channel source replication need compiler and native proof.

Required validation: compile the unchanged Piglit shader, inspect the
actual schedule/constants, vary the uniform matrix without relinking,
check nonsymmetric positive and negative coefficients plus offsets and
alpha, and run the existing fragment compiler selftests. Do not transform
texture contents on the CPU or silently accept an unsupported program.

This remains open behind the major GL2 startup and GPU throughput work.
