# Public clip-space VS and FAA constant interface

The new CPU probe supplies genuine public CREATEVERTEXSHADER45,
SETVERTEXSHADERCONST48 and SETVERTEXSHADER47 packets to the original M9128
normal sampled context. It compiles three VS3 controls (COPY, ADD, MUL position)
and a second viewport. The declaration contains FLOAT4 POSITION0 and FLOAT4
TEXCOORD0; the shader writes position, a caller constant color and UV. The
original declaration parser, frontend, optimizer, FAA rewrite and linker run.
The probe supplies no GPU memory, sampled values, status reply or pixels.
Actual final queue preparation stops at23bc3 before its first completion load.
These compiler observations are independent of the newer graph INIT/post-INIT
source controller and do not establish equivalence with that whole prefix.

The source's public constant handler is dff80, selected by opcode48. Opcode46
is DELETEVERTEXSHADER, not SETCONST. The source handler bounds an8-byte
`{start,count}` followed by16 bytes per register and invokes108960. Declaration
register7 is TEXCOORD0 according to the actual legacy semantic table; the
probe does not substitute the old register1/BLENDWEIGHT declaration.

All four controls produce328 known program bytes: FS32, live shader/carry56,
record gather40, clip-position200. Actual C2/C3 operands join one retained
program owner at offsets0,32,88,128. ADD and MUL change only the shader/carry
vector. The gather and final clip-position vectors stay byte-identical. The
first vector therefore needs the application's live VS lowering, not a
fixed-function attribute MOV template. The200-byte tail differs from the
XYZRHW position tail; the latter cannot be reused for clip-space inputs.

The typed17/18 constant bounds join one actual system DMA owner through the
real reference dereference. FS consumes bytes0..16. COPY consumes only the
used color register in bytes16..32; ADD/MUL consume c1 followed by c0 in
bytes16..48. This shows that the compiler's compact constant ordering is not
the public register ordering. Gather's constant range is empty. Position
consumes the next104 bytes, or13 pairs. The observed source physical tag2 is
not a native address or a blanket policy for a future graph DRAW transaction.

For the bounded public D3D default-depth controls, the first position pairs
are `(W/2,-H/2)`, `(1,1)`, `(X+W/2,Y+H/2)`, `(0,H)`. At viewport
`(16,8,160,120)` these become `(80,-60)`, `(1,1)`, `(96,68)`, `(0,120)`.
The origin pair becomes `(16,8)` and guard coefficients become190.787506 and
-254.850006. The same-run heap-field stores134922/134930,1349b5/134945,
13494b/134951,1349c6/13495b,13499c/1349a2 and134b98/134bad join these
exact values through f485b/f4863 and the real DMA copy at1e5383. Original
initialized instruction spans are retained. The checker validates the whole
constant span and the conditional source stores, rather than matching
address-looking values. The remaining source constants are retained evidence
for this admitted case, not permission to generalize point, fog, user clip,
depth range, raster origin convention or another AA configuration.

The same probe separately executes original14eba0 on five explicit CPU
program inputs: carry counts1/2/3 and gather counts1/2, each admitted MOV body
plus STOP. The original helper pads the shortened count1 vector to4 slots and
sets the same fourth-from-end finish marker as the native carry/gather helper.
Counts2/3 retain5/7 slots. These are original finalizer input controls, not a
claim that five different whole public shaders emitted those shortened forms.
The previous52 original MOV packer controls establish their MOV encoding.
Together they close the previously unexecuted reduced-vector finalizer scope.
The helper's EAX0 is recorded without treating this void-style finalizer as a
failure or a hardware completion.

The general whole-input exporter still stops at unclassified payload encoder
248f2. The retained resource/operand subset therefore explicitly says
`complete_manifest=false`; no full native shader/DRAW profile is exported.
The position scheduling/output metadata, live constant-key remap and retained
record descriptors still need a complete native interface. Mesa app FAA stays
gated; there is no new graph receipt or app DRAW ioctl in this milestone.

Run the strict proof:

```sh
python3 tests/m9-faa-public-vs-interface-20261007/check.py
```

It checks four public controls, typed program/constant ownership, the original
five shortened-vector finalizers and14 refusal controls. To reproduce, run
`tools/m9-vendor-faa-public-vs-interface.py --re-root <RE> --case <case>
--out /tmp/vs/<name>.json` with the vendor Python for copy/add/scale, then copy
with `--viewport 16 8 160 120` as viewport.json. Collect the bounded evidence with
`tests/m9-faa-public-vs-interface-20261007/collect.py /tmp/vs <proof.json>`.
DLL, imported helper, producer and actual0540 input hashes are retained.
