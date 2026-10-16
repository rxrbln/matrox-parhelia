# Genuine public shared-primary dirty FAA flush

The source fixture can enter the original dirty FAA flush through real public
draw commands. After the returned first triangle, it allocates a second depth
resource using the actual embedded companion's profile and the same 320x240
shape, through `a3870`. Actual `7a770` publishes a handle into the retained
caller's resource table. This resource constructor/handle publisher is an
explicit bounded fixture below the still-unjoined public PDEV CreateSurface
admission; no claim of a complete public surface-creation callback is made.

The caller then supplies DP2 RENDERSTATE8 requests `(7,1),(14,1),(23,4)`,
SETRT41 retaining the same primary with the new depth handle, and DRAW52.
The original retail `beeb0` executes; only the host fixture's one-call guard
is bypassed. No renderer, dirty flag, association list or vendor instruction is
patched. An owner read guard is installed before the new resource constructor
and remains active through allocation, publication and the public request.

The disabled-depth control demonstrates why the earlier same-primary/new-depth
experiment did not flush. SETRT executes and publishes the new caller depth,
but `137c70` observes the disabled-depth mode and `137c84..137c95` selects the
primary's embedded companion. The effective pair passed at `139636` remains
unchanged. There is no `101290/a7390/a7440/aa750` call before the new `1b0` wait.
This is source caller-state behavior, not a missing-association inference.

With the public depth state enabled, `137d6f` publishes the new effective depth
interface. Since the primary interface is shared, the real association conflict
enters `139636 -> 101050 -> 101290 -> interface78 -> a7390 -> a7440 -> aa750`.
The dirty original surface supplies graph `[primary,companion,main,sentinel]`
and rectangle `[0,0,320,240]`. Its old association remains retained while this
flush is pending; cleanup and dirty clearing are not claimed to have returned.

The whole `public-shared-depth-flush-wait` capture stops at the actual `252b9`
deadline `190` before the new GPU read. Only the earlier independently qualified
120 receipt was used to return the original triangle. There are zero GPU-owned
BAR reads and zero new counter reads during the additional constructor/public
request. No opaque GPU records or pixels are imported. The independent original
`23b80` finish retains the complete 8792-byte primary and requests `1a0`; it
does not resume the pending public caller or invent that completion.

The exporter captures all 87 typed callbacks, 30 timeline producers and six
direct secondary endpoints, actual MDL/PFN bindings and exact source-known
LOCAL images. It proves every earlier consumed LOCAL and SYSTEM span unchanged;
the only earlier primary change is an append. The two actual page-clear
segments remain the original companion and sampled-primary clears. The new
phase is a public dirty-flush boundary, not an internal `ac1f0` CPU-copy request:
`public_flush_invoked=true`, `output_request_invoked=false`, and no ordinary
output owner exists. The source-specific native generation and placement proof
are reviewed separately, without inheriting the other profile's same-numbered
in-place `1a0` hardware receipt.

[Caller check](../tools/m9emu/check-faa-shared-depth-caller.py) validates the
depth-state A/B, original handler sequence, effective pair, resource publisher
scope, graph and protected input boundary. Source producers are
[controller](../tools/m9emu/m9_faa_shared_depth_flush.py) and
[whole exporter](../tools/m9emu/m9_faa_shared_depth_capture.py).
This is source evidence for a distinct candidate lifecycle; it proves no FAA
rendered pixels and does not enable application FAA.
