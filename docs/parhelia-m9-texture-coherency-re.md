# Matrox M9 texture-write coherency: static evidence boundary

This note isolates the static evidence for an M9 resource write followed by
texture sampling.  It deliberately does not rename common resource/address
records as texture-cache flushes.  Addresses below are image virtual addresses
in the saved 32-bit Windows `MxMD.dll` unless stated otherwise.

The checked artifacts are:

| artifact | SHA-256 |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |
| `m9xdriver-x86_64-1.4.3-80847-20140908-build_98/kernel/KernelLibraries.o_shipped` | `bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7` |

Run `python3 tools/m9-texture-coherency-evidence.py` from the repository root
to check the hashes, packet words, Linux cross-check, and the complete direct
`CALL rel32` census for the named Windows routines.

## Exact texture descriptor path

`0xf4350` is the final four-unit texture/sampler emitter.  Its only direct call
is at `0xf55e0`, inside the common emitter's unit loop.  The emitted groups are:

| dirty group | indices | relevant source |
|---:|---|---|
| common | dynamic `70 + unit` | backend `+0x10` |
| `0x02` | `78,79,7a` | backend `+0x4c,+0x50`; relocation from `+0x44/+0x48` |
| `0x04` | `7c,7d,87` | sampler/format fields |
| `0x08` | `82,83,84,85` | sampler/format fields |
| `0x10` | `80,81,7e,7f` | sampler/LOD fields |
| `0x20` | `c0` | relocation from `+0x3c/+0x40` |

The comparator at `0xf41f0` marks groups by comparing descriptor and sampler
fields.  `0xf4350` clears the backend dirty mask at `0xf4615`.  Neither routine
reads a content-generation counter or receives a signal from the upload
routines identified below.  `0xf4350` does not emit indices `e6`, `e3`, `ef`,
`c2`, `c3`, or `8f`.

There is, however, an independent resource-registration path before the dirty
test [BIN].  At `0xf436b..0xf4382`, binding flag `+0x08 & 0x02` passes the
`+0x44/+0x48` address/backing pair and argument one through TaskMgr vtable slot
`+0x30`.  At `0xf4384..0xf439b`, flag `+0x08 & 0x20` does the same for
`+0x3c/+0x40`.  Only then does `0xf439d` test descriptor dirty dword `+0x00`.
Consequently a clean, unchanged descriptor still registers its sampled
backing whenever either binding flag is present.

TaskMgr vtable `0x1f48e8` resolves slot `+0x30` to `0xfaa40`.  That method
normalizes the address tag, resolves backing tags 2, 3, and 4, and appends the
resolved object to vector `TaskMgr+0x5e4..+0x5e8` when its last argument is
one.  Its call to submission-object slot `+0x84` is not a hidden coherency
operation in this build: TaskMgr initialization at `0x23921..0x23923` installs
the enclosing object, whose vtable is `0x1f2c60`, and that vtable resolves
slot `+0x84` to the exact no-op `0x63c40` (`ret 8`).  Thus `0xfaa40` records
resource membership/lifetime here; it emits no packet and performs no wait.

This proves both descriptor rebinding and independent sampled-resource
tracking.  It does not prove that rewriting content at the same address makes
the descriptor comparator dirty, and it rules out an implicit flush in the
registration callback for this concrete Windows object.

## Render target to sampled texture: exact fence join

The surface emitter `0xfb920`, called at `0xfae0e`, uses the same TaskMgr slot
with the opposite routing argument [BIN].  Each active color attachment
selected by the low four binding bits passes its pair at `+0x38+8*slot` with
argument zero (`0xfb940..0xfb962`).  The depth pair `+0x68/+0x6c`, auxiliary
pair `+0x74/+0x78`, and the additional resources selected by bits `0x800` and
`0x40` are registered the same way.  `0xfaa40` routes argument zero to vector
`TaskMgr+0x5f4..+0x5f8`.

This gives two exact numeric classes and a narrow role inference:

| state producer | `0xfaa40` argument | TaskMgr vector | fence class at commit | role [DER] |
|---|---:|---|---:|---|
| texture emitter `0xf4350` | 1 | `+0x5e4..+0x5e8` | 0 | sampled/read reference |
| surface emitter `0xfb920` | 0 | `+0x5f4..+0x5f8` | 1 | render/depth write reference |

