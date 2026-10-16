# Private native M9128 FAA replay

Latest physical result: kernel `bcccb66` on M9128 boot `96a79ac0` passes
the positive CPU control and retires the private frame, but its image fails.
The companion-table profile produces new fragment-store and AA-companion
writes; the actual readback is scattered incorrect data rather than the
green triangle. See the latest qualification at the end of this note and
`tests/m9-faa-clear-20261006`. The allocation-dependent companion clear is
now correctly regenerated; this closes a real command bug, while rendering
remains incorrect. Earlier runs below are historical.

The Linux producer now owns the actual normal FAA frame's allocations and
source bytes. It is a bounded diagnostic behind `m9_faa_diag=1`, selected
through the existing FIRST_TRIANGLE ioctl. It does not enable application
MSAA or permit arbitrary client-owned indirect streams. Normal application
submission refuses while the private context diagnostic is enabled.

`tools/m9-native-faa-template.py` converts the pinned normal capture into
30 unique backing owners (40 resource views), 53 typed address fixups,
26 timeline fields and an input blob containing only 125,008 actual known bytes. Aliases share
their backing owner; constants and geometry retain the physical DMA domain.
The primary is 5,676 raw bytes, while the two counted secondary images
retain their original 604/772-byte structure and physical selector2.
Address-looking instruction/texture-state words are not numeric relocations.

The generated kernel header contains compile-time ownership tables and the
blob size/fingerprint. The kernel loads `m9x/faa-normal-known-inputs.bin`,
allocates page-rounded pinned VRAM/coherent DMA32, copies only valid spans,
and joins the sampled primary to the caller's reserved 320x240 BO.
Opaque stores/HZ/pool and uninitialized allocation tails receive no inferred
CPU initialization. Program/database/metadata sources are unchanged when a
genuine preceding vendor allocation moves their GPU addresses.

The private producer verifies native local identity leaves and their root
leaves before retaining the captured tagged-address encodings. It joins
the requester's command/common-counter/default-restore ownership to native
allocations. Default restore is separate from FAA pool6; the captured
selector1 restore binding is checked against native identity mappings.
Transport drain precedes counter-register modifications under the same
lock. A posted failure retains private owners and quarantines the caller's
color/fence until bus mastering stops. A successful final GPU writeback,
rather than command posting, produces the completion point.

`tools/check-m9-native-faa-template.py` proves captured identity replay,
distinct native rebasing, raw/counted packet preservation, tracker ordering,
untouched non-fixup bytes, absence of opaque tail initialization, and refusal
of unaligned/wrapping owners, damaged source blobs and ambiguous aliases.

`tools/m9-native-faa-image.c` initializes only the application's sampled
primary background using SURFACE_ACCESS, submits the captured green triangle
at (80,180), (240,180), (160,60), then reads through the exact vendor sampled CPU window. Before submission it verifies a positive CPU pattern and clears the background.
It saves actual PPM pixels and checks interior/exterior colors and partial
edge coverage. This is a hardware oracle; it does not rasterize on the CPU.
The scoped sampled-window admission exists only while the diagnostic is on.

The first physical M9128 run (kernel776911a) reached actual retirement
point0x1e0 without timeout and kept SSH healthy on the same boot. The image
oracle FAILED: entirely black, zero colored/partial pixels,8326 bad interior
pixels. Raw former output storage read before another allocation was also
zero. See tests/m9-faa-native-20261006 for hashes, logs and actual pixels.
This is bounded-stream retirement evidence, not working FAA rendering.

Vendor CPU-map tracing subsequently proved selector1 uses F0220140 with
padded height256 and tagged GPU address. Kernel30023d9 admits only that
exact private320x240 view with verified identity mappings. The current image
fixture requires a76800-pixel positive CPU read/write control before clear
and submission. The corrected freshboot probe (kernel30023d9) first refused its pre-submit
CPUcontrol because the identity manager was not yet initialized. A transient
probe using legacy CPUinitialization again retired the FAAstream at0x1e0,
but exact tagged F022readback timed out (rc137, noimage). SSH stayed healthy.
See tests/m9-faa-cpu-map-20261006. Kernel010853b subsequently bootstraps the
private host view before reserving color (built, not deployed here). The
CPUtranslation join and rendered fragments remain unqualified. All four selected E3
timeline snapshots, twenty E3 completion tokens and two waits now translate
through the same genuine timeline bias. The independent fetch audit rejects
plans that omit any of these actual producer-written slots. The private
pool save/restore GPU effect and shader fetch behavior require the hardware
run; CPU capture alone does not prove rendered fragments or completion.

Reproduce the producer and audit from the RE repository:

```sh
python3 tools/m9-native-faa-template.py \
  captures/m9128-device-inputs-20261006/normal-faa-input-bytes.json \
  --external captures/m9128-device-inputs-20261006/normal-faa-native-addresses.json \
  --plan captures/m9128-device-inputs-20261006/native-faa-plan.json \
  --blob captures/m9128-device-inputs-20261006/native-faa-known-inputs.bin \
  --kernel-header /path/to/kernel/drivers/gpu/drm/m9x/m9_faa_native_data.h
python3 tools/check-m9-native-faa-template.py
```

## Reusable ordering and the remaining M9 mapping distinction

The current Parhelia/P690 implementation provides app-level ordering rules:
drain pending draws before helper state changes, resolve the old FAA owner
before framebuffer rebinding, retain color/depth/samplers and private buffers
through resolve/final retirement, then restore ordinary bindings. Its packet
words and FAA fence opcodes are family-specific and are not M9 packet templates.
The private M9 producer already preserves captured scratch/draw/resolve
chronology and retains owners across real completion under the transport lock.
Those application ownership rules remain necessary when generic M9 FAA is wired.

