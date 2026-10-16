# M9128 FAA logical and stale-value audit, 2026-10-07

The latest qualified hardware prefix is prepared preprocessing120, archived in
`tests/m9-faa-prepared-flow-hardware-20261007` (RE7efccaad). Kernel a78c07b
now defers the program's known240 bytes until the real c0 boundary, derives its
raw CPU mapping from the retained live owner, and performs the source-proven
kick/mapping/upload/release sequence. It verifies the uploaded bytes before
posting120. Actual hardware retires120 with empty queues and wedged=0; the
preprocessing buffer changes. Final raster/resolve and FAA pixels remain
unproved. Neither that buffer change nor the CPU program upload is an image.

The entry-inventory fix is kernel aad2ec0: generated mask2 admits the genuine
preprocessing entry32 while older full profiles retain both entries. The raw
binding-index alias is fixed in Mesa faea59a, with wide-value validation before
narrowing and paired-index wrap rejection;140 actual initializer/emitter
controls pass, and the isolated DSO and batch build pass. The review below
records the evidence that led to these changes; its original "no product
change" statements describe the review checkpoint, not the current code.

## Current review: program entry assumptions and CPU mapping order

Reviewed kernel4663b49 (code08c9bda), REcb961527 and Mesaca86fce. This is a
logical review; no new hardware run or rendering success is claimed.

1. **The program guard assumes a later, complete draw.** In
   `m9x/m9_faa_native.c:m9_faa_program_access`, `entries < 2 || offsets != 3`
   requires both program offsets0 and32. The genuine preclear-draw prefix has
   only the preprocessing entry32. Extracting the actual C helper and the
   complete existing `m9_faa_preclear_draw_data.h` table reproduces EINVAL at
   two live placements under ASAN/UBSAN, before identity checks or MMIO.
   That existing prefix does not enable PROGRAM_ACCESS_CONTROL, so this is
   a prospective integration blocker, not a regression in the previously
   tested full profiles. The fresh prepared draw export likewise has only
   entry32. Its generated contract must carry the source-proven entry mask;
   do not weaken malformed-pair or unique-owner validation globally.
2. **The frontend kick is not the whole observed program-access flow.**
   The current callback checks GPU identity and writes314=1. The fresh
   original continuation after the actual prepared-c0 receipt observes
   2cba8 writing314=1, 59b82 writing31c=f,0,c, then2c878 writing31c=0 before
   the next requester posts. The kick's source resource identity uniquely
   joins the program owner. Exact mapper arguments, aperture effects and
   temporal first-consumption evidence are being sealed by the source agent.
   Current native code has no corresponding program CPU-map call. Existing
   `m9x_address_cpu_local_prepare` in `m9_address.c` shows why a GPU identity
   check is insufficient: it separately derives direct CPU-root leaves from
   the live local owner and publishes them with f,0,c. Its actual-helper
   fixture passes, including mapping conflicts and publication order.
   This is an observed flow discrepancy, not proof that the missing mapping
   caused black pixels. Do not copy captured aperture addresses or merely
   append the register values; establish the owner/view/extent and full
   service transition before integrating the prepared draw profile.
3. **Upfront uploads still require a temporal proof per new prefix.**
   `m9x_faa_prepare_diag` uploads all known spans before the first submit.
   Preservation of earlier known bytes alone cannot prove that moving a
   later CPU write earlier is safe: a GPU producer could overwrite it before
   its first consumer. The new program, constants and secondary inputs need
   a first-consumption join against the c0 commands. No such overwrite has
   been demonstrated here; this remains an integration proof requirement.

4. **A raw diagnostic index silently aliases other registers.** Mesa
   `m9_faa.c:53` narrows the 64-bit M9X_FAA_BIND_INDEX option to int without
   validation; `m9_draw.c:712` then narrows it to uint8_t. Extracted actual
   initializer/emission code reproduces index256 emitting0 and wide value
   4294967296 storing/emitting0. With BIND_DESC enabled, index255 wraps the
   following descriptor index to0. Root independently reproduced all three
   controls. Validate the wide option before narrowing, and reject an index
   whose paired descriptor exceeds255. This is a raw probe-option admission
   bug; it does not affect the current private kernel profile or establish
   the cause of its black image. Kernel relocation admission may reject the
   malformed binding later. No product change was made in this review.