The fence relationship is concrete [BIN]:

1. Commit method `0xf9b70` obtains one 64-bit submission value from
   submission-object slot `+0x74`.
2. It attaches that value to every first-vector object through
   `0xfadc0(..., class=0)` at `0xf9b90..0xf9bc7`, and to every second-vector
   object with `class=1` at `0xf9bd0..0xf9c07`.
3. It calls submission-object slot `+0x94` only after both walks
   (`0xf9c0c..0xf9c17`).
4. On a later 3D state/draw operation, `0xf85c0` walks the sampled first
   vector at `0xf8f20..0xf8f87`.  For each object it calls resource vtable
   slot `+0x04` with argument one, retains the maximum returned 64-bit value,
   and passes that maximum to submission-object slot `+0x6c` at
   `0xf8f8c..0xf8fa0`.
5. The enclosing object's vtable resolves slot `+0x6c` to `0x25270`.  It
   compares the requested 64-bit value with its last value at `+0x100/+0x104`
   and, when newer, emits indexed record `43 = low32(value)` at
   `0x25325..0x25338`, then saves the full 64-bit value.

The concrete slot `+0x94` in vtable `0x1f2c60` resolves to the one-byte
`ret` at `0xa1950` [BIN].  It cannot add a hidden resource fence, flush, or
submission packet after the two class walks.  The fence-class relationship
above is complete at this callback boundary.

The indirect resource callbacks also form an exact pair [BIN].  `0xfadc0`
dereferences each vector entry once and calls synchronization-object vtable
slot `+0x00` with `(low, high, class)`.  The sampled-resource walk in
`0xf85c0` performs the same entry dereference and calls slot `+0x04` with
`class=1`.  The class-1 producer update and consumer query therefore operate
on the same resolved backing synchronization object, rather than on unrelated
TaskMgr-local counters.

Therefore a render task which registers a surface in the second vector stores
its submission fence as class 1; a later task sampling the same backing
queries class 1 through its first-vector entry and emits the maximum dependency
as index `43` [DER from the exact class flow].  This is the recovered vendor
producer-to-consumer ordering mechanism.  Descriptor emission is not used as
the fence trigger: the sampled backing enters the first vector even when its
descriptor dirty word is zero.

Index `43` is an ordering-state write, but static code does not prove a cache
side effect.  Its concrete emitter `0x25270` only compares the requested fence
with the last 64-bit value, emits `43 = low32(value)` when newer, and saves the
full value.  It contains no texture descriptor access or adjacent explicit
cache-state transition.  The packed tag `0x45454543` occurs only twice in the
whole image: once in this emitter and once at `0x65772`, where `0x656c0`
constructs the generic command-state restore template alongside many unrelated
indices.  The second occurrence shows that `43` is persistent/restorable
command state; it does not establish texture-cache invalidation.  Consequently
`43` is sufficient for the recovered vendor fence dependency, while its
sufficiency for making an earlier same-address write visible to texture
sampling remains a hardware/capture question.

Top-level state serializer `0xfadf0` orders its blocks as `0xfc0e0` late state,
`0xfb920` surfaces, `0xf4f50` common state (which contains the only call to
`0xf4350`), `0xf6f20` program/vector state, and `0xfb260` input state, then
clears the context dirty dword.  This proves the state-registration order
within one operation.  A legal render-to-texture transition still requires
the producer operation to be committed before the later consumer operation;
sampling from an attachment still bound for writing is an API feedback case,
not a coherency sequence recovered here.

## Exact resource-copy/upload path

The resource-copy entry `0xb21b0` has four direct callers (`0x55f5d`,
`0x56074`, `0xa5197`, and `0xa57d4`).  It selects among copy helpers including:

```text
0xb283b -> 0x56c60 -> 0x1b8d0
0xb2c29 -> 0x56eb0
0xb2ce4 -> 0x56a10 -> 0x1b8d0
```

