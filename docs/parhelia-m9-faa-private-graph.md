# M9 FAA private graph admission and execution gate

Status on 2026-10-06: the retained vendor frame joins real CPU allocation,
clear, binding, public DP2 triangle and resolve instructions, but does not yet
prove that the triangle renders through the same FAA graph. No executable
Linux indirect-pool admission follows from this trace.

## Concrete surface membership gap

The SHA256-pinned MSerX `MxMD.dll` activation entry `a77c0` accepts the actual
surface wrapper in EAX and no stack arguments. It reads the surface at
`wrapper+c0`, then the device at `surface+238`. The real caller `7d030` invokes
this entry at `7d098` when its CPU request bit `02` is set. This is a normal
surface activation caller, separate from a fragment-mode register write.

The activation entry performs these actual instructions:

| PC | Action |
| --- | --- |
| `a77e7` | Set `surface+428` bit `08`. |
| `a77fb` | Set `surface+270` to one. |
| `a780d` | Require the selected allocation profile's class to equal two. |
| `a7817` | Load the surface's metadata owner from `surface+420`. |
| `a7827` | Set metadata `+3c` admission flag to one. |
| `a7848` | Look up the actual backing object in the device registry. |
| `a785e` | Join an existing registry owner when one already exists. |
| `a787d` | Call `ac7e0` with EAX=`metadata+1c`, EDI=device and args `(surface,0)`. |
| `a7886` | Clear metadata `+3c` if allocation/initialization fails. |
| `a78cc` | Publish the initialized owner through the genuine registry insertion. |

The other actual `ac7e0` callers at `51ef2` and `b6a19` also initialize
`[surface+420]+1c`; they do not construct a detached scratch graph.

The frame helper retained as `b954cf25` instead allocates a separate 32-byte
graph, calls `ac7e0` directly, manually calls `aa750`, then executes a normal
public DP2 request. This omits surface membership and the normal admission
publisher. A plausible explanation for its later resource overwrite is that
the public context sees an ordinary surface owner. The corrected emulator
must execute `a77c0` or its actual normal caller and recover the graph from the
returned surface owner. Assigning the missing flags or graph fields in Python
would not prove the vendor publisher.

## Actual triangle overwrites in the retained frame

### Genuine handle publisher for public render-target binding

The public context constructor subsequently reads `primary+268` (the resource
wrapper), then wrapper `+bc` (its registered handle) at `755aa..755b3`. A plain
`a3870` allocation leaves that handle zero. The constructor therefore cannot
find the actual render target in its DirectDraw-local registry even after
`a77c0` successfully publishes a surface-owned FAA graph.

The real public association callback is `bf0e0`, advertised at D3D owner
`+cc`. It checks PDEV `+30` at `bf18f` before invoking the handle publisher.
Current retained allocator/queue/frontend owners leave this Boolean zero; an
actual callback call correctly returns `8007000e`. The genuine `2f750`
publisher writes it to one at `2f80e` only after all 17 module initializers
from table `1f2480` have returned successfully. No PDEV flag is patched to
bypass this remaining full startup admission boundary.

A bounded real producer is sufficient to investigate subsequent CPU binding:
`7a770` takes the actual already-allocated primary in EAX and the real
DirectDraw-local registry as its single stack argument. The real `beb40` and
`c04a0` create and recover that registry using the explicit OS caller's
DirectDraw-local key. `7a770` invokes `a3710`, which constructs the wrapper;
`a4010 -> a9ab0` joins wrapper `+c0` to the actual resource and resource `+268`
to the wrapper. `885e0` publishes the wrapper in the registry and returns the
handle which `7a7d7` writes to wrapper `+bc`. None of these driver outputs is
assigned by the host.

`tools/faaemu/m9_surface_associate.py` and
`tools/m9-vendor-surface-associate.py` execute this bounded producer and verify
the actual returned wrapper, nonzero handle and table lookup. The retained
receipt reports `public_callback_executed=false`, actual OS version `700`,
handle one and the observed publisher PCs. The OS pointer structures and
surface MORE handle are explicit CPU caller inputs. The public-callback mode
is separate and requires its genuine full startup prerequisite.