The earlier raw AD truncation and relocation-array truncation fixes remain
present. Fixed320x240, program1080 and ordinary-output50000 are guarded
private-profile inputs, not independently identified stale live addresses.
The sampled source bases in generated headers are comparison/provenance
values: admitted address operands are rewritten from retained native owners.
No additional literal-address defect was found in the inspected relocation,
clear rebuild or startup owner paths. This bounded audit does not establish
complete vendor-flow equivalence or application FAA support.

## Earlier review: raw extent, repeat masks and incomplete relocation joins

Reviewed REa55bfdbb, kernel8790b0e and Mesaca86fce. The last actual hardware
result remains the250 prefix: it retires with healthy queues, but there is no
qualified FAA image. The returned-public280 profile has passed host checks
and builds; it has not been submitted to hardware.

Confirmed issues and their disposition:

- **Wrong size source and stale bit width, fixed in Mesaca86fce.** The raw
  unbound probe used color GEM bytes for AD. It now computes the prospective
  logical fragment-store extent from live target dimensions, with independent
  128-byte backing alignment. The helper reuses the checked21-bit store encoder;
  its former20-bit clamp truncated the valid capped value100004 tofffff.
  Source-backed320x240 yields9b04. Forced diagnostic overrides remain exact.
  This corrects raw diagnostic arithmetic; no store/resolve is supplied by it.
  The already-correct bound-store path and private hardware profile did not
  use the buggy raw helper, so this is not the cause of their black image.
- **Repeated inactive stack residue, fixed in kernel8790b0e.** The returned
  replacement draw adds a second proven single-target mask block. The generated
  owner33 offsets are AB/route988/1048 and2348/2408. Normalize both blocks' three
  inactive halfwords, after preflighting every block. Preserve active masks
  and the public resolve's legitimate AB/AC zeros. The old single-block helper
  would leave the new inactive residue intact. Active channels were already
  enabled, so removing this residue alone is not a demonstrated pixel fix.
- **Silent truncation in native generation, fixed in REa55bfdbb.** The
  output-template builder zipped operand, encoder and domain arrays without
  equal cardinality checks. The returned-profile strict checker validated only
  the earlier encoder prefix. Dropping the last new encoder was accepted, yielding
  101 rather than102 fixups, leaving the last uniform address unrebased. The
  sealed plan/header contain all102; this is an admission hole, not a defect in
  the current artifact. The shared validator now requires exact lengths, row
  identity, observed payload, tag and original callback joins before generation.
  Both250/280 profiles pass13 missing/extra/identity negative controls with
  byte-identical sealed plans/blobs; the strict returned checker also rejects
  missing/extra terminal rows. Root also checked row joins for all22 historical
  and current manifests.
- **Missing normal-read initialization remains a real contract gap.** Earlier
  source fixtures omit the genuine86660 conversion wrappers and2f899 preparation.
  Their PDEVd4/d8 are absent. Normal Lock uses the sampled resource's3d0/3c8
  staging/conversion path; its3fc is zero, so the embeddedac1f0 path is skipped.
  Historical directac1f0 output is not proof of this normal public Lock flow.
  New source helpers execute the real constructors/preparation before queue
  initialization. Their complete fresh fixture must be qualified as a whole;
  injecting pointers into an older retired capture would not be valid.
- **Application integration remains incomplete.** Preparation allocates surfaces,
  but production draws neither assign job.faa.graph nor initialize that graph
  for execution. Non3d raw companion layouts remain unqualified. These are
  missing functionality, not stale opcode constants.

