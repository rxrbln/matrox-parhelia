# M9128 public VS first head and unfinished secondary block

The original public VS caller reaches `fa6c9 -> 25270` with requested point
`0xf0`. This is a **nonblocking dependency probe**, not a wait for the GPU to
retire `0xf0`: `252b9` reads common counter +00 once, and the forward path at
`2532e/25338` emits index `43` carrying the requested point if the observed
counter trails it. The function returns at `2534f`; it has no CPU retry loop.
`fa9e0` first calls queue vtable slot `+3c`, which resolves in this exact
M9128 run to `259e0` (`xor eax,eax; ret`). Slot `+68` is `25370` and issues
three observed tickets `c0`, `d0`, `f0`; the optional callback is absent.
Slot `+6c` is the `25270` probe. None of these calls drains the queue.

Before the unread `252b9` load, the **only** posted new primary span is
owner 27 `[4880,4944)`, posted twice at END 4944. It contains E3 token
`0xc1` at `+4888` and stops immediately before the indirect pair at `+4944`.
The original CPU has written owner 29 secondary `[0,736)`, but no posted
primary command reaches it yet. That secondary includes an E6 prologue at
`+692`, a deferred length word `0x49d00000` at `+704`, and E3 token `0xf1`
at `+732`. `248f2..248fe` later fills the length from the live same-owner
slot and target; it is a counted block length, **not a jump**. The current
native first-head opportunity is consequently the two source END posts and
an actual same-boot counter observation at that point. It must not attach
the secondary indirect pair or assert `0xf0` completion.

The source experiment's separate `23b80` queue finalizer, invoked after
interrupting the original caller, adds 16 secondary bytes at `[736,752)`
with E3 token `0x101`, then posts the new primary `[4944,4992)` indirect
range. This exposes the deferred block while its length field is still zero.
Real hardware retired `0x100` for that *diagnostic* submission, but the
subsequent attempt to patch `+704` to `0x49d00091` and extend the block
timed out at `0x130`. The failure does not prove the open block caused the
stall; it removes any basis for treating the diagnostic finalizer as
equivalent to an uninterrupted public DRAW.

The later, diagnostic continuation's first new `45d7eff0` packet writes
F0=`f0000400`, EF=`00080600`, D7=`000054ae`. These are actual `fc1ed`
raster-shadow outputs: F0 from shadow+28, EF from +20, D7 from +24.
The established output rule decodes EF bit 19 as target 1 (`b2`) enabled and
low role 6 as a route-fed gather pass without clip-code output. The first
public VS carry has EF=`00280200` (three targets, first head), while the
fixed-red first carry has EF=`00288800` (fused final/clip pass). Thus the
follow packet is a distinct gather stage, not a retry of the first carry.
Because the diagnostic finalizer advances the timeline and secondary cursor,
its `+752` offsets and `0x110/0x120/0x130` tokens are not yet portable to an
uninterrupted path.

The failed requester fetch at live restore owner+`0x132d` points into record
two at its index-43 header (`owner+0x132c`, low address tag one). The original
M9128 type-one initializer gives generic record two E6 presets
`624f7fff/62000000/2`, E3=`fffffff0`, and index43=`fffffff0`; record one
alone has the alternate `0c000000` E6 word. A read-only 8 KiB snapshot after
failure matches the complete source-qualified initial bytes in this covered
range. The pointer therefore identifies a restore-pool location, but neither
its cause nor a malformed record. It is not the gather secondary DMA fetch.

`tools/m9emu/m9_faa_public_vs_open_block_probe.py` re-executes the original
M9128 CPU producer from a qualified INIT receipt, verifies the queue slots,
static dependency instruction bytes, source writes and MMIO post extents, and
stops before supplying the unread common value. Its independent finalizer
observation is explicitly separate. Run it with:

```sh
/home/ai/tmp/faaemu/venv/bin/python \
  tools/m9emu/m9_faa_public_vs_open_block_probe.py \
  --receipt-dir tests/m9-faa-graph-init-hardware-20261007/confirmed-retirement \
  --out /tmp/m9-public-vs-open-block-proof.json
```

The next source continuation requires the actual common value after a fresh
HEAD-only two-END hardware session. A prior INIT `0xb1` observation cannot be
substituted for the later read. If the new observed value is below `0xf0`,
the original `25270` path emits a GPU-side index43 dependency and continues;
it does not wait for the CPU to observe `0xf0`. No future status is supplied
by this checkpoint.