The returned DirectDraw-local key and surface local/global objects must be
reused by public context creation; creating new caller objects silently
selects a different registry. This proof closes the handle publisher, not
the FAA triangle's final graph association, GPU rendering or completion.

### Detached-frame negative baseline

The frame's binding selects the genuine graph companion and its HZ. Its
triangle then writes different bindings before the operation:

| Register | FAA bind | Triangle | Meaning |
| --- | --- | --- | --- |
| `B8` | `01a51001` | `012a8801` | Companion address changes. |
| `B9` | `7a220140` | `b8002000` | Companion descriptor changes. |
| `BA` | `01ae9001` | `00000000` | Graph HZ address is removed. |
| `BB` | `f0010140` | `f0010040` | HZ descriptor changes. |

The fragment mode remains `1f`; checking mode alone misses all four changes.
The frame's main/auxiliary BC/BD and pool6 address can still be retained while
the depth/companion graph has already changed. The initial bind's association
with the exported graph is independently checked by
`tools/m9-faa-surface-admission-check.py`. Its `--require-coherent` option
requires the companion, HZ and their descriptors, main, auxiliary and pool6
bindings to survive the final pre-triangle lowering. The existing retained
frame fails this gate, as expected. Success would remain a CPU binding proof,
not a hardware image or a completion claim.

The triangle system-DMA image uses a distinct type2, count-prefixed software
representation. This audit reads those records as semantic register writes;
it does not copy them into the Linux count-free command stream. The actual
vendor submitting consumer and its packing rules must remain explicit.

## Smallest coherent Linux execution gate

The first gate is a private trusted diagnostic frame. It must cover
initialization/clear, retained bindings, GPU draw, resolve and an actual final
completion. The generic client FAA_POOL relocation remains non-executable;
generic `2e/2f` indirect execution and `1a` register stores remain refused.
The existing nine-word binding block is only one step in that full pipeline.

A private owner can reuse the existing kernel diagnostic allocation and
transport patterns. Allocate and pin its VRAM graph before entering client BO
WW reservation. Keep pool6 and the sentinel unexposed: no GEM handle, mmap,
SURFACE_ACCESS, PRIME export or client-provided physical address. Private BOs
can be accessed only by fixed kernel templates patched with checked offsets,
address tags and extents. Do not reuse the ordinary system restore pool as
this FAA context resource.

The exact pool6 allocation in the joined capture is `0x580` bytes although
the ordinary cache request is `0x500` bytes. Its actual fragment pair is
`+84/+88 = [0,GPU address|1]`. The sentinel pair is `+7c/+80` and uses an
indirect CPU record; its relocation resolves to the separate private VRAM
sentinel. The short-lived shifted `+88/+8c` exporter interpretation was
corrected and must not enter an implementation.

Actual sentinel initialization supplies five DWORDs:
`a4a3a1a1,28041968,0,0,0`. The first two are CPU-written program/header bytes;
the last three are initialized snapshot slots. The genuine context producer
stores A1, A3 and A4 to sentinel offsets `08/0c/10`, then restores the private
pool program range at `284..2c4` with the captured low tags. The sentinel's
first-use restore precedes the FAA graph bind. These are distinct programs
and lifetimes; their contents cannot be inferred from unrelated clear bytes.

Before executable construction, join an actual CPU write-validity mask for
pool6, or prove the exact GPU producer which initializes every later fetched
program byte and its ordering before the first restore. Untouched zero-filled
emulator backing is unknown. Mapping validity must follow the actual CPU BAR
alias; a raw GPU offset is insufficient if mapping introduces an offset.
Do not fill unknown program slots with zero or a guessed nop.

