# M9 MRT routing and retirement boundary

This note isolates the remaining multiple-render-target failure in the saved
M-Series stack. It uses only the in-tree Windows and Linux binaries and the
preserved MRT result. No live target was accessed. Machine-code claims are
checked by `tools/m9-mrt-retirement-evidence.py`.

Confidence labels follow the other M9 notes: **[BIN]** is directly present in
the binary, **[CAP]** is preserved observed output, **[DER]** follows
deterministically from those facts, and **[OPEN]** names a missing join.

## Result

The Windows mini-driver has a complete structural four-target path. It lowers
four independent attachment declarations, emits four address/descriptor
pairs with relocation selectors 2 through 5, packs a format class for every
slot, compiles target-local color masks and blend fragments, registers every
active target as a render-write resource, and attaches one submission value
to all registered render-write resources before submit **[BIN]**. RT1, RT2,
and RT3 therefore belong to the same submitted Windows job and retirement
class as RT0 when enabled. A generic “only RT0 gets an address or fence” model
is false for the Windows implementation.

The preserved Linux run does not identify an RT1 readback failure. Mask `0x2`
returned from `glFinish`, printed `MARK mrt-02 error=0x0`, and then stopped
before the one aggregate result line **[CAP]**. The fixture next begins a loop
at attachment 0, with no marker around either `glReadBuffer` or
`glReadPixels`; the stop could be in any read-buffer selection, readback, or
later iteration **[DER]**. Mask `0xf` with readback disabled also returned from
the finish marker, then spun in the kernel while `/dev/m9x` was closed even
when the process used `_exit` **[CAP]**. Readback is therefore not required to
trigger the retirement failure.

The exact static wedge boundary is the Linux resource destructor. `m9x102`
calls `m9x880(resource, 1)` in an unconditional, timeout-free loop until it
returns zero. `m9x880` loads the target fence from the backing object at
`+0x60`, invokes `m9x653` with its fourth argument zero, and calls `m9x641` to
compare that fence against the memory completion writeback **[BIN]**. With the
zero fourth argument, `m9x653` skips its path that can obtain a new submission
value or invoke submission slot `+0x100`; the destructor loop only updates the
wrap-aware completion view and polls **[BIN][DER]**. A missing or impossible
producer completion can consequently spin forever.

The first of the two joins is now closed, and not statically: the native
destination that routes a colour export to RT0..RT3 was MEASURED on the card
on 2026-09-19 as the pair `0x2c + 4p` / `0x2d + 4p`, by binding four targets
with every write mask open and asking which surface a single output's
destination reached (`docs/parhelia-m9-mrt-routing.md` section 2).  The second
-- proof that every *Linux vendor* MRT attachment enters the `m9x954` resource
vector -- remains absent, and is moot for the new driver, which replaces that
ABI with its own explicit per-job BO list: every colour attachment is added
through `m9_batch_add_reloc` with `COLOR` and read/write access, which is what
section "Safe contract for the new driver" asks for.

`docs/parhelia-m9-mrt-routing.md` carries the measurement.  The three MxMD
sites that build a target's two virtual output registers as `0xffec + 2p` and
`0xffed + 2p` are still the only static evidence; the DERIVED reading of them
as a physical `0x2d + p` was **wrong** -- `0x2e` reaches no surface -- and the
physical step is four, not one.  That note also closes the per-target write
mask, which lives in raster `ab` and `ac`, from the 2026-09-17 captures.

## Preserved probe boundary

`tools/m9x-gl2-core-result-matrix.c` creates four distinct 32 by 32 RGBA8
textures and attaches them to `COLOR_ATTACHMENT0..3`. Its fragment shader
writes red, green, blue, and yellow to `gl_FragData[0..3]`. For each mask it
first binds all four buffers and clears them with the fixed-function path,
then selects the requested draw-buffer mask, draws the shader, and calls
`glFinish`.

The relevant preserved outcomes are:

| mask | finish marker | read result | teardown |
|---:|---|---|---|
| `0x0` | returns | all attachments zero | completes |
| `0x1` | returns | RT0 is red; RT1..3 zero | completes |
| `0x2` | returns | no aggregate result line | not reached separately |
| `0xf`, reads disabled | returns | deliberately omitted | kernel spins closing `/dev/m9x` |

The mask `0x2` line establishes only that the API finish call returned. After
that marker the first loop iteration selects attachment 0 and reads it; there
are no markers between the four iterations. It is not evidence that
`glReadPixels` of RT1 is the failing operation. The no-read `0xf` result is the
stronger isolation: an outstanding resource or cleanup submission survives a
successful finish call and blocks file teardown.

The saved `libGL.so.2.0.0` does not close this gap statically. `glFinish`,
`glReadBuffer`, and `glReadPixels` are indirect dispatch thunks at `0xfb480`,
`0xfa9b0`, and `0xfa8e0`; their concrete context methods are not joined to the
stripped resource records in the saved image **[BIN][OPEN]**.

## Four color attachment descriptors

