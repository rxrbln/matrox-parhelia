<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Retained M9128 graph INIT to first public draw: native seam

This review is bounded to PCI `102b:0540` revision 1, subsystem `22c0`,
the six-owner 320×240/format `0x66` graph, and the successful actual INIT
retirement at point `0xb0`. The [INIT hardware proof](../tests/m9-faa-graph-init-hardware-20261007/README.md)
qualifies that prefix only. It does not qualify a public draw, pixels, a
second graph initialization, or arbitrary Gallium state. Apply the
[capture replay policy](gpu-capture-replay-policy.md) to every later command
field and CPU mapping; source addresses and a private captured program are
not portable inputs.

## State that the current kernel retains

`m9x_faa_graph_init_ioctl` authenticates the file's graph ID and transport
generation, reserves all six GEM owners, runs the trusted first-context
producer and records state 2 only after its real wait. `m9x_faa_prepare_graph`
has transferred a graph binding to `transport.faa`; its graph kref and six
pins remain there after `FAA_GRAPH_RELEASE` or file close. The native replay
also retains the private owner set, including the opaque renderer pool owner
22 and separate context sentinel owner 21. These are prerequisites for the
subsequent public caller, not disposable INIT scratch. Failed publication
keeps published owners quarantined until bus mastering is stopped.

After INIT, `transport.faa_source_context` and
`transport.faa_virtual_primary` are true. The live flags30/B command owner
and its mapped pages remain owned by the native replay. Ordinary
`m9x_transport_arm_locked` returns `-EACCES` while that virtual primary is
active; `M9_SUBMIT` cannot start another command namespace. Even
`m9x_faa_graph_binding_acquire` is deliberately an *initial* binding
operation: it refuses when `transport.faa` or `faa_virtual_primary` already
exists. A post-INIT draw therefore needs a continuation on the existing
binding and command owner, not another PREPARE, INIT, or ordinary submit.
The opt-in Mesa INIT bridge is consequently experimental: after it succeeds,
an unrelated ordinary draw cannot fall through to `M9_SUBMIT`. Releasing the
file token and retargeting does not free the native first context, and a new
PREPARE is refused while that source context remains active. This is why an
INIT receipt cannot yet enable general application FAA rendering.

The [source INIT boundary](parhelia-m9-faa-graph-init-boundary.md) ends
before the public caller. The prior [prepared program mapping](parhelia-m9-faa-prepared-program-mapping.md)
shows the required kind-1 CPU alias and actual frontend `314`/`31c` ordering
in a different whole profile. Render's new same-context trace has reached a
genuine public `CREATEPS54`/`SETCONST57`/`SETPS56` and red `DRAW52`: it
observes `314=1`, live CPU-root `31c=f,0,c`, release `31c=0`, one
`17e480` preprocessing launch, and new primary, secondary, constants and
geometry writes. Its original `252b9` wait requests `0x100` but has not read
completion. These new trace details are provisional until Render seals its
typed/direct/timeline plan and independent finish; they are not a GPU
receipt or an encoding rule.

## Required continuation interface and admission

1. Authenticate an exact `(DRM file, graph_id, generation)` that still owns
   the initialized graph and its real `init_point`. Acquire its graph kref
   before dropping the file lock. Under the transport lock, compare the
   *same graph object* with `transport.faa->graph_binding`, verify the live
   generation, source-context and virtual-primary flags, completed INIT
   point, and absence of wedge, reset, outstanding queue or async work.
   `m9x_faa_graph_binding_acquire` cannot perform this continued lookup;
   a separate retained-binding comparison is needed. A file RELEASE before
   admission must refuse; an already admitted draw retains its refs through
   retirement or quarantine.
2. Reserve the six graph BOs as one WW set, plus any new program, constants,
   geometry, texture or depth BOs that this draw actually consumes. Keep
   lock ordering file/owner reservation before transport. Build and map new
   private owners outside `transport.lock`; do not reserve a TTM BO while
   holding it. Attach a write fence to every written graph owner before
   publication, and retain all readable owners until the actual draw wait.
   On a post failure, keep every potentially DMA-visible owner until safe
   bus-master teardown, as INIT does.
3. Compile the actual public state and create its program, constants and
   geometry from live inputs. The INIT blob contains no draw program. The
   source's CPU-root alias must be derived from the live program leaves and
   validated mapping, with only source-known bytes uploaded in the observed
   `314`/`31c` order. This internal program mapping is not permission for a
   direct diagnostic F022 aperture read or replay of captured CPU PTEs.
4. Append source-qualified draw packets to the retained primary at its
   **actual** `r->primary_bytes` endpoint. The live companion page-clear
   layout can move that endpoint, so a captured `4880` or `+76` alone is not
   an encoder. Regenerate all address, page, extent, packet-length and
   timeline fields from retained owners. Check the expanded primary and
   secondary/root mappings and PTEs before extending `COMMAND_END`; never
   rewrite `COMMAND_BASE` or reset the queue. Existing
   `m9x_extend_faa_virtual_primary_locked` gives the bounded END/wait
   mechanism, but `m9x_validate_faa_virtual_resume_locked` currently admits
   only diagnostics and has no graph identity check or draw fence contract.
5. Return a draw point only after its own actual final wait. The source's
   pending `0x100` deadline is not a completion reply. Do not infer pixels,
   resolve readiness or public Lock admission from the INIT point or a CPU
   vendor return. Any subsequent output/Lock needs its separate source
   sequence and receipt.

The smallest implementation order is an internal continued-binding lookup
and pure admission tests; then a source-closed program/owner encoder with
two live placement/alignment controls; then a dedicated draw ioctl that
reserves owners, extends the existing primary and waits for a real point.
The first executable producer remains gated on Render's complete post-INIT
typed/direct/timeline inventory and an independent finish target, plus the
live shader/state lowering used by Mesa. Adding a graph draw ioctl or
relaxing the diagnostic resume gate now would accept a command stream that
the available evidence cannot construct.
