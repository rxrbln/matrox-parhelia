# P690 mode-7: actual vendor-path audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

There is **no saved successful P690 execution of `0x9f000001`**.  The Linux
HAL, Windows display driver, and Linux ICD prove that mode 7 is a real
three-dword, direct-index packet and show exactly when their *generic*
direct-fetch path selects it.  That is ABI evidence, not evidence that the
P690 executed it in the recorded workload.

The only retained P690 final draw section is in
`captures/ioq3-p690-working-draw.txt` (SHA-256
`4ade3269b3b6d482dd38c32778ef26dd5759cbaf9e34a05faef85168b2864768`).  It
follows a logged `glBegin(GL_QUADS)` / `glEnd`, and contains:

```
8f000001, 00000000, 00000004,
00000002, 21212425, 00000002, 1a9bc000
```

Thus that P690 draw used the vendor's slot-relative range packet plus its
normal fence-register list.  It contains no `0x9b`, `0x9f`, `0xdb`, or `0xdf`
header.  It is a positive P690 rendering record for mode 3, but it is not an
indexed `glDrawElements` capture and cannot establish a global absence of
mode 7.

The two private P690 mode-7 diagnostics have independently hung after their
last primary CALL with both class-0 local VRAM and sealed coherent class-3
DMA32 backing.  Combined with the absence of a matching P690 vendor command
capture, that is a reason to keep `PARHELIA_PARAM_NATIVE_INDEXED = 0`, not a
reason to invent another header flag, WARP entry, cache packet, or fence form.

## What the saved binaries prove

The direct-fetch dispatch is genuine and shared by the vendor stacks:

* Linux `mtx2218` selects `mtx1067` when the index object is
  GPU-addressable.  Its direct-fetch primitive table is `mtx2855`, thirteen
  entries all pointing at `mtx1443`; the object relocation table has those
  `mtx1443` references at `.data+0x1a0..0x200`.  `mtx1443` calls `mtx2661`
  and emits `[header, start, end]`; `mtx2661` selects mode bits
  `0x1c000000` for a four-byte element.  With ordinary flags the result is
  `0x9f000001`.
* Linux `mtx3712` chooses that table only when every enabled vertex stream is
  GPU-addressable (`ctx+0x1cc != 0`).  `mtx1035` then fills the eight stream
  bases, their packed strides, and fetch floor/limit; `mtx3355` emits the
  complete direct-fetch state before the selected draw builder.
* The Linux ICD independently writes `0x1c000000` into its DrawElements
  record only after the element array's `+0x24` GPU-addressable byte passes.
  It also requires every enabled vertex array to pass the same test.  Client
  arrays therefore use the copy/de-index route instead.
* Windows `MTXPARD.dll` has the same 32-bit address-index mode selection at
  `0x12e1c7..0x12e1fa` and the same three-dword writers at
  `0x132920..0x1329ff`.  Its code establishes an interoperable packet
  contract, but the saved PE alone cannot show which D3D workload reached it
  on a P690.

These facts prove that `0x9f` is a supported *software interface to this
family's engine*.  They do not prove a P690 direct-index execution.  The 2009
ICD contains no PCI-device-id branch in its array dispatch, while the saved
runtime captures are split by board: the large matrix is a PCI-X 0x0528, and
the P690 extract retains only one final mode-3 draw.

## Capture inventory and negative results

The current checked-in capture set has three materially different kinds of
evidence:

| Material | Board / scope | Mode-7 result |
|---|---|---|
| `captures/gl-matrix/*-secs.txt` | historical PCI-X 0x0528, 44 decoded secondary-section files | 2,438,905 dwords scanned: 28,673 `97080001`, 8,177 `970c0001`, 1,536 `970e0001`; zero `9b000001`, `9f000001`, `db000001`, or `df000001` |
| `captures/live-ring-0528/` | historical 0x0528 ring and secondary archives | not P690 runtime evidence; its observed GL draws are address-mode `0x97` |
| `captures/ioq3-p690-working-draw.txt` | retained P690 vendor extract | one retained final draw, `8f000001`; no mode-6/7 header |