The MxMD draw lowerer at `0x181fa0` consumes four-byte fragment-output
declarations. The low 16 bits select compiled resource `r`; the slot is

```text
s = ((((declaration >> 16) & 0x3f) - 0x10) >> 2),  s in 0..3
```

and low value `0xfffe` is unbound. For every bound declaration it copies
compiled resource `+0x1bc+8*r` and relocation backing `+0x1c0+8*r` into
surface-shadow `+0x38+8*s`, copies descriptor `+0x1fc+4*r` into
`+0x58+4*s`, grows the active count at `+0x34`, and sets dirty bit `1<<s`
**[BIN]**. Address copying retains the high three tag bits while replacing the
low 29 address bits.

The emitted target map is exact. The `fb920` loop at `0xfbaa3..0xfbb98` starts
at state byte offset `0x2c4`, advances eight bytes per slot, loads descriptors
from surface shadow `+0x58+4*s`, and submits the tagged address at
`+0x38+8*s` with relocation selector `2+s` **[BIN]**:

| target | state indices | MMIO address/descriptor | surface address | descriptor | reloc selector |
|---:|---|---|---:|---:|---:|
| RT0 | `b0,b1` | `0x12c0,0x12c4` | `+0x38` | `+0x58` | 2 |
| RT1 | `b2,b3` | `0x12c8,0x12cc` | `+0x40` | `+0x5c` | 3 |
| RT2 | `b4,b5` | `0x12d0,0x12d4` | `+0x48` | `+0x60` | 4 |
| RT3 | `b6,b7` | `0x12d8,0x12dc` | `+0x50` | `+0x64` | 5 |

Emitter `0xfb920` pairs their address relocations with selectors `2..5`.
Common-state index `89` at MMIO `0x1224` carries one three-bit render format
class at bit position `3*s` **[BIN]**. This is enough to implement target
addresses and formats without guessing.

## Draw-buffer and color-output routing

The MxMD program builder constructs target-local fragments. Routine
`0x1389a0`, called inside the per-target loop at `0x137488`, translates each
target's RGBA write enables to masks `0x4444`, `0x2222`, `0x1111`, and
`0x8888`. Blend compiler `0x170ff0` is called once per target and its result is
appended to the target program at `0x1375bc..0x1375c5`. It tests the target bit
in the low byte of state `+0xf8` at `0x1710c7..0x1710ed` **[BIN]**.

That `+0xf8` field is the target-local blend/output enable state, not a proven
direct representation of the OpenGL `glDrawBuffers` array. MxMGLX contains
the `gl_FragData`, `gl_FragColor`, and `gl_MaxDrawBuffers` frontend symbols,
while MxMD consumes the four declaration slots and constructs per-target
program fragments **[BIN]**. The private record joining a particular GLSL
output location to its final native color-export selector has not been
recovered **[OPEN]**. Descriptor fan-out alone cannot prove that oC1..oC3 use
the correct native endpoint.

This unresolved selector can explain a command stream that is structurally
valid yet fails to retire after RT1 is enabled, but the current evidence does
not distinguish it from a resource/fence membership error.

## Windows resource and fence contract

`fb920` loops from slot zero to surface `+0x34`, tests `1<<slot` against the
low four dirty/active bits, and calls TaskMgr vtable slot `+0x30` with the
slot's relocation address, backing object, and routing argument zero for every
active color target **[BIN]**. TaskMgr helper `0xfaa40` routes argument zero to
the second resource vector at `+0x5f4..+0x5f8`. Commit routine `0xf9b70`
obtains one submission value, assigns class zero to the first vector and class
one to every entry in that second render-write vector, then calls submit slot
`+0x94` **[BIN]**.

The Windows contract is consequently:

1. Install every enabled target's tagged address and descriptor.
2. Add every target backing object to the render-write resource vector.
3. Retain those objects through submission.
4. Attach the same producer submission value, fence class one, to every
   registered target before submission.
5. When an object is later sampled, query its render-write fence and emit the
   resulting maximum dependency before the consuming job.

This contract rules out a Windows design that fences RT0 alone. It does not
prove the saved Linux implementation populated an equivalent vector for every
MRT attachment.

## Linux submit and destructor paths

Linux `m9x954` checks two pointer vectors for non-emptiness: `+0xf8..+0x100`
and `+0x208..+0x210`. If either contains entries, it obtains a submission value
through vtable slot `+0xd0` **[BIN]**. Its visible backing-fence loop walks only
the first vector. For each entry whose type at `+0x7c` is zero, it follows
entry `+0x38` to resource `+0x228`, stores the common submission value at
backing `+0x60`, decrements the entry reference at `+0x74`, and clears cached
aliases **[BIN]**.