The current generator retains nonnull program entry offset0 correctly. All
four C2/C3 pairs,16 direct sites and47 timeline fields are joined; no additional
literal captured-address flaw was identified in those current sealed fields.
The fixed320x240 dimensions, format words, two clear segments and50000 ordinary
output capacity remain guarded experimental inputs, not general application
parameters. Live bases, page masks/counts, suffix lengths and timeline origins
are still recomputed from retained owners. Do not globally replace constants
or zero masks without their producer/consumer evidence.

Root verification: actual raw/helper and locked-caller ASAN/UBSAN checks, full
isolated Mesa DSO and rebuilt batch executable pass. Nine real kernel program
tables pass; extracted native code passes128 placement/alignment cases including
both mask blocks and refusal before any write on a malformed later block. The
returned-profile module builds. No hardware test was repeated for this audit.

The sections below are historical review checkpoints; superseded findings are
retained to explain the evidence and earlier fixes.

## Current review: real-table program guard regression

Reviewed kernel ce402c0 (including65cafcc), Mesa c192362 and RE22e9d9fa.
The new kernel has a confirmed admission regression: `m9_faa_program_access`
interprets every encoder24886 relocation as a C2 base needing an immediately
following248ab entry offset. The real replacement table also uses24886 for
standalone kind19 resident-database bindings: secondary offset21c, owner12,
followed by an unrelated248c8 relocation. These are valid source commands.
The staged build therefore returnsEINVAL at the c0 callback before posting120.
This is a deterministic host-side refusal after a posted prefix, not a
hardware-measured wedge. The module has not been deployed. It cannot explain
the older black180 image or the previously retired public1f0 result, which
precede65cafcc. The synthetic pair-only test omitted this legitimate reuse;
the native placement test did not call the program-access helper. Correction
and regression coverage now use the full generated tables as well as malformed
pair controls. The staged ce402c0 module must be replaced before hardware use.

Kernel da3e710 fixes the regression in two passes: validate all entry-offset
pairs and establish their unique program owner, then check orphan page bases
on that owner. Unrelated resident-database bindings remain valid. All seven
real profile tables pass at two placements under ASAN/UBSAN, including removal
of either half of every actual program pair; all23 synthetic controls at two
placements and phased failure controls pass. Root independently ran the
expanded table fixture and built the replacement-profile module successfully.
No hardware run followed this review, and no FAA image success is claimed.

The following existing gaps were rechecked against current source:

- Mesa context preparation never assigns `job.faa.graph` or initializes the
  graph for execution. Allocation alone still cannot enable application FAA.
- The raw `M9X_FAA=1`/STORE=0 path still derives AD from color bytes at
  `m9_context.c:3053`, and `m9_faa.c:296` still calls that the vendor extent.
  Only the bound-store path uses the logical fragment-store extent. This is
  stale diagnostic behavior/documentation, not the private test's AD producer.
- Formats other than0x3d still use generic width*height*8 storage and the
  ordinary depth descriptor. Their layout is unqualified. The prior189→61
  format-alias bug is already fixed by c192362; do not report it as open.
- The emulator mode request uses the adapter-wide miniport owner, whose
  head selector isffffffff. Actual6dd9c rejects head>=4 and skips the count
  store, although6dde1 returnsAL1. The strict observer now stops before that
  unwritten output is consumed. Genuine head publication is missing;
  supplying head0 or a fabricated count would bypass the source contract.
  Further source tracing identifies the earlier GetModes caller11940 and
  IOCTL232304 leading to5801f, which can publish that head on the live owner.
  A separate child object is not established as necessary. Exploratory
  execution now observes that original publisher assign head0 without a field
  patch; the rest of the GetModes sequence remains under investigation. This
  explains the emulator constructor boundary, not the native GPU pixels.

Fixed320x240 geometry, sampled descriptors and50000 ordinary-output capacity
remain explicitly bounded experiment inputs. Runtime source checks, live owner
fixups, page-clear reconstruction and shifted phase endpoints protect their
allocation-dependent fields. No additional literal captured-address flaw was
found in the inspected current paths. The old exactly-two program-binding and
four/five-phase limits have already been removed; the encoder-role regression
above is a separate error introduced while tightening pair validation.