The owner must retain every referenced program/source operand plus color,
main, auxiliary, companion, optional companion HZ, pool6 and sentinel until
the completion **after resolve**. Intermediate triangle/E3 tokens are not a
release point for the graph. Subsequent context consumers need the graph's
same lifetime even if the first triangle has completed. No CPU rewrite or
replacement of the indirect records is allowed while those consumers remain
posted. A posted timeout quarantines all graph resources until observed
retirement or PCI DMA stop, using the current transport/fence contract.

Keep FAA queueing disabled during the first image proof. On teardown, stop
DMA before releasing unresolved resources, signal retained reservation fences
before the final TTM put, and release BOs outside the transport lock. These
are the ownership rules already proved for ordinary submissions; FAA cannot
bypass them merely because the command templates are trusted.

Only after the private frame produces an FAA image and retires correctly
should the reusable helpers gain a file-bound graph API. No placeholder graph
token, app ioctl or writable opaque-program BO is introduced in this round.

## Normal render-target validator owner, bounded MM dispatch

The genuine surface association now lets public context creation retain
registry handle 1. Its next target-validation consumer at `75788` calls
`hardware+fea8[head]` through virtual slot `+8`. This owner is published by
ordinary module `20810(device)`: constructor `214e0` creates four real
`1f2c3c` objects; `208fd` publishes them and `20982` selects `device+6998`.
No synthetic vtable or direct pointer assignment is used.

The first constructor calls `21620`, which emits actual miniport request
`2322a0`, input 28 bytes, output zero bytes and a null output pointer. The
retained request is `[1,0,0,0,0,1,1]`: four Boolean capabilities, head index,
and two Boolean policy flags. The scoped host bridge forwards these values
to genuine MM dispatcher `7165f` and handler `16a49`, returning only its real
result and returned-byte count. This is a new reviewed scalar request; it
does not admit arbitrary I/O or provide an allocation/completion reply.

Actual execution then reaches `16a49 -> 1eb33 -> 1f331/1f360`, which reads
BAR2+`1800` display-controller control. The current emulator refuses that
uncaptured input. `target-owner-boundary.json` records this honest boundary;
its MMIO event's read `value=0` is the hook's placeholder, **not an observed
register value**. The function has not returned and its four target owners
have not been published. A captured initial control input is the next proof,
with any later hardware access remaining a separate explicit boundary. This
negative receipt is retained as the pre-capture baseline.

The MM private target box is `20231850`, while the MD shared target box is
`202305ec`. Their different ownership is observed through the real
dispatch/constructor fields. No wholesale heap copy or strategy-pointer
mirroring is needed or performed; doing so would overwrite genuine MD
state with unrelated MM objects. This owner proof remains CPU-only and is
not FAA draw coherence, native execution or an FAA image.

The subsequent captured-input replay closes this constructor boundary.
`captures/m9128-device-inputs-20261006/dc-banks-idle-e4611b75.csv`, supplied by
the sole target owner, has SHA256
`d457b823156d033a9ea84c87bae1d311f8dfede8aef991ebd8ec5b657f6bf8db`.
It records 128 aligned DC-bank values; active DC0 control is `33046001`.
`initialize(..., dc_snapshot=path)` admits only this pinned capture, initial
reads, and the finite control save/disable values used by actual `1f2ac`.
The portable consumer already exists in canonical `device-inputs.py`
(`7e8c1c7a`); no uncaptured hardware input or out-of-tree runtime patch is
needed. The replay only reads DC0 control twice, with no MMIO writes.

The actual function now returns 1, publishes the four `1f2c3c` validators,
and sets `device+6998` to the current-head owner. It also publishes its fifth
shared owner at `hardware+feb8` and selects it through `device+699c`.
This fifth owner uses target selector 4, genuinely produced at `20930`; it
is not a fifth display head. Four unchanged MM `7165f/16a49` calls return
0 with status 0 and returned-byte count 0. The final two scalar requests
disable the shared/current target strategy as part of normal `20a80` setup.
`target-owner-returned.json` retains all four requests, owner assertions,
observed publication PCs and the two captured reads. This is sufficient to
join the normal vendor draw validator; retained FAA draw bindings and a
native FAA image remain separate gates.