`0x56a10` and `0x56c60` adjust packed-mip-tail coordinates before reaching
`0x1b8d0`.  That routine constructs a GPU transfer/blit task and calls the core
emitter `0x1b650` at `0x1b9f5`.  Other copy shapes go through `0x571d0`,
`0x574f0`, and `0x57600`.  At `0xb2e20` or `0xb2e32`, `0xb21b0` stores the
calculated byte offset in destination-resource field `+0x3e0`; the stored
value is an offset, not a Boolean dirty flag.

The source and destination relocation metadata is also bounded [BIN].
`0x1b8d0` calls `0x1b420` once per resource.  That helper supplies the address
record at resource `+0x3c`, chooses relocation backing at `+0x208` or at
`resource->+0x1fc + 0x208`, calculates a 4-KiB-aligned extent, and passes those
three values to `0x1b2b0`.  The latter appends the mapping through the generic
`0x1cab0 -> 0x597a0` or `0x59840 -> 0x5b3c0` relocation paths.  If its
relocation-area cursor would overflow, it first calls `0x1ac40(1)` and resets
the cursor before appending the current resource.  That capacity drain orders
older work only; it occurs before the current copy and cannot serve as the
copy-to-sample completion.  None of these metadata helpers has a direct call
to the known class setter `0xfadc0`.

The copy builder has an exact resource-fence preflight [BIN].  The resource
constructor at `0x50cbb` installs vtable `0x1f3258`; its slots `+0x04/+0x08`
are `0x555c0/0x55550`.  Before emitting the copy, `0x1b8d0` repeatedly invokes
both slots with argument one on each of its two resource arguments
(`0x1b978..0x1b9b1`) until all four calls return zero.  The two wrappers first
forward to as many as two child resources at `+0x320/+0x324`.  If no child
blocks, they fall through to these base-resource methods:

| base method | backing operation at resource `+0x1e4` | numeric channel |
|---|---|---:|
| `0x4e130` | vtable `+0x04(class=2)` query, queue-progress wait when requested, then vtable `+0x00(value,class=2)` | 2 |
| `0x4e250` | vtable `+0x0c()` query, queue-progress wait when requested, then vtable `+0x08(value)` | separate, unnumbered here |

Thus the saved Windows copy route synchronizes both resource objects against
two previously recorded backing-fence channels before it writes.  It does
not query class 1 in this preflight.  Class 2 is an exact numeric argument;
the purpose of the other channel remains unnamed because its getter takes no
class argument.

After those waits, `0x1b650` submits its legacy command queue through
`0x1afd0(1)` at `0x1b811..0x1b815`.  A caller-controlled flag can additionally
invoke `0x1ac40(1)` at `0x1b81a..0x1b824`; that routine submits a short record
and polls queue completion.  Neither call receives the original source or
destination resource object.  `0x1b8d0` then runs the paired `0x1b4a0`
resource/relocation cleanup calls.  There is no post-copy call to the known
TaskMgr class setter in this user-mode route.

The two tails can now be separated exactly [BIN].  `0x1afd0(1)` first emits
the requester's wrap/continuation record when its cached and current producer
pointers differ, calls the queue-post routine `0x1a880`, and finally writes the
resulting raw producer pointer to requester MMIO `0x504` at
`0x1b042..0x1b04e`.  It neither polls completion nor returns a fence value.
By contrast, `0x1ac40(1)` appends the four-argument index-`7c` record through
`0x1ae60`, invokes `0x1afd0(1)`, and then waits until requester `0x508 bit 0`
is set.  Its diagnostic branch additionally requires a stable
`0x500/0x50c` snapshot while polling.  This is a real synchronous queue drain,
not a resource-class update or a texture-specific invalidate.

The fifth argument of `0x1b8d0` is forwarded unchanged to that optional drain
at `0x1b9d2..0x1b9d5 -> 0x1b650 [ebp+0x34]`.  The complete direct caller set
provides this exact policy matrix:

| `0x1b8d0` call site | fifth argument | post-copy `0x1ac40(1)` |
|---:|---:|---|
| `0x499c0` | 1 | yes |
| `0x56b66` | 0 | no |
| `0x56da7` | 0 | no |
| `0x57083` | 0 | no |
| `0x573a3` | 0 | no |

The ordinary rectangle/mip copy family is therefore normally asynchronous:
its command has been posted through `0x504`, but no completion value is
attached to the destination by this path.  One specialized caller explicitly
selects the synchronous drain.  This rules out the earlier possibility that
every `0x1b8d0` operation implicitly waited before returning.

