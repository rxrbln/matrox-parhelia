# In-place sampled-copy continuation after real 1a0 retirement

The exact `preclear-inplace-copy-wait` source prefix was physically qualified
on M9128 boot `52df8cdf-af4a-4513-b38e-3a010e8c0cbc`, kernel `d249a3e`.
Its independent finish retires `1a0`, with actual channel zero `1a1/0` and
channels `20/40/60` each `10/0`. The receipt and source-plan hashes are retained
in `tests/m9-faa-inplace-copy-hardware-20261007/`. This is a different source
profile from the new public shared-depth flush even though that separate
profile's independent finish also happens to use `1a0`.

The isolated [controller](../tools/m9emu/m9_faa_inplace_copy_receipt_continuation.py)
reconstructs the original `ac1f0` stack stopped inside sampled CPU preparation
at `252b9`, requested deadline `190`. It checks that the complete 5936-byte
source prefix, owner manifest and independent finish equal their signed
captures before restoring that stack. Only the physically observed `1a1`
is supplied for the actual channel-zero four-byte read. No other counter,
pixel, GPU-owned BAR byte, mapped MDL unknown byte, or opaque record is supplied.
The existing owner-and-offset read guard remains installed throughout.

The sampled resource has `+3d0=1`: the original `b1290` copy method had selected
the retained staging object through `57d90` before the `190` wait. After the
real receipt it returns from that preparation, appends 3412 source-known bytes
to the original primary MDL after its earlier 5936 bytes, and makes eight actual
typed relocation callbacks. Actual endpoint MMIO `404` writes occur at `2461e`
and `4a327`. These are new CPU-issued GPU actions; the preceding `1a0` receipt
does not qualify their completion.

The next boundary is CPU object construction, before instruction `b135a`:
`b1315` loads `[resource+238]+d4` into ESI; `b131e` loads the corresponding `d8`
field; `b135a` would compare `[ESI+234]`. The fixture's original device has
both fields zero, and ESI is zero. The controller stops before this dereference.
It does not manufacture a conversion object or treat the null fixture fields
as a hardware register defect. Their genuine constructor/publisher must be
joined before this original method can proceed.

The actual pending request still uses the original sampled primary. Its caller
destination is `[0,0,64,64]`, pitch 256, and source rectangle `[0,32,64,96]`.
Its selector-nine tuple remains `[live GPU|1, cc220140, 100f0]`, but the selected
staging conversion means this tuple alone does not prove the eventual CPU
copy's source view or pixel layout. No `55550`, `2cb40`, or `8b390` entry executes
after this receipt before the new boundary, and the original CPU call remains
unreturned. A raw sampled F022 read is therefore not substituted as its ROI
pixel result.

The [strict checker](../tools/m9emu/check-faa-inplace-copy-receipt-continuation.py)
verifies the receipt, guard, copied source, source consumer bytes and all 853
new MDL write events. It rejects a wrong-profile counter, invented pixels,
a forced conversion pointer, and a false returned copy. The
[reproduction recipe](../tests/m9-faa-inplace-copy-continuation-20261007/README.md)
uses the established Unicorn/pefile interpreter. This source continuation
qualifies no rendered FAA image and proposes no native GPU state change.
