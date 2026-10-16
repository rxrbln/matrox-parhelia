# First public draw after retained graph INIT

This separate source control reconstructs the sealed graph INIT exactly, joins
its real M9128 completion receipt, then calls public DrawPrimitives2 on the
same graph and context. CREATEPS54, SETCONST57 and SETPS56 are real public
commands: pixel shader `MOV oC0,c0`, c0=(1,0,0,1). The retained native sequence now retires preprocessing `0x110`, returned
DRAW plus explicit finish `0x140`, and the normal Lock prefix `0x170` on real
M9128 hardware. Each step preserves the same graph and source post order.
The returned Lock CPU view and real diagnostic antialiased pixels now pass.
Application rendering and repeated-frame integration remain open.
This is separate from the earlier diagnostic180 flow.

The qualified INIT receipt is the confirmed-retirement archive under
`tests/m9-faa-graph-init-hardware-20261007/`: INIT/independent WAIT_POINT both
returned b0; the actual four counters are b1/0,10/0,10/0,10/0. The controller
pins the exact INIT plan and archived file hashes. It rejects all new deadlines
not covered by that receipt. GPU-owned BAR and mapped MDL bytes require a
source CPU write; newly constructed local shader owners join this guard before
CPU mapping. No raw hardware buffer is imported.

The actual first draw sequence is:

1. Retain all six borrowed graph owners, existing private context/pool and
   initialized virtual-primary transport. The independent INIT finalizer is
   not resumed through its resource destruction after retirement.
2. Source PTE publication precedes program access. Real 2cb40 takes the newly
   allocated program resource, arguments (resource,0,1). 2cba8 writes 314=1;
   the CPU-root publication writes 31c=f,0,c. The program has two exact alias
   pages, allocation extent1080 and240 source-written bytes. All60 DWORD stores
   at1e5383 occur after map and before release2c878/31c=0 and preprocessing.
3. Public draw launches preprocessing at17e480. Constants/declaration source
   storage is sparse136B at0 plus188B at256. The secondary grows to648B;
   the independently emitted finish extends it to664B. C2/C3 both reference
   the same live program owner at+32. Neither a +0 program entry nor raster
   completion is inferred.
4. Actual source posts END at primary source offset4944 twice, at2461e then
   b55af. Source CPU-known primary extends to4956 at the original100 wait,
   but those last12 bytes are not included in either observed END. Do not
   synchronously wait for100 after the4944 post: the source wait is unread.
5. A separate genuine23b80 finish emits another12 primary bytes and posts4968,
   with independently produced target110. Its23bc3 status read is stopped
   before execution. This instrumented finish is explicitly distinct from the
   suspended original100 caller, and must not be treated as a returned draw.

The derived plan has31 owners,20 address fixups,19 timeline fields and123604
source-known bytes. Primary27 contains4968B; program26 adds240B; sparse
constant/geometry28 adds324B; secondary29 adds664B. The old INIT primary26
maps to post-INIT primary27, and startup27 maps to startup30. Every earlier
owner maps by retained backing identity, and every original fixup/timeline
slot is preserved. The first4880 primary source bytes are exact, including
INIT's b1 footer. All previously CPU-written spans remain unchanged; the only
changed prior resource descriptor word is its observed last-use value0→d0
at backing+30, with no allocation/layout change.

A native continuation must borrow the existing binding and append, rather
than prepare/submit this whole plan as another INIT. Keep origin10; do not
reseed counters or rebase existing/new tokens from next_fence c0. Program
upload/mapping occurs after actual INIT retirement. The owner map and
`post_init_upload_spans` identify new uploads; never upload old sentinel or
context images again. Retain all new owners after any post, including timeout
or partial-publication failure.

The companion clear is placement-dependent. If retained native INIT consumed
4884B while source INIT consumed4880B, its live delta is+4. Append88B at the
retained runtime primary_bytes; source END4944/4968 become live4948/4972.
For any supported placement derive the delta from the retained object, then
translate only later primary offsets/END values. Preserve the already posted
prefix. The actual native C fixture checks this rule at two placements and
eight alignments; the source checker preserves original fixups/trackers under
the owner-index mapping. This CPU encoder proof is not a real draw receipt.

