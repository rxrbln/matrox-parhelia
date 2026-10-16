# M9 zero-sampler record masks: source/runtime join (2026-10-10)

Current hardware qualification: Mesa `3142d3d` replaces only R88 bits3:0 from
actual native temporary usage for both sampled and zero-sampler record producers.
The same selected 24 shader tests now all pass, including the former 19 failures;
all five matrix and three varied floor controls match independent software
references. Higher masks and register allocation retain their conservative
encoding. Full advanced Quake3 still has a separate orange-floor defect.
[Hardware evidence and exact scope](../tests/m9-mask-aggregate-hardware-20261010/README.md).

The fixed `0x0c << logical_texture_count` interpretation in the older fragment
pass-splitting notes is not a universal record-mode encoding. The actual shipping
compiler derives register-pair reuse from usage, then the runtime excludes the
physical placeholder. This supersedes that interpretation for zero-sampler
record producers. The historical source-only milestones below precede the
sampled producer and hardware qualification recorded above.

Evidence is retained in
[the compiler/runtime proof](../tests/m9-zero-sampler-record-mask-20261010/README.md).
Pinned MxMD.dll SHA-256:
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
Addresses below identify vendor instructions in this image, never native GPU
allocation addresses.

## Observed compiler contract

`150dc0` reads six usage DWORDs from the actual compiled variant: written
`+0c/+10`, inputs `+14/+18`, outputs `+34/+38`. It publishes the 16-bit pair
mask at `+398`. The independent reference already implemented by
`tools/m9-vendor-glsl-compile.py:vendor_pair_mask` matches genuine recalculation:

1. Set a pair bit when neither of its two written-register bits is present.
2. For pairs12/13, clear the bit if both lanes occur in inputs or outputs.
3. For each four-register group, if any input lane exists and only one of its
   two pair bits remains set, clear both pair bits.
4. Apply that closure to outputs only when both halves contain output lanes.

The proof verifies actual variant usage/masks for uniform-only GLSL, a real
white texture control, a vertex control and all six runtime compiled records.
It also retains genuine calculator controls with additional written bits16/18
and a distinct 4096-byte compiler allocator placement. These establish compiler
metadata behavior, not portable pointers or a runtime texture binding.

`17f040` transfers the compiler mask into the common-state shadow. The public
uniform-coordinate producer uses ordinary temporary00 (written usage1); its
observed compiler/common mask is `0000fffe`. The low pairs may be reused only
when their actual final native temporary destinations leave them unused.

## Observed final runtime lowering

The final common-state writer is a separate source stage:

- `f98b3` clears the texture-count field, `f98b9` sets bit20 and `f98bf` stores
  it. The actual zero-logical-sampler producer receives one physical placeholder.
- `f98ce` executes `AND DWORD PTR [common+10],fffffffc`, protecting physical
  unit0 register pairs00/01 and02/03 after compiler usage has been transferred.
- `f5050` emits the common-state indexed group. For this same retained shadow,
  final R88 is `0010fffc`, R89 `fe007ff9`, texture units1, coordinate routes0.
  Its recorded last R88 writer is a full DWORD store at `f98ce`.

The proof records the actual common packet owner and its `c18a8988` indexed
header as well as live program/system/texture allocations. The placeholder and
record export remain actual vendor-owned runtime resources; no unknown texture
result or GPU completion is invented. The source-generated public PS has one
sample whose coordinate is uniform arithmetic, causing a genuine zero-sampler
record producer followed by its sampled consumer. The varying-coordinate
control separately supplies actual zero-sampler producer compiler metadata.

## Native consequence and scope

The current native full/error/guard matrix producers write registers04..07,
including hoisted constants and arithmetic. Their four low pair-unused bits are
3 before placeholder exclusion and0 afterwards. The previous fixed `0x0c`
marks those live pairs reusable. That is a concrete ownership conflict; the
actual vendor `fffc` control has a different live temporary set and cannot be
replayed as a universal mask.

Mesa commit89c2c18 derives those four bits from each final native producer
stream, ignores exact stop padding, and then clears physical placeholder bits0/1.
Temporary-writing opcode1c is decoded only in00..07. Record outputs11/13 and
interpolated sources are outside this low temporary domain. The helper covers
all admitted raw templates and hoisted constant writes. Register allocation,
high pair masks, sampled producers, stream/record extents and retained allocation
owners stay on their existing paths. This is source-derived live ownership,
not a captured parameter replay or an arbitrary low-mask override.

Actual-C host proof:1040 checks pass, including every low temporary write set,
marker/no-marker stops, wait/idle forms, vendor and full/white native streams.
ASAN/UBSAN passes with leak detection disabled. The182-shader corpus output is
byte-identical to base419904b (required23/23 exact, same pre-existing nonzero
suite status). Full/error producer R88 becomes `00100300`, guard `01100300`;
white stays `0010031b`, ordinary column `0010c300`, consumers `00100303`.
All five schedules/constants/native words remain identical.

This CPU emulator/source proof does not establish hardware rendered correctness.
The hardware gate is full/error/guard/white/column against software, followed by
relevant matrix Piglit and advanced glmark2/Quake3 controls with healthy GPU
retirement. Root owns the shared locked hardware queue.

## Sampled producer source control (2026-10-10)

`runtime-dependent.json` retains a second source-generated public PS: sample
unit0, add uniformc0 to that value, then use it as the coordinate of sample
unit1. The actual vendor returns a sampled record producer with one physical
texture unit and native slot `1fc0111c bc8fc110` (write output11 from input0).
It writes no ordinary low temporary. Genuine `150dc0` mask isffff; `17f040`
common state and final `f5050` both retain `0010ffff`. Its last R88 writer is
`17f1ea`, and the zero-sampler `f98ce` exclusion is not applied. This actual
common-state join establishes that the unused-pair mask is not a universal
ORed record flag for sampled producers either. It remains CPU/runtime source
proof, with actual retained owners and no supplied GPU completion or texels.

Mesa candidatea99623d extends the final native low-pair derivation to sampled
record producers. Only R88 bits3:0 may change; bits31:4, `record_reserved`
allocation, texel-live exclusions and all gates remain unchanged. The actual-C
fixture now checks3132 cases, including this genuine sampled stream and a pinned
copy of the former production R88 function for exact high-bit preservation.
ASAN/UBSAN passes (leak detection disabled). Seven shader comparisons preserve
all native words, constants and schedules, with R88 XOR restricted to0000000f.
The182-shader corpus stays byte-identical, required23/23 exact.

White producer `0010031b` becomes `00100313`, protecting live06/07. The scalar,
INLINE0 floor187 has the same conflict in producer passes0/2/5; pure one-unit
texture exports instead changefffb toffff. Weapon199 and zero-sampler masks
stay unchanged. This identifies a source-level ownership discrepancy; it does
not establish that the residual orange gameplay floor has this cause.

Three independent floor fixtures in
`tests/m9-floor-varied-inputs-20261010` add smooth varied normal texels, enabled
deluxe lighting with gradient texels, and both together. All other original
floor187-q3pb inputs remain identical. Their llvmpipe references distinguish
actual shader values missing from the old flat-white/deluxe-off control.
Hardware qualification remains root's locked batch; neither candidate rendering
nor gameplay success is claimed from these host proofs.
