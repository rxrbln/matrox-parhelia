# Genuine ordinary SYSTEM constant-red control

This separate control draws through the real public D3D callbacks directly
into an unsampled SYSTEM format-6 resource. It is a useful discriminator for
the still-black FAA ordinary output, whose main stores respond to the public
red/green constant change. This source capture supplies no GPU execution,
completion or pixels. It does not enable application FAA or prove full PDEV
admission.

The caller requests the same resource constructor and profile used by the
real internal ordinary output helper: `51870`, kind 1, profile 14, logical
format 6, 320×240, one layer. The real returned resource uses hardware format
`0x64`, an 80-page flags-`0x30` OS MDL mapping, compact PTE policy `b`, and
source-known companion metadata `[0,80,0]`. Its returned descriptor-9 cache
is valid and contains its own GPU base tagged 1, `c8210140`, `100f0`.
The native producer must derive a new live owner and mapping; these captured
addresses are not native addresses.

The actual association producer `7a770` creates the retained wrapper and
handle below the existing unjoined public PDEV admission. Advertised public
ContextCreate `bec70` then succeeds. A first prototype public draw reached
`75788` with an unpublished per-head manager. Its exact source consumer is
`7577b..75792`: context hardware `+0xc`, selected head `+0x28`, hardware
`+fea8[head]`, then the owner's vtable `+8` admission. The final flow calls
the genuine `20810` publisher, including actual `20a80` and MM target
selection, exactly as the sampled FAA fixture already does. It assigns no
internal manager pointer or success result. The source checker pins those
consumer bytes, publisher helper hash and same-run returned owners.

The genuine public DrawPrimitives2 `beeb0` caller uses CREATEPS54, SETCONST57
and SETPS56 with the same MOV `oC0,c0` program and red `(1,0,0,1)` constant
as the earlier FAA red control. Context and draw both return success/HR0.
Two genuine preprocessing/raster launches occur. The 240-byte program,
115160-byte resident state, 136-byte raster/preprocessing constants and
188-byte actual geometry are byte-identical to the FAA public-red capture.
This target has sample count 1 and its actual FAA owner remains disabled.
No FAA activation, companion graph, `abc20` resolve or companion page clear
is invoked. Ordinary raster state is retained from its real compiler; it is
not copied from the sampled FAA shader state.

The independent real `23b80` finish is called only after the returned draw
and actual queue `+94 == 1`. It emits its own final work and stops at
`23bc3` before loading the GPU-written completion counter. Desired low is
`d0`, high zero. Primary grows from 4632 to 4644 bytes by a genuine direct
`2c/2d` secondary endpoint packet. Secondary grows from 1164 to 1180 bytes:
its final 16 bytes are count 2, E6/E3 header, selector 1 and token `d1`.
The three independently emitted secondary batches are 604, 560 and 16 bytes;
all six direct endpoints are captured from their real stores. The final
token is in secondary at offset 1176, not the last primary word.

The new complete plan has 23 retained owners, 62 typed operands, 30 nonnull
fixups including six direct endpoints, 16 actual timeline fields and 123776
known input bytes. It contains no known pixel bytes for the 327680-byte
destination. Only the real sparse CPU inputs are exported; output pixels and
unknown tails stay absent. The common-counter mapped A proxy, physical
counter channels, default virtual restore grammar and one-shot ownership
rules remain the established source contracts.

The strict source checker verifies the public caller, per-head constructor,
real descriptor, each mapping's PFNs/PTEs/known metadata, all chronological
known MDL writes, typed domains, direct endpoint stores, independent finish
and source input identity. The actual native C encoder passes two placements
times eight page alignments, all 30 fields and 16 timelines, whole nonfixup
byte preservation, the source secondary count/finish and unknown-output
preservation. Default output, green, red, init-only and post-triangle plans
and blobs regenerate byte-identically; their stored headers are unchanged.
The fixture also extracts the actual kernel's guarded absent-clear helper:
it retains exact primary length 4644 without touching a companion or packet,
and refuses a null CPU owner, VRAM primary or insufficient primary extent.
This requires the separately integrated kernel profile guards; it does not
exercise an old sampled companion-clear implementation for this control.

All artifacts are under `captures/m9128-device-inputs-20261006/`:

- `ordinary-system-constant-red-{input-manifest,source,control,addresses}-101f8.json`
- `ordinary-system-constant-red-native-plan.json`
- `ordinary-system-constant-red-known-inputs.bin`
- `ordinary-system-constant-red-kernel-data.h`

Reproduce with `/home/ai/tmp/faaemu/venv/bin/python`, which already contains
Unicorn and pefile. From the repository:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9-native-ordinary-system-template.py \
  --manifest captures/m9128-device-inputs-20261006/ordinary-system-constant-red-input-manifest-101f8.json \
  --source captures/m9128-device-inputs-20261006/ordinary-system-constant-red-source-101f8.json \
  --control captures/m9128-device-inputs-20261006/ordinary-system-constant-red-control-101f8.json \
  --addresses captures/m9128-device-inputs-20261006/ordinary-system-constant-red-addresses-101f8.json \
  --plan /tmp/ordinary-red-plan.json --blob /tmp/ordinary-red-inputs.bin \
  --kernel-header /tmp/ordinary-red-data.h

/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-ordinary-system-native-cpu.py \
  --plan /tmp/ordinary-red-plan.json --blob /tmp/ordinary-red-inputs.bin \
  --kernel-header /tmp/ordinary-red-data.h --kernel-source KERNEL_M9_FAA_NATIVE_C
```

The generated kernel header explicitly defines
`M9_FAA_ORDINARY_SYSTEM_CONTROL=1`, `M9_FAA_HAS_SAMPLED_COLOR=0` and
`M9_FAA_HAS_COMPANION_CLEAR=0`. Sampled color and companion-clear owner indices
are absent sentinels; clear bytes/start/end are zero. A separate exclusive
private kernel selector must choose the entire header and distinct firmware.
Every real owner must be allocated. The external caller color BO remains only
the ioctl reservation/readback lifetime token; it must not replace the actual
SYSTEM render target. No absent owner may be indexed, no companion clear may
be called, and exact primary bytes must be 4644. The existing explicitly
gated RGBA ordinary-output reader matches the actual descriptor and geometry.
Use the explicit `--expect-ordinary-system-red` oracle for this unsampled
control. It preserves the actual positive CPU control, real retirement,
interior/exterior, expected red channel and 9300..10000 colored-pixel checks.
Only FAA's minimum of 50 partially covered pixels is excluded. Existing default
FAA green and `--expect-red` modes retain that minimum and their original
checks. The switch changes only the readback verdict, never GPU state.

The offline fixture extracts the actual production CLI, per-pixel statements
and verdict and exercises 24 pass/fail cases without DRM or GPU calls:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-image-oracles-cpu.py \
  --source tools/m9-native-faa-image.c
```