Generated artifacts are in `captures/m9-faa-graph-post-init-20261007/`:
`draw.json`, `manifest.json`, `addresses.json`, `native-plan.json`,
`native-blobs.bin`, `m9_faa_graph_post_init_data.h`. Captured addresses and CPU
aliases in these forensic records are not executable native inputs. The plan's
fixups and owner map must derive every executable address from retained owners.

Reproduce host proof from the repository root:

```sh
PYTHONPATH=tools/faaemu:tools/m9emu /home/ai/tmp/faaemu/venv/bin/python \
 tests/m9-faa-graph-post-init-20261007/check.py
M9_FAA_NATIVE_SOURCE=/absolute/path/to/m9_faa_native.c \
 sh tests/m9-faa-graph-post-init-20261007/check-native.sh
```

The first command also refuses a forged receipt, opaque GPU input, false draw
return, altered INIT prefix, invented final deadline, changed program writer,
wrong direct address domain, reordered program release and altered staged END. Original INIT
plan/blob/header regenerate byte-exactly. The pending110 receipt is the next
hardware qualification; original100 continuation and later raster/Lock/resolve
still require their genuine caller and input contracts. No application image,
public presentation or generic shader support is established by this prefix.

## Native ordering review and the next original caller

The first staged END excludes the preprocessing indirect packet, which starts
at4944; the only new address field consumed before it is the sentinel at4896.
The four secondary endpoint fixups are4948,4952,4960,4964. This proves that
upfront known secondary/constant storage is not consumed by the first staged
barrier, while the program's actual CPU mapping and upload must still follow
INIT. There is no additional source MMIO or GPU-input read between the two
identical END stores. A separate refusal control changes this staged END and
is rejected. Do not treat the duplicate as an extra completion phase.

Before the program kick, the source publishes31c=f,0,1 then5,0,2; these are
mapping publication actions, distinct from the subsequent CPU-rootf,0,c.
A native implementation must derive the required live local/GPUvirtual
mappings and use its existing publication helpers; replaying the numeric
sequence without owner/PTE identity is insufficient. Release31c=0 follows all
240B program stores. No surface-window formatted slot is acquired for that
raw program upload. Admission must recheck the current binding, generation,
six pinned live owners, completed INIT observation, virtual-primary ownership
and empty healthy queue before any mapping/MMIO or END extension. After any
post, failure handling must preserve the owners until retirement or safe
bus-master-off cleanup. No queue start, counter seed or INIT-prefix rewrite
belongs in this continuation.

The next source controller must reconstruct this same frame, save original
252b9 registers and stack before the independent finish, and consume only a
new qualified110 receipt for this plan. It must not resume the23b80 finalizer
through destruction. Resume the original public DRAW to its own return,
checking the actual return/HRESULT and any newly emitted packets/deadlines.
Only then call the genuine retained sampled wrapper's ordinary Lock receiver
(resource+268, actual vtable+14=a8a60 →a8d60 →55850), preserving its prepared
startup wrappers and normal staging/resource selection. Do not insert Lock
into the suspended DRAW. The later57d90/48c60 path, mapper publication and
returned formatted view require same-frame dynamic joins; neither a F021
pixel view nor a normal Lock result is supplied by the present prefix.

## First prefix hardware receipt and next boundary

The exact single-run M9128 archive is
`tests/m9-faa-graph-post-init-smoke-20261007/hardware-20261007/`. On boot
`1c02066a-0808-4957-a07b-482672b8f39b`, the live graph INIT retired at
`0xb0`, then the first DRAW prefix retired at `0x110`/raw `0x111`. The
source-derived appended primary endpoint was 4,972 bytes after the
placement-dependent companion-clear delta. Both immediate and +2s snapshots
reported an empty healthy queue. The offline receipt checker passes.