## Corrected normal counted-image contract and kernel producer

The normal surface-owned frame (`0050ee32`) exports two independent images
from the same live contiguous DMA owner (`01042000`, extent `41000`):

| image | bytes | count groups | primary words | terminal |
| --- | ---: | ---: | --- | --- |
| scratch quad | 604 | 35 | `45452d2c,01042002,0104225e` | `23000001,0,4` |
| FAA triangle | 772 | 44 | `45452d2c,0104225e,01042562` | `23000001,0,3` |

Their unchanged SHA256 values are respectively
`c1d45b1d65dcf4109b2d644d5bbc29da86b13088e452fcea2efac949b9d55536`
and `a4df84b2af6c68c50da75e8787d3688ca91175866904f4a09f0628e2deaf8475`.
The combined extent is `560` bytes. Every exported byte matches its actual
CPU write, not untouched backing. Both start pointers select physical DMA
with low tag2; both ends use saved-kind1 mode2. The second launch retains the
real FAA companion/HZ/main/auxiliary graph and main capacity. The first is
the genuine scratch pass, not a substitute ordinary triangle. This is
recorded in `normal-counted-submit.json` and checked by the normal graph
checker plus actual `24b10..24d45` observers (`bd8ef694`).

Kernel candidate `1326475` introduces `m9_private_stream_range`, an internal
bounded owner-span encoder. The existing native-proved user-secondary path
uses its class1 GPUVA variant, with identical pointer words. The new physical
DMA32 variant produces the two exact normal exports above. It validates the
complete owner extent, aligned nonempty member range, domain and 32-bit end
before writing any output. It never accepts userspace addresses or copies,
strips or reforms a counted image. Its `m9_private_stream_owner` must be
constructed from an already allocated private command owner; it is not a
new app ABI or proof that the owner has retired.

`check-m9-private-stream.py` compiles this actual C header and compares both
captured exports, existing class1 framing, aligned end bounds, overflows,
invalid domains and unchanged outputs on failure. Sixteen checks pass. The
full Linux7.2 module builds with the usual existing objtool/pahole warnings.
The physical-DMA private variant has no executable FAA caller yet; generic
FAA_POOL execution remains refused. This component is preparation, not the
full private graph or native FAA completion.

### Full-frame owner and ordering requirements

The corrected pool6 `24f20` sequence is **later**, during the resolve
transition through `fc8f0/fc99b`. It is not a pre-draw pool initialization.
The first triangle's `fb920` supplies its counted pool binding; resolve then
emits the raw 76-byte state preparation and pool range `284..2c4`, followed
by its raw pool/FAA rebind. Preserve this exact producer/read order. The
pool constructor supplies no CPU memset. Opaque bytes may be GPU-produced
state, so missing CPU initialization cannot justify filling them with zero
or inserting a new earlier clear. Join the actual GPU state-save producer
and following reads before declaring this graph executable.

The smallest diagnostic lifetime can reuse device-owned private resources,
as the existing kernel triangle diagnostic does. Its graph and command
storage remain unexposed and retained until device teardown stops DMA.
Before any CPU rewrite or resource replacement, hold the transport lock and
drain **all** existing owners; refuse writes when wedged. Prepare VRAM/TTM
allocations before taking any client reservation/transport execution locks.
Preserve both draw images and their source/program operands through the
completion after the raw resolve; a first-draw token cannot release them.
If the diagnostic writes an externally owned color BO, retain its ref/pin,
reserve it, wait foreign dependencies and publish the true final write fence
before posting, using the current completion/quarantine contract. No generic
client-indirect execution or file-state ABI is needed for this first image.

The remaining integration inputs are the corrected normal GPU pool-save/
restore ordering, the exact shader/constant/source operand owners and known
CPU bytes behind every fetched range, and the trusted raw clear/resolve
producer. The counted pointer form alone cannot invent those templates or
prove the full graph's lifetime. Subsequent dynamic contexts should use
explicit private graph owners retained by final fences and released off
the transport lock; that extension is not silently provided by this encoder.