There is one other direct caller of core emitter `0x1b650`: `0x1ba30` at
`0x1bb7d`, itself reached only from `0xbd761` [BIN].  Its final argument setup
forwards descriptor field `+0x48` into `0x1b650`'s optional-drain slot, so this
alternate producer family selects synchronous completion per descriptor.  It
does not change the normal `0x1b8d0` matrix above, and a universally safe new
driver path can request the same post-producer drain regardless of which
front-end built the transfer.

That negative result can be made complete for the known setter wrapper
`0xfadc0`: its only direct call sites in all of `MxMD.dll` are `0xf9bc2`,
`0xf9c02`, `0xf9e88`, and `0xf9ec6`.  The first pair is commit method
`0xf9b70`; the second pair is the alternate 3D commit block
`0xf9e44..0xf9edd`.  Both walk `+0x5e4` with class 0 and `+0x5f4` with class 1
before submission.  None belongs to the copy/upload helper family.  The copy
builder also receives no TaskMgr pointer and never appends its destination to
either of those vectors.

The direct-call census finds no `CALL rel32` edge from `0xb21b0`, `0x56a10`, `0x56c60`,
`0x56eb0`, `0x1b8d0`, or `0x1b650` to `0xf4350`, `0xf4f50`, `0xf5730`,
`0xf59b0`, `0xfc620`, `0xfcb60`, or `0xfc8f0`.  This is a precise negative
result for direct calls.  Together with the concrete virtual-call resolution
above, it establishes that **the inspected MxMD copy path does not explicitly
give the destination a class-1 fence**.  It does not prove that the destination
never receives an equivalent fence: `0x1b420` builds resource/relocation
records before submission, and the lower submission or kernel layer may
associate completion with those records.  No checked static path exposes the
class selected there.  Treating a copy destination as a class-1 producer in a
new driver is therefore a conservative unification with the render path, not
a recovered vendor-copy rule.

`MxMGLX.dll` contains diagnostics for
`TextureObjectMgr::GenericUpdateTextureImage()` and
`TextureObjectMgr::SynchronizeTexture()`.  The strings confirm that its GL
layer has explicit update and synchronization concepts.  Static string
references have not yielded a native M9 packet producer, so they do not join a
GL texture update to any packet below.

## What the common `e6/e3/c2/c3/8f/ef` sites prove

The only direct caller of common-state emitter `0xf4f50` is `0xfae25`.  On its
normal successful path:

1. Common dirty bit `0x02` calls `0xf5730` at `0xf5197` and is then cleared.
2. Common dirty bit `0x04` calls `0xf59b0` at `0xf51a8` and is then cleared.
3. After those conditional branches, `0xf5227..0xf5242` emits the fixed indexed
   pair `e6 = 0x04000001`, `8f = 0x80000600`.

There is no `e3` in that fixed pair.  The `ef = 0` write at
`0xf4f95..0xf4fa6` occurs only when common object field `+0x3e4` was nonzero;
the path first calls whole-common-shadow initializer `0xf4a80`.  This is a
context/common-state reset condition, with no static join to resource content
writes.

Another reset path reinforces that separation.  When TaskMgr field `+0x5d4`
is nonzero, `0xf85c0` marks common dirty groups `0x02`, `0x04`, and `0x80`,
marks each live texture unit's group `0x02`, then clears `+0x5d4` at
`0xf8693..0xf86f0`.  Vtable method `0xf9b60` is the direct setter of this
global reset flag.  No per-resource fence or content generation is read in
that branch.  The common `e6/e3/c2/c3/8f/ef` emissions therefore remain state
reset/address-bank behavior rather than the recovered resource dependency.

### `0xf5730`: common dirty group `0x02`

This routine compares record fields `+0xc4/+0xc8` with globals
`0x20d51c/0x20d520`.

* When the address/domain differs, it optionally registers the resource when
  record flag `+0x08 & 0x02` is set, then emits the `c2,c3,8f` tag and applies
  relocation selectors `0x13` and `0x14` to the `+0xc4` address record.