`m9x528` contains one proven producer for this first vector: it takes the
current resource at task `+0x110` and inserts it into the vector subobject at
task `+0xf0` through generic helper `m9x1131` **[BIN]**. The complete ELF
relocation census has only three direct calls to `m9x1131`: `0x2bc22`,
`0x2dd6b`, and `0x3754e`. The middle call is this task-vector insertion; the
other two operate on `m9x738` and `m9x852` object-local vectors. None supplies
four target-indexed Linux task insertions **[BIN]**. Indirect vector producers
may still exist, so static analysis has not identified the current resource as
a particular MRT slot or joined every attachment to either submit vector
**[OPEN]**. The fact that `m9x954` does not walk `+0x208` is not by itself a
vendor bug: that vector may hold a different resource kind whose lifetime is
handled elsewhere.

On teardown, `m9x102` takes resource types zero, three, four, and five through
the unbounded `m9x880(resource,1)` loop; type one can enter it under two device
feature tests. `m9x880` reads backing `+0x60`, asks `m9x653` to refresh the
wrap-aware target, and passes it to `m9x641`. `m9x641` rejects MMIO read
`0xffffffff`, reads the memory completion word, clears its low nibble, extends
wrap at completion record `+0x20/+0x24`, and returns whether the target is
newer than completion **[BIN]**.

There is no timeout, scheduler yield, device-loss escape, reset, or error
return in the destructor loop. If an extra target carries a fence that was
never submitted, was assigned to the wrong backing, or cannot be reached by
the completion engine, close spins forever **[DER]**. `glFinish` returning
does not refute this: unbinding the FBO/program can create later cleanup work,
and the GL finish marker may use a different completion object from a backing
object's last producer fence **[OPEN]**.

## Safe contract for the new driver

For each submitted draw, the kernel and Mesa driver must enforce one coherent
job contract:

1. Validate a matching address, descriptor, format class, and shader color
   export for every enabled target.
2. Put every writable color backing in the job BO list with write access and
   hold a reference until retirement.
3. Create one output `dma_fence` for the job and attach it as the exclusive
   producer fence to every writable attachment.
4. Emit one completion sequence after all target writes. Signal the fence only
   after IRQ or bounded polling observes that sequence.
5. Make readback, sampling, rebinding, deletion, context destruction, and file
   close wait on that same producer fence. Apply any required domain/cache
   transition after the producer completes and before CPU or texture access.
6. On timeout, stop the guilty context and run bounded engine recovery. Keep
   BOs pinned until reset makes access safe; never replace recovery with an
   unbounded close loop or free a possibly active BO.

The new driver now exposes four targets (`caps->max_render_targets = 4`,
MEASURED 2026-09-19).  What follows is the gate that applied while the
routing was unknown, kept because the *kernel-side* clauses are still the
contract command validation has to hold to; expose RT0 only when any of them
is not met:

* report `PIPE_CAP_MAX_RENDER_TARGETS = 1`, `GL_MAX_DRAW_BUFFERS = 1`, and
  `GL_MAX_COLOR_ATTACHMENTS = 1`;
* accept only a draw-buffer list containing `NONE` or `COLOR_ATTACHMENT0`;
  return the appropriate API/framebuffer error for attachment or output index
  greater than zero instead of silently remapping it;
* reject fragment shader color outputs at locations 1..3;
* in command validation, reject writes/relocations to `b2..b7` and nonzero
  render-format fields `index89[11:3]` from untrusted userspace.

## Exact gates for enabling RT1..RT3

Gates 1, 3 and 4 are met (MEASURED 2026-09-19, `docs/parhelia-m9-mrt-routing.md`):
the native colour-export selector is measured and joined to the declaration
slot, an RT-only job was decoded from the driver's own stream dump with
`b2/b3`, the `COLOR` relocation, the index-89 format field and the RT1 colour
export in it, and every target was read back independently with sentinel
values that identify their target.  Gate 2 is moot for this driver, which
replaced the vendor ABI with an explicit per-job BO list.  What remains:

1. Recover or differentially capture the native color-export selector for
   locations 0..3 and join it to the MxMD declaration slot. The descriptor,
   relocation, and Windows job-membership paths for RT1..RT3 are already exact.
2. Capture the Linux vendor submission record to identify any indirect vector
   producers and prove that each enabled MRT backing enters the fence-updated
   resource vector, or replace that ABI with the new driver's explicit per-job
   BO list. The saved ELF has no further direct `m9x1131` join to mine.
3. On the new driver, capture and decode an RT1-only job containing `b2/b3`,
   relocation selector 3, index-89 format bits `5:3`, the RT1 color export,
   the RT1 BO write reference, and the common completion record.
4. With distinct sentinel colors, pass masks `0,1,2,3,4,8,f`; read RT0..RT3
   independently and test output-location-to-attachment remapping.
5. For every mask, unbind, delete all FBO attachments, destroy the context,
   and close the fd under a finite timeout. Verify the BO references and the
   common job fence retire before release.
6. Inject a lost interrupt and a nonadvancing completion value. Verify bounded
   polling, reset, error propagation, and subsequent context recovery without
   freeing an active BO.

The strict RT0-only gate is the deterministic current answer. The recovered
four-target packet structure is sufficient to design the next capture and the
resource contract, but it is not sufficient to advertise MRT safely.