Using only that new receipt, the preserved original `252b9` caller returned
EAX1 and its public HRESULT was zero. The genuine wrapper at resource+268
selected its vtable+14 normal Lock and emitted Lock relocation fields. The
emulator stopped before reading a new common-buffer deadline at `0x140`;
that deadline is not covered by the `0x110` receipt. The bounded source audit
at `tests/m9-faa-graph-post-init-lock-20261007/` proves one natural post at
source primary endpoint 4,980. That 12-byte suffix and its 692-byte retained
secondary packet belong to the **returning original DRAW**, before normal
Lock is invoked. The Lock writes 14 relocation fields farther into the CPU
primary buffer but has **no observed post** before waiting for `0x140`.
The original DRAW enters vendor commit `0xf9b70`, publishes fence `0x140`
through `0xfadc0`, and then makes the sole observed MMIO post. At both
publication and post its CPU queue counters read `next=0x130, cached=0x110`.
The following Lock's index-43 dependency emitter requests that published
`0x140` fence, while the `0x141`
completion token is CPU-written at primary offset 5,108, beyond the only
posted endpoint 4,980. The emulator's queue counters already read
`next=0x130, cached=0x130` before Lock, then `next=0x150, cached=0x130`
at its `0x140` wait; the active cursor changes from the secondary queue to
the primary while the secondary cursor is retained. These are CPU queue
observations, not GPU completion receipts. No explicit `0x23b80` queue
finalizer or `0x23bc3` status load executes in the returned DRAW/normal Lock
window before this wait; the earlier independent finish was a separate
controller call at the suspended DRAW100 boundary. The source queue/finalizer ordering
must be resolved before any native `0x140` submission; the 4,980-byte DRAW
tail is not a Lock completion. All captured
bytes and addresses remain forensic evidence, not live native inputs.

The source-only tail plan at
`captures/m9-faa-graph-post-init-tail-20261007/native-tail-plan.json` strips
all 17 proven secondary owner address words, both tagged indirect endpoints
and two width/height fields from its templates. The latter fields at secondary
offsets `0x3f4/0x3fc` contain packed `240:320` and numerically overlap the
captured conversion-startup owner 30 range; their actual writers
`0xfbf41/0xfbf59` and the live primary shape prove they are **not address
relocations**. Any native tail encoder must derive the packed extent from the
validated current graph shape, just as it must derive owner bases at the new
placement. This plan has no hardware receipt and cannot yet be handed to the
normal Lock or app as completed FAA rendering.

## Explicit finish after returned DRAW closes the source submission boundary

The new source experiment calls the genuine `0x23b80` queue finish immediately
after the original DRAW returns, before invoking normal Lock. This is an
explicit independent flush; it is not a claim that the original DRAW or Lock
calls that method. The real producer appends 16 secondary bytes at
`1356..1372`, including a completion token `0x141` written by `0x2557b`,
and a second indirect packet at primary `4980..4992`. It posts endpoint
4992 and stops before its first actual status load at `0x23bc3`, requesting
`0x140`. No GPU status is supplied and no teardown executes.

Thus the native diagnostic can reproduce the complete returned DRAW plus
explicit flush, retaining the original DRAW post at 4980 followed by the
finish post at 4992. The timeline token is generated from the retired prefix
point plus `0x30`; the four indirect endpoints and 17 resource fields come
from live owners. The source join checks that the preceding 692 secondary
bytes match the earlier original-DRAW audit exactly. The sealed complete
template has 708 secondary bytes and 24 primary bytes. This resolves the
source completion-producer gap; actual native submission, retirement,
subsequent normal Lock/resolve and application pixels remain unproved.

Evidence: `tests/m9-faa-returned-draw-finish-20261007/`; source regeneration:

```sh
PYTHONPATH=tools/faaemu:tools/m9emu /home/ai/tmp/faaemu/venv/bin/python \
 tests/m9-faa-returned-draw-finish-20261007/capture.py --out /tmp/m9-finish/proof.json
python3 tests/m9-faa-returned-draw-finish-20261007/check.py
```

## Complete returned DRAW physically retires at 0x140