Actual target read-only inspection now shows GPUprimary1422000 has a correct
GPU leaf and a correct root for its leaf-storage page, but its direct CPU-root
entry is zero. The genuine2cb40 mapper copies the primary leaf into the0360
root child at indexsurfaceGPU>>12 before publishing {GPU|1,F0220140,10100}.
This is not a0f64-table copy. The missing direct CPU-root join is independently
visible in tests/m9-faa-cpu-map-20261006/proof/address-observe.txt. Its exact
flush/copy proof and native implementation are in progress. GPU rendering
qualification still requires actual image evidence after the mapping fix.

## CPU mapping repaired; downstream image still fails

The physical kernel1b4dd67 run on bootfd7798f9 passes all76800 positive
patternpixels with exactF0220140/10100/GPU|1 and completes postretirement
readback without timeout. All117648 knownVRAMsourcebytes verifyexactly.
The FAAstream retires1e0, but output remains black. Before/after private
owner observations and readonly output extraction now prove GPUpreprocess
produces the three correctpositions and greenRGBA varyings. Pool/sentinel,
auxiliary andHZ change; mainfragmentstore remains unchangedzero. The next
question is downstream fragment/raster/FAA output, rather than missing VS
geometry or a broken CPUmapper. Actualbytes/logs/image are retained in
tests/m9-faa-root-join-20261006. Kernel82b4dd2 extends directroot constructor
identities to supportedlocalBARpages using actualvendorinitializer evidence;
its CPU qualification now passes in the subsequent run below. No app FAA
success is claimed.

## Vendor virtual restore tags verified; image still black

Kernel `be6d018` retains the preceding CPU-root repair and extends its direct
identity initialization to the vendor's bounded 256 MiB BAR scope. It also
corrects DWORDs 9, 14 and 19 in all 1024 private default-restore records to
virtual-address tag1. The genuine normal format1 initializer is `656c0`;
the complete source proof identifies precisely those 3072 differences from
native `m9_restore_fill`, without replacing work shadows or altering the
ordinary submission path. All fields are checked before modification and
verified after publication.

The physical run on boot `808a1512-2221-4b8f-96fd-3df67a781ba9`
verifies the 3072 fields, passes the 76800-pixel F0220140 CPU control, and
retires the actual FAA frame at `0x1e0`. The primary is `0x3a57000`.
The image remains black: colored/full/partial zero, interior_bad8326,
exterior_bad0, non_green0. Main fragment store and AA companion remain zero;
auxiliary, HZ, sentinel, pool and preprocessing owners change. The pool hash
differs from the preceding run, but this does not prove pixel rendering.
See `tests/m9-faa-virtual-restore-20261006` for the exact module hashes,
loaded/after logs and actual black PPM. The private attempt is consumed;
another full submission requires a fresh boot and a new implementation
candidate. CPU visibility is closed for this fixture; fragment output and
generic application FAA remain open.

## Companion profile writes FAA owners; the image remains incorrect

Kernel `7dd03c5` implements the source-proven private Windows100f8/101f8
constructor profile, while retaining the complete captured101b8 native
frame. The compact table occupies401000/104000 and its optional16-byte
companion505000/410000; CPUroot915000, tail995000 and restorea99000 follow.
The actual cold constructor clears only companion DWORD0. Local mappings
publish [page|8,0,0,unknown] without initializing the unknown last DWORD.
Coherent DMA publishes its known [pte,0,pagecount,0] metadata, retaining the
proven native nibble-a domain. Bounds, conflicts, actual readback and failed
publication quarantine are covered by the source-helper CPU fixture.

On fresh boot `6c79ac0e-786d-4dcb-822e-d34a4b13fb3f`, all76800 positive
CPU pixels pass; known VRAM inputs verify; primary0x3a2f000 retires at1e0.
The image oracle fails: colored270, full0, partial270, interior_bad8326,
exterior_bad183, non_green809. This is not a successful FAA screenshot.

The main fragment store and AA companion, previously zero, now contain
1041 and1404 nonzero DWORDs respectively. Their actual read-only retired
BAR bytes match the before/after kernel observations. Preprocessing retains
correct positions and green varyings. Packed-looking destination pixels
require further resolve/translation/CPU-view analysis; owner writes and
retirement do not establish correct fragment coverage. Complete logs,
module/source hashes, actual PPM and compressed raw buffers are archived in
`tests/m9-faa-companion-20261006`. No application FAA is enabled.

## Corrected companion page clear; hardware image still fails

Kernel `bcccb66` validates the source clear, generates the 151 live companion
pages with their actual alignment, updates the count, moves the already
patched suffix and submits its adjusted length. On native2f0e000 this is20
words/5680 bytes, preserving the spare152nd backing page. The actual vendor
instruction and native-helper fixtures pass all eight alignments.

Fresh physical boot `96a79ac0-255c-403a-8e29-d3df6a31e68f` confirms the
corrected count/length, CPU76800 control and real retirement1e0. ImageFAIL:
colored278/full0/partial278/interior_bad8326/exterior_bad185/non_green823.
Fragment-store and companion FNV hashes/counts are identical to the previous
run. This proven command fix is retained, but it does not resolve the pixel
failure. See `tests/m9-faa-clear-20261006` for actual hashes/logs/PPM, and
`docs/gpu-capture-replay-policy.md` for the standing implementation rule.