* When it matches, it emits `e6 = 0x04000001`, obtains `e3` dynamically from
  the command-buffer callback at vtable offset `+0x70`, emits `c2 = 4,c3 = 0`,
  and follows with `+0xec` entries from the array at `+0xf0`.

Its only direct caller is the common dirty-bit path above.  No upload helper or
texture descriptor object reaches it through a direct call.  The packet is an
exact resource-bank transition, but the static evidence does not identify it
as a texture resource or a texture-cache invalidate.

### `0xf59b0`: common dirty group `0x04`

This routine optionally registers the two address records `+0xd0/+0xd4` and
`+0xd8/+0xdc` when `+0x08 & 0x04` is set.  It emits `8c,8d,8e,8f` from
`+0xcc` and zero payloads.  One variable-bank branch emits
`e6 = 0x04000001`, a dynamic `e3` from the same callback class, and entries
controlled by record counts `+0xf8` and `+0x2fc`.

Its only direct caller is the common dirty-bit path above.  Like `0xf5730`, it
has no direct static join to the texture upload or sampler emitter.  Calling it
a texture-cache transition would therefore exceed the evidence.

## Late address-domain transitions are a separate family

The complete direct callers of the late helpers are:

```text
0xfc620 <- 0xfc3c9
0xfcb60 <- 0xf9080, 0xfc378
0xfc8f0 <- 0xfc365, 0xfce79
```

`0xfc620` and `0xfcb60` operate on late-state address records at `+0x58` and
`+0x74`.  They compare low address bits and upper domain bits against
`0x20d51c/0x20d520`, register relocations, and use selectors `0x2c/0x2e` for
indices `2e/2f`.  Both routines scan task resources at emitter fields
`+0x5e4/+0x5e8`, select a maximum completion value, and wait/join it through
virtual callbacks before emitting the transition.  Their fixed packet bodies
are:

| routine | fixed body |
|---|---|
| `0xfc620` | `e6 = 0x42000000`; eight index-`44` values of `0x24081913` total, with the eighth bundled in the final `44,2e,2f` packet |
| `0xfcb60` | `e6 = 0x42000000`; eight index-`44` values of `0x28041968`; two later `e6 = 3`; then `2e,2f` |

`0xfc8f0` emits address-pair transitions through indices `1a,42`, with observed
payload forms `0xa1000009`, `0xa3000009`, and `0xa4000009`; one branch ends in
`e6 = 0x40008000`.  It uses relocation selector `0x30`.

These facts support the narrower description **address-window/domain
synchronization with resource completion ordering**.  The first vector is now
known to include sampled texture resources, but the late-state caller family
is still disjoint from the ordinary texture descriptor call family.  Static
analysis therefore does not establish any of these helpers as a per-update
texture-cache operation.  They may still be necessary when a resource changes
address domain.

## Linux cross-check prevents a false `e6` conclusion

The saved Linux `KernelLibraries.o_shipped` contains the little-endian literal
`0x04000001` nine times.  A resolved non-texture occurrence is in global
function `m9x193` at `.text+0x2c2e9`: record mode 2 stores `0x04000001` and
then writes computed resource-address words.  This proves that the value is
also a resource/requester record and is not unique to texture sampling.

Nearby `m9x1011` at `.text+0x2bf33` places `0x6dc00000`, `0x42000000`, and
`0x000000a1` behind an `e6,e6` tag in another address transition.  Index `e6`
is a polymorphic command/state port.  Neither the index nor
`0x04000001` alone names a flush.

Linux `m9x954` independently confirms task-level fence ownership, but does not
add the Windows class split [BIN].  It obtains a submission value through its
vtable slot `+0xd0` at `.text+0x2d9f3..0x2da00`; for each type-zero referenced
resource it follows `resource+0x38`, then `+0x228`, and stores that value at
backing `+0x60` (`.text+0x2da45..0x2da53`).  This supports retaining and
fencing producer backing objects.  The checked Linux path has a single vector
at `+0xf8..+0x100`, so it does not independently prove which fence field a
texture consumer must query or whether a cache invalidate accompanies it.

## Defensible driver sequence before a differential capture

For render-to-texture followed by sampling, the static code now supports this
specific sequence:

1. Register the render/depth destination in the class-1/second resource vector,
   submit the producer, and attach its submission fence as class 1.