The historical corpus's lack of `0x9f` is expected: its GL array inputs are
ordinary user memory, so the ICD's GPU-addressable test selects `mtx1017`,
which copies and de-indexes to a mode-5 `0x97` packet.  It cannot prove that a
P690 rejects `0x9f`.  Conversely, static occurrences of the mode bits cannot
prove that the P690 accepts the new DRM diagnostic.  The retained P690 extract
is the only direct P690 command observation, and it is too small to cover the
necessary direct-index workload.

## WARP, context state, primary chain, and fence

There is no mode-7-local WARP binder in either saved emitter.  `mtx1443` only
appends the three dwords to the already-selected secondary command accumulator;
it neither writes `0x1100`/`0x1104`, changes `0x10f0`, nor loads a WARP
program.  The normal `mtx3712` state path is responsible for stream state,
primitive, fetch floor/limit, descriptors, and whatever normal WARP state the
outer rendering context selected.

Therefore the binaries establish this ordering, but not a special P690 WARP
entry:

```
normal context WARP/state
  -> complete stream bases and strides
  -> primitive, fetch floor/limit, descriptor reset/list
  -> 0x9f header/start/end in the normal secondary accumulator
  -> ordinary secondary CALL, producer update, and completion protocol
```

`mtx3249` is the immediate-submit sibling of `mtx1443`; it uses the same
ordinary accumulation/submission machinery.  The saved P690 mode-3 draw has
the usual terminal register list `0x21212425` carrying the fence sequence and
fence address.  No P690 mode-7 capture exists to compare a primary CALL,
producer value, or fence against it.  No evidence supports a mode-7-only IRQ,
writeback, or doorbell change.

The open screen test binds its known working passthrough WARP state through
`p3d_emit_state()` before the full vendor-shaped direct-fetch snapshot.  Its
same state construction has retired mode-5 screen draws.  This makes a guessed
replacement WARP program a weak next hypothesis, but does not turn the current
mode-7 state into vendor-equivalent P690 evidence.

## Minimal next experiment

Obtain a *vendor* P690 direct-index trace on a separate disposable vendor boot;
do not load a vendor driver on the open-driver target at `192.168.1.111`.
The required trigger is not normal ioquake3: Q3 supplies client arrays, so the
ICD normally takes the documented de-index/copy route.

Use `GL_ATI_vertex_array_object`, which the saved ICD implements by
`glNewObjectBufferATI` registering a driver-pinned system-memory object through
escape `0x14`.  Bind every enabled vertex array with `glArrayObjectATI` from
such an object, and place the 32-bit element array in a driver-recognized
GPU-addressable object as well.  A one-stream, one-triangle
`glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_INT, ...)` is sufficient.

On that already-running vendor stack, use the existing
`tools/parhelia-mtxtrace.c` interposer with `MTXTRACE_BUFS=1 MTXTRACE_CPTR=1`.
Require all of the following before drawing a conclusion:

1. The client maps `/dev/matrox` directly and the trace has non-empty IOCTL,
   MMAP, and SUBMIT records.
2. The submitted client record has mode bits `0x1c000000`, and the kernel-side
   output contains `9f000001` (or an evidence-backed layout-flag variant).
3. The preceding secondary state contains the stream base/stride snapshot,
   bounds, descriptors, and normal WARP binds; retain the associated primary
   CALL and fence/update records.
4. The fence retires and the triangle image matches a known mode-5 reference.

A successful capture resolves the missing state/WARP/primary comparison.  If
that vendor object-buffer workload still emits mode 5, the correct conclusion
is that the generic address-index builder is unreachable through this ICD/P690
combination, and the Q3 optimization should remain an open hardware feature
rather than a Mesa fast path.  In either outcome, do not perform another P690
mode-7 hardware trial until the capture explains the distinction.
