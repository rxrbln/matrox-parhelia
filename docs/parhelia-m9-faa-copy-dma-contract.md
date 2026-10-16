# Normal Lock conversion buffers: direct DMA address and CPU span

The fresh preparation helper produces two contiguous OS allocations for PDEV
`d4/d8`. Their selector-eight descriptor addresses are **physical payload
addresses tagged with class two**, not flags30/B GPU virtual addresses.
Normal Lock's conversion packet adds a source-managed ring offset to that
descriptor address. Its CPU readback uses the same allocation's CPU payload
alias plus the same offset, after waiting for the buffer's recorded GPU point.
No new GPU PTE publication follows from this contract.

This is a source/CPU-helper result. It does not execute complete `b1290`, supply
a GPU receipt, read rendered bytes, or qualify FAA pixels. Fresh constructor
and preparation remain a whole-fixture prerequisite; no old receipt is reused.

## Actual owners and descriptor producer

The retained proof executes the real constructor and preparation helpers,
then original `467e0(metadata,8)` on both resulting live wrappers. No object
fields or queue bytes are substituted. Their OS allocation sizes are `201000`
hex bytes, usable sizes and backing ring capacities `200000` bytes. Both
wrapper `+1e8` resource kinds are **two**; this is distinct from the constructor
backing-mode value one and OS allocation request flag one.

`66b45..66b71` records allocation `+10` raw CPU allocation base, `+c` allocated
size, `+14` requested usable size, `+1c` aligned CPU payload alias and `+20`
physical payload address. CPU and physical payloads receive the same alignment
delta. A native replacement must retain that delta and the usable extent; the
extra allocation page is not permission to read past the two-MiB payload.
`4dff4/4dffb` and `4e7e2/4e7ec` publish the allocation's physical payload into
backing and wrapper metadata `+15c` (object `+198`).

`47366/4736e/47379` reads metadata format `+140`, address class `+144`, and
valid address `+15c` separately. `46822..4682e` produces
`(payload_address & ~3) | (address_class & 3)`. The tag is not inferred from
the format or OS allocation flags. The observed comparison at `47538` is
actual class two against expected translation class one; `4753e` skips the
translation branch. Observers at `47b30` and `59aa0` are never reached.
Instruction hooks invalidate the Unicorn translation cache before execution.

The helper's synthetic physical owners currently produce descriptor words
`01001002/f0002000/00010040` and `01202002/f0002000/00010040`. These are
observations of live owned allocations, not portable addresses or runtime
packet templates. Initial contents are unspecified and are never pixels.

## Typed GPU consumer, ring ownership and extent

Normal Lock assigns selector eight to both wrappers at `b146c/b149f`.
`b1754/b175f` selects d4 or d8 using the current iteration's low bit.
`b1804..b1810` obtains the selected wrapper, its `+248` backing, and the
selector's three-word descriptor. `b1813` reads its address word; `b1817` adds
backing `+425c`; **`b1826` writes that result directly to the F2 packet field**.
This direct producer does not call `25680` or the translated-address helper.

The retained CPU experiment executes only `b1813/b1817`, stopping at `b181d`
before queue access. It records the two exact descriptor/ring reads. This
closes the address calculation, not a full caller or queue submission. A
future native fixup needs the typed F2 field and live ring addend, not a
captured address or assumed zero offset.

The row width is rectangle `x1-x0` (`b1414/b1417`). Bytes per pixel comes from
the source format's bit count divided by eight (`b1411/b1420`). `b1449..b1466`
rounds width to the wrapper's pixel alignment; the retained wrappers have
alignment eight. Row pitch is padded width times bytes per pixel (`b149c`).
`b14e9..b151d` limits rows to `min(32, ring_capacity / pitch)`.
`b1788` reserves rows times padded pitch. Ring start alignment is the wrapper's
`+1b4` (128 bytes here), applied at `b176d..b1796`.

Real `8af50` advances the ring entry, publishes `+425c` from `+4260`, tests the
new end against capacity `+4258`, and resets `+425c=0` upon a wrap. Native raw
readback must validate `offset+rows*pitch <= usable_bytes`, alias alignment
delta and allocated extent, and preserve the selected owner/ring entry until
the corresponding wait. Capturing all `201000` allocation bytes would include
padding without establishing a pixel interpretation.

For the current 320-wide, 32-bit triangle read request, the source formulas
give 1280-byte pitch, at most 32 rows per chunk and 40960 bytes per full chunk.
Those are derived examples, not extra arguments supplied to the helper. A
complete Lock capture must retain its actual rectangle, source format, both
descriptor selectors, row count, pitch, ring offset and iteration.

## CPU consumer and channel copy

`b1852` obtains the real queue point; `b1863/b1866` places its low/high halves
in the current ring entry `+8/+c`. After posting the next chunk,
`b18d7..b18e0` chooses **the previous iteration's buffer** and calls `86bc0`
at `b18f0`. The initial iteration has no previous read; the final empty
iteration still reads the last written buffer.

The actual d4/d8 resource kind is two. The `2cb40` table at `2cda4` selects
`2ccb4`: allocation comes from wrapper `+1e4`, and CPU base comes from
allocation `+1c` at `2ccba`. The helper executes these two read instructions
and stops before the caller-stack output write at `2ccbd`. The alias equals
wrapper `+1f0` in this fixture, but that equality does not change the actual
kind-two dispatch. An earlier informal kind-one attribution was corrected.

`86cde/86ce4` loads the ring deadline; `86d00..86d22` reads and waits for the
real GPU-written common counter, including its wrap generation. Only then
`86d55..86d62` returns **CPU payload alias plus backing `+425c`**. The second
conditional `314` kick at `86d4b` is skipped for actual kind two. The earlier
`2cb40` feature-dependent access kick is a separate existing contract; this
proof does not replay or disable it.

The copy uses actual row bytes, not padded pitch. `b1571..b1597` splits row
bytes into DWORDs and a possible byte tail, falling back to byte copying for
an unaligned caller address. `b19a7..b19d8` copies a row and advances by the
source/destination pitch remainder. The contiguous control at `b197a` copies
row DWORD count times row count. For 32-bit format and caller flag `20` hex,
`b1930..b194d` explicitly swaps low and third bytes while preserving the other
two. This is a source-controlled channel conversion, not an opaque-record
decoder. Raw DMA capture must preserve bytes before this copy and let the
actual source routine perform the conversion after a qualified receipt.

## Reproduction and negative controls

```sh
PY=/home/ai/tmp/faaemu/venv/bin/python
$PY tools/m9-vendor-faa-copy-dma-contract.py --out /tmp/copy-dma.json
$PY tools/check-m9-faa-copy-dma-contract.py \
  --proof docs/traces/m9-faa-copy-dma-contract-20261007.json
```

The checker verifies the image/input/implementation hashes and existing
constructor/preparation proof, 29 instruction spans and the kind-two dispatch
table, actual address-class comparison, owner/payload association, two real
descriptor and read-only address/CPU-alias prefixes, and no MMIO or GPU reply.
Nine contradictory-evidence controls and six span/layout refusals cover wrong
domain/kind/alias/tag, completion claims, padding, alignment, row limits and
ring-end overrun. This does not establish later normal Lock completion,
rendering, or compatibility of an old FAA hardware prefix with new startup.

<!-- SPDX-License-Identifier: GPL-2.0-only -->