## Earlier review: public continuation and remaining hardcoded assumptions

Reviewed kernel e316f3f and Mesa776e5bf. The distinct public shared-depth
prefix now retires1a0 on M9128 with healthy counters; this is preparation
completion, not a resolved image. The sampled-color retention fix9dee327 is
included in that hardware module. The earlier text below records prior review
states and must not be read as the current deployment status.

One stale structural limit remains in that tested kernel: the phased requester
rejects more than four endpoints. The new public resolve continuation needs
the existing c0/120/140/1a0 boundaries plus its own finish. Kernel e5af628
admits five while retaining complete endpoint/PTE preflight. The actual-C
ASAN/UBSAN fixture checks fifth-phase success, timeout, callback failure,
late endpoint/target refusal and a newly consumed stale page. This removes a
next-profile admission failure; it did not cause the earlier black output.

The [genuine public continuation](parhelia-m9-faa-public-resolve-dispatch-wait.md)
has now executed mode9 rendering dispatch
and published three1c0 last-use deadlines, then stopped at an original
cleanup wait before aa750 returns. The earlier1a0 prefix did not contain
that launch. Its620 appended bytes neither overwrite earlier CPU inputs nor
replace the earlier independent1a0 footer. Kernel151e438 admits the new whole prefix. The strict source checker,
128 actual-C placement/alignment controls and isolated module build pass.
The subsequent fresh-boot151e438 test retires1d0 without wedging, but
main/companion/sampled-primary hashes remain unchanged. Pool state changes.
Actual cleanup/public return and rendered-pixel proof remain separate gaps;
see [the hardware archive](../tests/m9-faa-public-resolve-hardware-20261007/README.md).

The new resolve block emits zero AB/AC words from the actual vendor producer.
Do not apply the older inactive-color-mask normalization there: its proof is
specific to the earlier secondary block and its active-target routing. A
zero state word is not stale merely because another pass uses all-one masks.
Likewise, the fixed private320x240 dimensions and descriptor constants remain
bounded inputs, not a substitute for deriving application surface state.
A separate raw Mesa option bug is confirmed: `M9X_FAA_COMPANION=189`
is compared as189 when choosing allocation/layout, then narrowed and masked
into hardware format61 (`0x3d`). At320x240, the real helper selects614400
bytes/descriptor7a210140 instead of the source-backed614432/7a220140 used by
option61. This is an invalid-option alias into a different layout, not a
measured overrun or explanation of the correctly selected private0x3d image.
Mesa c192362 now rejects the original wide option outside -1 or0..127
before any narrowing, reports the invalid value and resets the whole probe
configuration so AD/AF overrides cannot reactivate it. The extracted real
initializer passes31 controls and three rejected mutations under ASAN/UBSAN;
the full isolated DSO build and batch executable pass. No GPU test is claimed.

No additional literal captured-address defect was found in the reviewed
native owner/fixup/clear paths. This does not close the public readback or
application graph integration gaps described below.

## Follow-up source review: current native and application paths

Reviewed kernel3b39c98 plus the uncommitted in-place-copy profile, and Mesa
dbb76c8. No new literal-address defect was found in the inspected native
fixup, clear rebuild, borrowed-owner or timeline paths. This is a bounded
review, not a claim that every required vendor transition is implemented.

The review identified these gaps (subsequent fixes are recorded below):

- `m9_faa_program_access` and its callback are compiled only for the complete
  output-write profile. Preparation refuses its option for both in-place
  profiles. The original public draw and c0/120 prefixes are unchanged in
  those profiles, so the source-backed C2 access kick also applies there.
  This is an omitted available ordering control; the older complete-profile
  hardware test already proves that adding this kick alone does not fix the
  black image. CPU-aperture side effects remain a separate contract.
