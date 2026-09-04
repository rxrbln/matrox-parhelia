# Parhelia bring-up, measured against the from-scratch RE procedure

Assessment of this project against `../intel-poulsbo/FROM-SCRATCH-RE.md`, a
vendor-neutral procedure for taking an undocumented GPU from nothing to an
accelerated desktop. Written to find what we are skipping, not to restate it.

## Where we actually are

| Phase | Exit criterion | State |
|---|---|---|
| 1 Inventory | name every component, licence, attach points | **done** — boundary list in parhelia-kernel-abi.md; cut line chosen implicitly *above* the kernel (we use the vendor module) |
| 2 Submission interface | **replay a captured frame and get the same picture** | **NOT met** — we capture, we have never replayed |
| 3 First triangle | triangle from a freestanding program, verified by reading pixels back | **NOT met** — tools/parhelia-3dtri.c consumes the stream and renders nothing |
| 4 Shader ISA | a hand-assembled shader runs and its output is read back | partial — two disassemblers, no assembler, no execution harness |
| 5 State model | change one state bit, predict the pixel change | strong — differential sweeps are our main instrument |
| 6-8 stack, GL ladder, 2D | — | not started |

## The strategic error this exposes

**We have been attempting phase 3 without finishing phase 2.** The procedure is
explicit that the order is what makes later phases falsifiable: "a triangle
cannot be trusted until a frame can be submitted without the vendor stack
underneath", and "notes that look convincing but have never been replayed are
hypotheses".

That is exactly the shape of our longest-running failure. The from-scratch WARP
draw arms the engine, the engine consumes the vertex stream, and no pixels
appear anywhere in 256 MB — and we have been debugging *state* to explain it
while never having demonstrated that we can reproduce a known-good frame at all.
A replay would tell us in one experiment whether the gap is state, submission,
or microcode.

**Next action, ahead of any further decoding: build the replay harness.** We now
have everything it needs — the ioctl/mmap trace (tools/parhelia-mtxtrace.c), the
buffer contents at submit time (tools/parhelia-glsweep.c), and the mmap-cookie
protocol.

## What the procedure confirms we are doing right

- **Differential sweeps, one property at a time** (§6). This is how width/height
  were pinned in the texture descriptor and how the cursor row pitch fell out.
- **"Rule the memory, not the colour"** (§6). The cursor capture encodes the row
  index into every row, so the stride is read off the dump rather than inferred.
  Same idea as the per-format texture runs.
- **Leave corrections beside the original claim** (§12). Applied to the region-2
  transport model, the `-lm` false diagnosis and the `0x384` soft-trap reading.
- **Capture differences, not captures** (§3). The whole glsweep design.

## Rules from §9 we had already violated, and what they cost

- **"A probe reporting zero needs a positive control."** We reported zero writes
  to the `0x91c` microcode FIFO from an mmiotrace. Control applied afterwards
  (below) — it changes the strength of that claim substantially.
- **"Verify the machine before believing the run."** An extended sweep produced
  a valid-looking file containing zero operations because direct rendering was
  silently off. The procedure's answer is a one-second liveness test run *first*;
  we now have two checks but not yet a harness that refuses to start.
- **"A test that passes with and without your change proves nothing."** The
  `MTX_MAX_ORDER` diagnosis fitted the evidence perfectly and was wrong; what
  disproved it was a control (ioquake3 reaching `finished R_Init`).

## Positive control for the 0x91c claim — and what it establishes

Ran the userspace tracer with its protect/single-step guard armed
(`captures/mtxtrace-userspace-writes.txt`):

- The instrument **works**: 1884 userspace write faults captured across 71
  distinct offsets during one texture upload and one textured draw.
- The GL client maps `/dev/matrox/mtx0` **seven times — six 4 KB pages and the
  large buffers. None is the 8 KB BAR1 register aperture.**

So a **GL client never maps registers at all**: every GPU access it makes is a
store into a shared buffer followed by the `nr=33` doorbell ioctl. For the GL
client path, therefore, the kernel-side mmiotrace *is* complete for registers,
and "no writes to 0x91c" holds for that path rather than being an artefact of
the instrument's blind spot.

This does **not** extend to the X server, which does map the register aperture
(`GET_MEMORY_INFO` hands out `reg_base`/`reg_size`) and drives 2D directly. The
microcode question (docs/parhelia-vertex-isa.md, the mtx676 conflict) must
therefore be settled against the **X server**, not a GL client. That is the next
capture: LD_PRELOAD the tracer into the X server itself.

## Ordered plan from here

1. **Replay harness** — phase 2's exit criterion, and the unblock for the
   triangle.
2. **Trace the X server** with the same tracer, to close the microcode question
   and to capture the 2D register path directly.
3. **WARP assembler + "run this program" harness** — phase 4 order is
   disassembler, assembler, harness, compiler last.
4. **Exploit the oracles** (§2): the microcode linker `mtx3558` and the vendor
   shader compilers can validate our encoders by round-trip, converting bit
   guesses into tests.
5. A liveness/refusal harness per §9.3, so a dead or indirect-rendering box
   cannot silently produce empty captures again.
