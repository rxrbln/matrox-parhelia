# M9128 FAA output destination and CPU read boundary

The joined frame originally supplied the sampled primary directly to the
explicit `abc20` output helper. Hardware retirement of that stream does not
prove that this destination contains ordinary pixels. The vendor has a
separate allocated-output contract. The retained
[101f8 helper capture](../captures/m9128-device-inputs-20261006/normal-faa-read-contract-101f8.json)
executes actual `ac1f0` after the genuine normal triangle and **before** any
manual sampled-primary resolve.

The explicit CPU caller supplies the actual retained outer owner plus four
bytes: its next three fields identify primary, companion and main. It supplies
source/destination rectangles derived from the admitted 320×240 geometry,
a 1280-byte CPU destination pitch and an owned CPU output buffer. This is a
bounded internal helper request. It does not mutate resource admission flags.

Actual `ac1f0` constructs a flags1/kind6 allocation request through `51870`.
Its profile is read from the live device `+69a8` plus `818` (profile14).
The returned resource is **system-backed**, allocation kind1, logical format6,
profile class2, hardware format100. Its layout has kind1, requested320×240,
padded320×256 and extent50000. The actual selector9 descriptor is the returned
GPUVA tagged1, `c8210140`, `100f0`. These are source results for this bounded
shape and format, not permission to replay a captured GPUVA or substitute a
local allocation with another descriptor.

At `ac2d4`, actual `abc20` receives that returned destination, retained
companion and main, and the rectangle. Actual dispatch `ac0e3` and backing
lifetime bookkeeping `ac139` execute. The final AA destination shadow is the
new system owner and its actual descriptor; fragment F0/EF/D7 remain
`e0000400/20007000/0`. The destination stays owned because execution stops at
actual `afbe0` entry, before CPU copy, waits, pixel access or release. No GPU
completion or counter reply is provided.

`a74b0→ac1f0` is a conditional public copy branch. The caller boolean and
resource `+3fc` must admit it. Source investigation shows the ordinary sampled
primary does not meet that separate resource-profile/request admission; this
capture therefore does **not** claim to have entered through that public
branch. It proves the allocator/output helper contract needed for a private
native diagnostic. Ordinary wrapper Lock is also distinct: actual
`a8a60→a66c0→a8d60→55850` sees primary `+3d0=1`, copies through `57d90/48c60`
to its retained `+3c8` staging resource, then maps the staging resource and
waits. The earlier direct `2cb40(sampled primary)` host mapper proof did not
cover this chain.

A native implementation must retain a separate system-backed destination,
reserve a live class1 GPUVA and publish all of its PTE/companion metadata
before submission. Draw still uses the sampled primary. Resolve uses the
actual admitted ordinary destination descriptor and owner. The native stream
and indirect sources must be compared against this actual output capture,
including typed operands, timeline, clear page runs and command lengths.
The [passive access-preparation proof](parhelia-m9-faa-output-access-preparation.md)
places the first GPU input inside the ordinary kind1 mapper, after actual
`afbe0 -> 4d2b0 -> aff90 -> 2cb40`. The temporary has no active resource-copy
layers or queue context switch; post-wait code remains statically decoded,
with no successful mapper or copy return claimed. The stop is before the
common-buffer read at `4e2fc`, with no counter value supplied. The actual
tiled address helper is independently verified against all81920 padded pixel
coordinates. See [the full unfinished source and mapping ledger](parhelia-m9-faa-output-inputs-re.md).
Linear DMA interpretation is disproved. Correct hardware retirement and
rendered pixels remain separate qualifications.

`tools/m9-vendor-faa-read-contract.py` runs the source helper against retained
OS/MDL owners with strict stops at the first wait or CPU copy. It requires the
full-frame MDL bridge and scatter observer as explicit tool paths.
`tools/check-faa-read-contract.py` verifies the retained owner relationships,
allocation request, returned format/layout, exact stage sequence and boundary.

The subsequent CPU address builder is now independently executed without
resuming the interrupted copy. Source `aff90` passes the retained raw layout
to `8b390`; `8b020` selects coordinate helper `8c4a0` for the actual platform
table values2/1 and this kind1/32-bit layout. Allocation-kind1 mapper
`2c980` selects `2ca7c`, which returns the already-owned OS MDL CPU alias
from resource `+1f0`; it does not install a CPU aperture decoder. The actual
coordinate helper maps `(0,1)` to byte32, `(16,0)` to256, `(32,0)` to7680
and `(319,239)` to327420. These differ from logical linear row offsets.
Resource `+1cc=1280` is the logical pitch; it does **not** prove that the
underlying DMA allocation is a linear pixel array. A correct native pixel
read must use the actual tiled coordinate contract or expose raw tiled bytes
explicitly. The source helper pins that function and records these CPU-only
address calculations without reading any pixel.

The native `m9_faa_output_offset` implementation is compared against actual
`8c4a0` execution for **all81920** pixels of the full320×256 backing, using
`m9x/tests/faa-output-cpu.py`. Every address matches, and the offsets are an
exact bijection onto the327680-byte allocation. Visible readback excludes the
last16rows. The retained
[coordinate proof](../captures/m9128-device-inputs-20261006/normal-faa-output-coordinate-proof.json)
records count, helper bytes and address-map checksum, rather than a large
array of repeated events. Refused coordinates leave the output argument
unchanged. This is a CPU address proof and makes no claim about GPU pixels.

The first explicit ordinary output hardware read (kernel `df1f49e`, boot
`3b7f5f11`) succeeds after real retirement `0x1d0`, but all76800 visible
pixels are black and the image oracle fails. This is a failed rendering
qualification, not successful FAA. Raw SYSTEM prefix deltas were not observed
in that run; [logs and image](../tests/m9-faa-ordinary-output-20261006/README.md)
preserve this limitation.

The subsequent source-faithful virtual-primary test (`03c30b3`, boot
`a4a2ad1d`) also reaches real1d0 retirement and returns an entirely black
image. Before/after raw327680-byte SYSTEM output CPU aliases remain zero;
this does not independently prove DRAM visibility. The actual source query
wait usescommon20/event0; its write wait usescommon00/event140, rebased to
native1d0 and observed as1d1/0. There is no source-proved separate40 wait.
See [the complete physical observations](../tests/m9-faa-virtual-primary-20261006/README.md).