- Mesa `m9_context_destroy` calls `m9_faa_store_report` before flushing its
  draw context and waiting for its last queued point. The report scans GPU
  storage and then clears the screen-wide `store_check` flag. Consequently,
  an early context teardown can record incomplete writes and suppress a
  later report. The userspace store lock prevents handle replacement, but
  does not wait for GPU retirement. A correct reporting fix must also cover
  other contexts sharing the store and the atexit path; moving the call
  below one context's wait alone would not close that contract. This affects
  application diagnostic evidence, not the independently retired private
  hardware observations or a demonstrated rendering failure.
- The raw Mesa probe still calls `m9_faa_ad` with color allocation bytes
  when `M9X_FAA=1` and `M9X_FAA_STORE=0`. Its old comment also claims color
  bytes are the vendor extent. The bound-store path overrides this using
  the retained logical main-store extent; the raw path does not. This is
  reachable diagnostic behavior, not evidence of complete application FAA.
- `m9_faa_surface_prepare` allocates the prepared graph, but production
  context code never assigns `job.faa.graph` or marks the graph initialized.
  The initialized graph, pool integration and resolve/publication must still
  be connected before preparation can become an application FAA path.
- Companion probe formats other than0x3d still use `width*height*8` storage
  and an ordinary depth row-span descriptor. Only0x3d takes the source-backed
  companion layout. The other raw-probe format/extent combinations remain
  unqualified; this is not a demonstrated failure of the private0x3d flow.

Follow-up implementation: kerneld249a3e extends the C2 access control to both
in-place profiles; its new copy-wait hardware run logs the kick atc0 and
retires1a0. Mesa776e5bf fixes shared-store reporting with per-owner last-use
tracking and a retirement wait under the owner lock, covering split submits,
multiple contexts and atexit. Failed submission refuses CPU inspection;
failed wait leaves the report pending. Host checks/full DSO build pass.
Neither correction establishes an FAA image.

A further native ownership defect was found after that run. Successful
pending-output profiles kept only the borrowed color address after the
submit caller dropped its pin/lookup reference and the helper closed its
handle. Complete-output profiles kept an extra identity reference but not
the caller pin. Kernel9dee327 now transfers the existing caller pin/reference
on successful FAA submission and releases it outside the transport lock only
after DMA shutdown and cache restoration. Posted-failure quarantine retains
its existing separate path. Inside-submit observations remain valid; older
post-exit raw physical dumps are byte evidence, not proof of a retained color
BO. Actual release/caller ownership checks, peer review and module build pass;
this fix awaits the next hardware candidate rather than repeating the same
rendering test solely for unchanged command bytes.

The strongest unresolved rendering question remains the missing public
dirty-surface lifecycle before the manually invoked internal resolve. A
correctly relocated command sequence below that boundary does not establish
that the selected operation is a complete public FAA resolve.

The source continuation after170 preserves all previously known CPU bytes;
its extra primary commands append rather than overwrite earlier shader,
geometry or metadata inputs. This rules out that specific upfront-upload
hazard, not GPU-state or MMIO-order differences. The uncommitted copy profile
still needs its own integration checks and hardware receipt; its final1a0 is
not a rendering success. The actual phased requester and program-access
guard fixtures pass ASAN/UBSAN in this review. No GPU test was repeated.

Constants need provenance, not indiscriminate removal. The sentinel filler
`0x28041968` is an actual vendor immediate, not a captured pointer. Fixed
320x240 geometry, descriptor words and output50000 remain confined to the
private source experiment. Live address operands, page masks, clear lengths
and timeline origins must continue to be derived independently.

## Confirmed stale values

The private second raster block has one active color target: indexed89 is
fe207ff8 and AB low16 isffff. Its inactive AB upper16 and AC contain captured
stack residue; AC differs282554d6 versus2824fad6 in the passing ordinary source.
These are packed color masks, not GPU addresses. The existing Windows1389a0 /
181fa0..182549 interpretation and first-triangle stream audit establish their
per-target halfwords. Replaying inactive stack residue is unnecessary and
nonportable. The native producer now normalizes only these three inactive
halfwords toffff, preserving the active mask and every other byte.

