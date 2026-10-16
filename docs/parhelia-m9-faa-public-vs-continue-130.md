# M9128 public VS continuation after real 0x100

The original M9128 public VS DRAW was resumed only with the distinct real
0x100 hardware receipt from boot `46b862d5-5a0b-4342-aff7-63dc1567b55c`.
The receipt binds the exact 796-byte source input and original public VS
program, constants and geometry. The original caller advanced from its unread
0xf0 wait to another unread wait at 0x120; it did not return from DRAW or
invoke Lock. A separate, genuine vendor queue finalizer then emitted one END
and stopped before its status load at 0x130. Neither 0x120 nor 0x130 was
provided as a GPU response, and no pixels were supplied.

The CPU-produced continuation has owner 29 secondary bytes `[752, 1368)`:
600 bytes before the original 0x120 wait and 16 bytes from the independent
finalizer. Owner 27 primary bytes `[4992, 5040)` are a 48-byte END footer.
Its first 12 bytes contain a fixed indirect opcode and two endpoints into
owner 29, encoded with tags 1 and 2. The only prior command mutation changes
owner 29 word `+704` from `0x49d00000` to `0x49d00091`: the low field is
`(1284 - 704) / 4`, a same-owner relative target. This must be validated
and patched before posting the new END. The source writes three completion
tokens at owner 29 `+796`, `+1348` and `+1364`, for 0x110, 0x120 and 0x130
respectively, each encoded as the low point plus one. It writes the END
register once, ending at source primary offset 5040. The native cursor must
come from the retained live primary, including any earlier dynamic clear
delta; the source offset is only a profile reference.

The [typed plan](../captures/m9-faa-graph-public-vs-20261007/continued-prefix-plan.json)
and [stripped template](../captures/m9-faa-graph-public-vs-20261007/continued-prefix-template.bin)
separate 12 address fields, three timeline fields, two null fields and the
literal control word from source bytes. Native code must resolve each address
against its retained live owner, validate domain, capacity, alignment and
generation, and use the current timeline epoch. The owner 29 word at `+1320`
is the next deferred marker; source supplies no payload for it here. The
three `0x40004000` words at `+1292/+1296/+1300` are fixed instruction
immediates from the exact original M9128 DLL (`mov eax,0x40004000` at
`0xfa227`, stores at `0xfa22c/0xfa235/0xfa23e`). The packet writes e6,
44 and 44. Six other original M9128 context constructors also set paired
state cells from the same fixed immediate (PCs `0x1e4d7`, `0x1e6c6`,
`0x1ea0b`, `0x1ee39`, `0x1f26f`, `0x1f69f`). This proves literal production
in multiple same-device contexts, but does not alone settle whether the GPU
register values depend on a fixed reserved mapping. The original M9128
initial manifest actually binds a 64-page flags48 SYSTEM MDL at 0x40002000;
its third mapped PTE is 0x40004000. The failed native boot also places its
ordinary system resource at 0x40002000 with enough extent to cover that page.
That numerical overlap calls for a consumer/role check before treating the
immediate as universally portable. M9148 captures showing the same values
cannot qualify this M9128 path. The failed first native follow probe must
not be treated as a 0x130 receipt.

This is a bounded fixed-input CPU source plan for the retained public VS
graph. It does not qualify application state, the raster tail, Lock, 0x130
retirement or pixels. A native producer needs all six graph-owner write
fences before the old-word patch, new uploads and END post, then an actual
completion receipt before advancing the original caller. The source trace
is [archived](../captures/m9-faa-graph-public-vs-20261007/continued-actual100-prefix.json);
`check-continue-prefix.py` verifies the exact source image, rebuilt bytes,
four distinct live placements and refusal cases.

The follow controller accepts a *single new-boot* capsule for INIT, public
VS prefix 0x100 and follow 0x130. It requires the exact 796-byte input,
staged module/helper hashes supplied explicitly, the 664-byte follow
firmware, state 10 then state 11, both intermediate and final transport
snapshots, the native 5044-byte/four-post/relative-word line and final four
counter lanes ending in 0x131. It rejects the earlier 0x100-only capsule.
The host admission test uses a temporary parser-only synthetic fixture;
those bytes are never supplied to the emulator or treated as hardware proof.
After a genuine 0x130 receipt arrives, run:

```sh
PYTHONPATH=tools/faaemu:tools/m9emu /home/ai/tmp/faaemu/venv/bin/python \
  tools/m9emu/m9_faa_graph_public_vs_continue.py \
  --init-receipt-dir tests/m9-faa-graph-init-hardware-20261007/confirmed-retirement \
  --follow-receipt-dir /absolute/path/to/new-boot-proof \
  --follow-module-sha256 STAGED_MODULE_SHA256 \
  --follow-helper-sha256 STAGED_HELPER_SHA256 \
  --out /tmp/m9-public-vs-actual130-source.json
```

## Failed native follow and restore-pool boundary

Two real native follow probes retired the first public VS prefix at 0x100,
then timed out at 0x130 with writeback still 0x101. Neither is a follow
receipt. On the second boot, the requester reported command 0xa9a245 and
fetch 0xa9a32d. These are in the **live** restore owner at 0xa99000:
owner+0x1245 lies in the filled gap after record one, and owner+0x132d
points into record two at word 11 (aligned +0x132c). They are not locations
in the follow secondary DMA or shader program. This numerical join does not
identify the instruction that caused the timeout or prove those restore
bytes were wrong.

The original M9128 type-one/1024 restore initializer was already compared
against the entire native CPU image by
`tools/m9emu/check-faa-restore-inherited-image.py`. The failed boot also logged
verification of tag one in record DWORDs 9, 14 and 19. A separate **read-only**
8 KiB BAR snapshot after the second timeout, `restore-after.bin` in that boot's
proof directory, matches the source-qualified initial image byte for byte at
its live base. Its SHA-256 is
`fa87ce60b0ae62d22bdc77288970a89dce2e516142e197d8e414c029f8c4c337`.
The bounded snapshot covers the prefix, records zero through four, both
reported fetch neighborhoods and intervening work/fill bytes. It does not
cover the rest of the 0x181000-byte pool or establish what the GPU read at
the instant of failure. `check-failed-restore-snapshot.py` re-runs the exact
M9128 source check, derives the base from that boot's dmesg, reconstructs
the first 8 KiB, and rejects four host-only mutations. Do not substitute a
captured base for the next live owner.

The initial restore grammar is therefore not an evidenced cause of this
failure. The next hardware diagnostic should observe the requester's fetch
register immediately after valid 0x100 retirement, before posting follow,
and compare a read-only restore snapshot at that boundary with this post-fail
snapshot. Any mutation beyond the captured 8 KiB remains a separate question.