Kernel `ea659e6`, module SHA `83f77b5cecc55e05b6d22187a44f12cdc7ce6a97859be14260ed47b5760cc5a2`,
passed the complete graph INIT → first prefix → returned DRAW plus explicit
finish on real M9128, boot `364aa8be-9dc4-4017-bd18-360a393be1dc`.
INIT returned/independently waited `0xb0`; the native prefix retired `0x110`,
then the final source-derived post retired `0x140`/raw `0x141`. The live
primary is 4996 bytes (source4992 plus the live INIT clear delta4), with five
ordered posts. Immediate and +2s checks show submitted/completed140,
next150, queue0/wedged0. The six user handles were closed before INIT;
the graph retained their owners and RELEASE succeeded exactly once.

The capsule is `tests/m9-faa-returned-draw-finish-20261007/hardware-20261007/`.
`check-hardware.py` validates the actual receipt and refuses eight altered
receipt cases. This is a full command-retirement result, not FAA edge coverage
or a screenshot. No normal Lock, view mapping or CPU pixel copy was executed
by the native helper.

Using only these newly qualified counters, `m9_faa_graph_finish_continue.py`
reconstructs the identical source finish, preserves its queue state, discards
its suspended CPU stack without resuming destruction, and starts the genuine
normal Lock. The former140 dependency is covered. Lock now produces three
real END events at primary offsets8536,8536,8548 (middle writer `0x4a327`),
then stops before a new170 common-buffer read at `0x4e1d1`. Its new primary
span is3556 CPU-written bytes after source4992; fourteen relocation events
include eight nonzero owner fields and six genuine null bindings. Footer
writers produce151/161/171 at offsets5120/5176/8544. Selected E3 writers
`0xf5898/0xf5b30` also obtain160 from the actual queue timeline getter
`0x25860` and must be rebased, despite appearing in shader-state blocks.
Their source producer, not numeric resemblance or packet location, identifies
them as timeline fields.
The new170 phase has no native encoder or hardware receipt yet.


## Normal Lock prefix hardware receipt, 2026-10-07

Canonical DRM `7d0e96a` (WIP `3b2ba9f`) generates the source Lock prefix from
live owners and timelines; helper RE `317d8271` runs it with `--draw-lock`.
Fresh M9128 boot `68120712-5aad-4837-9abf-8590f717d19b` completed INITb0,
preprocessing110, returned DRAW/finish140 and normal Lock170 on one retained
graph. Eight source-ordered END posts end at live primary8552 (source8548
plus the placement-dependent four-byte INIT clear delta). Immediate and+2s
snapshots report raw171,submitted/completed170,next180,queue0,wedged0.

Evidence: `tests/m9-faa-normal-lock-20261007/hardware-20261007/` includes the
run script, boot identities, artifact hashes, helper log and kernel snapshots.
Module SHA50b041667299632f96d2037e7d014137d19ab0765bb9b865bc931624bb31224e;
helper SHAde55be8c0957fc0a2fc09b0649c356cfe0f55004dd2597f65112672d8d71863c.
The authenticated3556-byte Lock template derives eight owner fields, five
timelines (including two shader-loader getter values) and two extents. Source
posts3544,3544(repeat),3556 are relative to the live returned-DRAW length.

This proves posted Lock prefix retirement only. The normal Lock CPU mapper,
returned view, pixel contents and application FAA remain unproved. Consume
only this actual receipt when continuing the vendor emulator; no pixel data
may be fabricated from successful completion.


## First correct graph FAA pixels

Canonical DRMaf4e5b7 and helperREf3b93e86 now read the source-qualified
returned normal-Lock view. Actual freshboot470e125d-d5c4-49b6-8148-25cf248d6e59
produced a correct red triangle:9361full plus440partial pixels, zero interior,
exterior or channel errors, and a healthy170 queue. The exact raw image, PNG,
receipt and mutation checker are in
[`tests/m9-faa-normal-lock-view-20261007`](../tests/m9-faa-normal-lock-view-20261007/README.md).
This supersedes earlier no-pixel claims for this fixed control. Application
FAA, repeated frames, live shader/texture/depth state and other sizes remain
open; do not widen the private red control into an application claim.