The generated header carries source-validated owner/offset metadata. Runtime
checks packet framing, complete extents, one-target89 and active AB before any
write; a different target layout or active mask refuses. The source blob stays
byte-identical. This is a semantic rewrite, separate from address relocation.
It removes a stale value but is not a sufficient explanation of the black image:
the active target mask already enables all four channels.

The older application probe computes AD from color allocation bytes, although
the current vendor graph uses logical fragment-store extent divided by32.
The full prepared graph's m9_faa_store_ad helper already has the correct rule.
Mesa2c01eb9 corrects the bound application caller: main/aux allocations use
their128-rounded logical extent, and AD comes from the live group's dimensions
under its store lock. Missing or undersized owners refuse submission; explicit
diagnostic overrides remain explicit. The current private180 image already
uses the source-derived9b04 and is unaffected by that bug.

## Corrected application lifetime gap

The store lock previously covered growth and handle snapshot separately, then
ended before emission and ioctl. There was no broader submission lock. The batch
retains integer GEM handles, not userspace ownership references; another context
could grow the shared group and close main/aux/companion before kernel lookup.
Companion reads also occurred outside that lock. Mesa dbb76c8 now keeps the group
stable from growth through binding, emission and every split-pass submission,
releasing once on all common success/error paths. Kernel references retain
accepted work after unlocking. FAA-off draws do not acquire this lock. The
screen destructor explicitly closes the companion and clears its diagnostic
atexit pointer; diagnostic map reads use the same store lock. These application
lifetime defects do not explain the isolated private180 black image.

## Deliberately bounded diagnostic constants

The private profile's320x240 geometry, output extent50000, format descriptors
and packet offsets remain deliberately fixed. They describe a guarded source
experiment, not arbitrary application FAA support. Address fixups, page clears
and timelines derive from native owners; retaining a source packet offset is
not permission to retain its captured address. The one-target mask rewrite
likewise refuses a different routing or active mask rather than guessing.

## Reviewed mechanisms that agree

- Page clears are rebuilt from live owner bases and logical clear lengths;
  both suffix shifts run backwards after address/timeline fixups. Captured
  page masks are verified as source evidence, then replaced, not replayed.
- Main raw fragment extent136020 rounds independently to136080 at graph
  allocation. The apparent32-byte versus128-byte discrepancy is not a bug.
  Companion clear length97000 is distinct from backing capacity98000.
- Borrowed color admission checks the pinned BO address and extent; source
  SYSTEM allocations retain separately derived GPU mappings and cache policy.
- Constant-red shader bytes and constants equal the passing ordinary source.
  Green preprocessing colors do not determine this constant shader's output.
- The original180 continuation performs only the frontend314 kick before
  CPU copy. No additional omitted post-wait raster/resolve commands appear.
- Full secondary decoding exposes remaining last-explicit-word differences
  in source-owned addresses, FAA graph/mode/extent, sentinel access and inactive
  masks. These words are not a readback of GPU state: indirect restores can
  also change registers, so this comparison is not a pixel proof.

## Remaining causal gaps