2. Register the later sampled backing in the class-0/first vector even when
   its descriptor is clean; query its class-1 fence, take the maximum sampled
   dependency, and emit that dependency through index `43` before submission.
3. Re-emit the `78,79,7a` texture address/descriptor group when the backing,
   offset, layout, or address domain changes.  Re-emitting on every content
   generation is a conservative experiment, not a recovered vendor rule.
4. Preserve the normal common-state `e6 = 0x04000001,
   8f = 0x80000600` pair while testing, because the Windows driver emits it on
   the successful common 3D path.  Do not describe it as a texture invalidate.
5. Use the `fc620/fcb60/fc8f0` family only for the address-domain transitions
   its object and relocation flow requires.  Injecting it after every texture
   write has no static justification.

For a copy/upload producer, use the same conservative class-1 destination
fence only as an experimental unified scheduler policy; it is not the vendor
copy contract.  The evidence-backed safe bring-up sequence is instead to use
the recovered `0x1ac40(1)` drain after posting the copy and before submitting
the sampling task.  This waits for requester quiescence and avoids inventing
either a class-1 copy completion or a cache-packet meaning.  Once a capture
shows that an asynchronous copy and later 3D task share sufficient FIFO and
visibility ordering, the drain can be replaced by that narrower dependency.
MxMD itself pre-waits copy resources through class 2 plus the separate legacy
channel and does not attach class 1 after the copy.  The Windows render-target
sequence gives task ordering and descriptor visibility, but still does not
prove whether index `43` completion also implies all texture-cache visibility,
or whether same-address render-to-texture needs an additional cache control.

## Remaining boundary and required capture

Static analysis has recovered render-surface class-1 fence production and the
later sampled-resource class-1 dependency, but not an exact dedicated
texture-cache invalidate, nor proof that one exists.  It also rules out an
explicit MxMD class-1 destination attachment in the inspected copy path.  The
copy emitter itself is now closed: its normal tail only posts requester
`0x504`, while its optional tail drains through index `7c` and `0x508 bit 0`.
The remaining lower-layer possibility is bounded to asynchronous requester
ordering or kernel processing of the generated relocation metadata; no
checked user-mode metadata helper attaches class 1.  The unresolved boundary
is cache visibility after the index-`43` dependency and whether an
asynchronously posted copy becomes visible to a later sampling task without
the recovered full requester drain.
A differential command/MMIO capture must compare at least:

* sample without an intervening write;
* GPU blit/upload to the same backing followed by sample;
* CPU lock/update to the same backing followed by sample;
* render-to-texture followed by sample;
* the same cases with unchanged and changed backing address/domain.

For each pair, capture the complete producer tail, task submission boundary,
consumer prefix, index-`43` dependency, relocations, and waits.  Diff the
occurrences of `e6/e3/c2/c3/8f/ef` while holding all sampler and descriptor
fields constant.  The smallest decisive render-to-texture pair keeps the same
backing/address/layout and changes only whether a render write precedes the
sample; index `43` should change with the producer fence, while any additional
packet is a cache-control candidate.  That experiment can distinguish an
ordinary dependency, an address-domain transition, descriptor rebinding, and
a true content-coherency operation.  Until then, assigning cache semantics to
any one of these packet words is unsupported.

## September 2026 live FBO producer/consumer closure

The focused vendor-GL capture in
`captures/m9x-linux-gl2-matrix-20260914/fbo-chain` executes a complete 64x64
RGBA8 FBO render followed by a GLSL `texture2D` draw. Producer sequence 14
binds `b0=0x11f21000, b1=0xc8010040`; consumer sequence 15 binds the exact
same pair at texture indices `78/79`. Producer post-state reaches
`43=0x350`, while the later consumer raster state emits `43=0x3e0` before its
draw and advances `e3` to `0x3f0`. Because the draw has other resources,
`0x3e0` is the maximum live dependency and is not assigned solely to the
texture.

A separate synchronized validation reads producer-center RGBA
`(3,130,130,255)` and sampled-consumer center `(2,128,128,255)`. Thus the
statically recovered class-1 resource ordering is now joined to a live exact
surface-to-texture address transition and verified visible content. No
additional texture-specific cache packet appears in the tested chain.