The pool is a saved register image, not independently decoded fragment data.
With phased submission, pool A1 and sentinel+8 are now both0x24a; A3/A4 remain
zero. Main and companion stores also receive writes. This establishes GPU
activity, but neither decodes those records nor proves correct FAA pixels.
The manually invoked ac1f0 output flow is still below the unjoined public
surface read/dirty-flush lifecycle. The association itself is present: actual
101050 inserts its node and both returned interfaces publish the dirty outer
graph before the draw returns. The earlier empty-list observation was only
the first lookup, not the state at output. The public embedded graph at+3fc
and this separate outer graph at+420 must not be conflated. No public flush
executes before the explicit internal output recipe; this is a remaining
contract gap, not proof that adding a flush fixes the black image. See
[the passive lifecycle proof](parhelia-m9-faa-public-draw-ownership.md).
The exercised device-global service/MMIO initialization matches native code
so far; the unclosed cold-device path does not justify speculative toggles.
The independent [global initialization audit](parhelia-m9-faa-global-service-prefix.md)
checks all13 exercised prefix writes and manager-before-prefetch ordering.
The [CPU chronology audit](parhelia-m9-faa-temporal-input-audit.md) proves all
earlier known inputs survive in the final image; it does not prove equivalent
ordering of GPU writes or intervening mapping operations. Kernel8d95847 adds an
optional phased-post diagnostic to isolate collapsing source c0/120/160/180
waits. It retains the same command bytes and actual retirement requirements.
The [fresh-boot same-module comparison](../tests/m9-faa-mask-control-hardware-20261007/README.md)
now demonstrates a real ordering effect: phased submission produces601 main
and1404 companion nonzero words, while a single post leaves both zero. The
final output stays black in both. Preserving waits is the default for this
complete diagnostic; later output-mapping publication and resolve remain open.
The previous nine BAR dumps omit the borrowed sampled primary (owner1), which
has no privately owned vram/cpu field. Kernel98dbe6a adds a bounded read-only
observation of the actual pinned/reserved caller BO before submit and after
retirement. It keeps borrowed ownership separate and never invents a zero
baseline after a mapping failure. The retained-store checker accepts this
optional paired observation only when its address matches the submitted color.
The subsequent fresh-boot d76d07d run observes that surface changing from zero
to1308 nonzero words/2819 bytes, FNV04099694. These are retained sampled-surface
words, not a decoded pixel proof.

Kernel d76d07d adds an opt-in test of the remaining output publication order:
reserve the live output address without staging its binding, then publish its
compact and secondary mappings after actual120 retirement, before the160
suffix. The source first references that output at byte5264, beyond the120
prefix endpoint5068. Host tests exercise the actual helpers, including failure
after a posted prefix; the integrated module builds. The
[hardware run](../tests/m9-faa-late-output-hardware-20261007/README.md)
confirms publication after actual120 but still produces an all-zero output;
main and companion hashes exactly match the earlier phased run. Later output
publication alone is insufficient. DMA allocation still happens early, and the native120
wait is stronger than the original CPU deadline110. The intervening C2 CPU
aperture mapping is another remaining chronology difference.

The [resolve resource-role proof](parhelia-m9-faa-resolve-resource-roles.md)
also rules out a simple reversed explicit destination: the successful vendor
path passes the SYSTEM temporary as destination and companion/main as inputs.
The sampled primary becomes the destination in the separate allocation-failure
branch. No explicit primary-to-output copy is justified by this evidence;
opaque indirect color references remain unproven.

The [complete address-encoding audit](parhelia-m9-faa-complete-address-encoding.md)
matches all52 relocations and31 owner extents against the source plan, and
compares the actual native encoder with shipping vendor instructions in310
cases. Two distinct native placements and packed low-bit controls agree,
including paired C2/C3 reconstruction. No additional stale-address or
discarded-bit defect was found in this admitted profile. This does not prove
that the captured flow includes every required lifecycle transition.

## Validation

The actual native C clear/address/mask path passes128 placement/alignment cases
under ASAN/UBSAN. Mask controls reject altered target routing, active mask,
packet headers and short extent without changing data, and verify every other
secondary byte is unchanged. Source checks and the private module build pass.
No new hardware rendering pass is claimed for mask normalization.
The integrated application AD/ownership fixtures pass ASAN/UBSAN and the full
Mesa DSO builds. Public lifecycle checks reject four false observations; the
global source checks reject four altered initialization variants. A threaded
actual-allocation fixture proves competing replacement remains blocked through
emission and both submission passes, including refusal and partial-pass failure;
the integrated Mesa DSO builds. The phased requester independently passes
ASAN/UBSAN timeout, late-endpoint, PTE, placement and clear-length controls.
