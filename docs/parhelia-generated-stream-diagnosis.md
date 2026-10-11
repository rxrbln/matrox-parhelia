# Why our generated 3D stream stalled, and what it needed

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Differential analysis of the vendor's captured per-draw command stream (which
replays and renders) against the stream `tools/gen-tri-stream.py` builds (which
stalled at packet 8). Pure file comparison — no hardware was touched.

Tags: **[CAP]** read out of the capture, **[BIN]** read from a vendor binary,
**[INF]** inferred.

## 1. Method

Both streams are `(ring, sec, reloc)` triples submitted one secondary call per
packet. A walker decodes each in execution order into a per-packet signature —
`REG:<register list>` for opcode 0, `BLK:<first>/<count>` for a constant block
load, `BLK*:<header>` for the two sub-header-less `0x41` forms, `DRAW` — and a
`difflib.SequenceMatcher` aligns the two signature sequences. The walker is
reproduced in §8 so the comparison can be re-run.

    vendor:  captures/live-ring-0528/{ring7,sec7}.hex + reloc7.txt   (122 calls, 1041 dw)
    ours:    captures/live-ring-0528/gen-{ring,sec}.hex + gen-reloc.txt

The `0x20` tag index is **not** a no-op pad: decoding the vendor's 122 packets
with "pad consumes no data dword" leaves two packets with a dangling tail,
while "pad consumes one dword" closes all 122 exactly. `0x20` is register index
0x20 = MMIO `0x1080`, a scratch register that is really written. [CAP]

## 2. The vendor's per-draw spine

Everything the working replay does between the previous draw and the `0x97`
packet, with the clear passes, lighting blocks and 2D blits elided:

| # | packet | meaning |
|---|---|---|
| 53 | `1120,1124,1128,112c` | WARP reset block |
| 54 | `1110,1114,1118` | |
| 55 | `1374=0; 10f0=0x20000000 ×2; 1100=2; 10f8=2` | null bind, MODE 0 |
| 56 | `1374=0; 10f0 ×2; 10c8..10d8, 10c0` | stage entries, nulled |
| 57 | `1374=0; 10f0 ×2; 10f0=0x80000000; 1130=0x00002204; 111c; 1144` | |
| **58** | `10b0 = 0x00000060` | **vertex-fetch reset** |
| **59** | `1000..101c = 0`, `1040..104c = 0` | |
| **60** | `1064 = 0xffffffff`, `1060 = 0` | |
| **61** | `10b4 = 0` | |
| 62–89 | RT slots, scissor, depth, three 2D blits, fences | frame setup, elided |
| **90** | `1000 = 0`, `1040 = 4`, `10b0 = 0x0002305c` | **block-load preamble** |
| **91** | `10b4 = 0`, `10b8 = 0x83c00000` | **one-attribute descriptor** |
| 92 | `1374=0; 10f0=0x20000028 ×2; 1100=0x07957183` | bind loader A |
| 93 | `0x41020007` bind table | 8 body dwords, WARP entries |
| 94 | `1374=0; 10f0=0x20000028 ×2; 1100=0x07957383` | bind loader B |
| 95–102 | eight `0x41` block loads: `first` = 0/8, 0x134/5, 0x149/5, 0x155/5, 0x160/2, 0x172/4, 0x178/1, 0x17d/2 | **one bind covers all eight** |
| 103 | `1130=0x0700300c; 1374=0; 10f0 ×2; 1100=0x07957583` | bind loader C |
| 104 | `0x4106000f` base registers `b0..b3 = e0/160/80/800`, `a0..a3 = 0..3` | |
| 105 | `1374=0; 10f0=0x20000028 ×2; 1130=0x0200300c` | closes the MODE-0x28 run, no `1100` |
| 106–110 | `1120…`, `1110…`, then three `10f0=0x20000002` binds: `1100` = vertex program, `10c8..10d8` = stage entries, `10f0=2` + `1130`/`111c`/`1144` | the real programs |
| **111** | `10b0 = 0x0006305c` | **real primitive word, bit 18 set** |
| **112** | `1000 = 2`, `1004..101c = 0`, `1040 = 4`, `1044..104c = 0` | |
| **113** | `1064 = 0`, `1060 = 0x000280cc` | |
| **114** | `10b4 = 0` | |
| **115** | `10b8 = 20c00000, a1c50020, 0, 0` | real `[x,y,r,g,b]` descriptors |
| 116–117 | `1220,1224,1230,1234`; `1228,122c` | render target |
| 118 | `1324 = 0xffffffff` | |
| 119 | `1300=0x707; 12bc ×2; 12b0 = 0x3fff8842` | fence, then surface select |
| 120 | `1300 = 0x00a60ec4` | DWGCTL, the draw opcode |
| 121 | `12f4 = 0x42000000` | |
| 122 | `121c = 0`, `1374 = 0` | |
| 123 | `1200 = 0x0001000f`, `1204..120c = 0` | |
| 124–125 | combiner slots | |
| 126 | ring fence write-back | |
| **127** | `1000 = 3`, `1040 = 4` | **class word, re-emitted last** |
| 128 | `0x97080001 start end` | draw |

## 3. What our stream did instead

Aligned against the table above, the old generator produced 33 packets of which
**27 matched** the vendor's — and the mismatch was a hole exactly where the
stall was reported:

    =   53    2  REG:1120,1124,1128,112c
    =   54    3  REG:1110,1114,1118
    =   55    4  REG:1374,10f0,10f0,1100,10f8
    =   56    5  REG:1374,10f0,10f0,10c8,10cc,10d0,10d4,10d8,10c0
    =   57    6  REG:1374,10f0,10f0,10f0,1130,111c,1144
    V   58 … 91  (34 vendor packets with no counterpart)
    =   92    7  REG:1374,10f0,10f0,1100          <- loader-A bind
    =   93    8  BLK*:41020007                    <- OUR PACKET 8, the stall
    …
    =  104   19  BLK*:4106000f                    <- base registers
    G        20  BLK:000/4                        <- our MVP, on the wrong side
    G        21  BLK:160/2                        <- our viewport, on the wrong side
    =  105   22  REG:1374,10f0,10f0,1130

Three defects, in order of severity.

### 3.1 Root cause: the `0x41` block-load preamble was missing entirely

Vendor packets 58–61 and 90–91 had no counterpart in our stream. Those six
packets are the vendor's block-load preamble, and they are not decoration:

    0x1000 = 0, 0x1040 = 4, 0x10b0 = 0x0002305c, 0x10b4 = 0, 0x10b8 = 0x83c00000

This is `mtx1814` (`.text+0x43140`), the kernel-internal draw builder and the
**only emitter of opcode `0x41` block loads**; the preamble is written at
`+0x43907..+0x4394f` immediately before the packet. [BIN, via
`docs/parhelia-draw-builders-kernel.md` and `-3d-submission-audit.md` §307]
`0x83c00000` decodes as one 4-float attribute at dword 0, `last` set — the
kernel's own one-attribute vertex declaration. [BIN]

So a `0x41` block load is **consumed through the vertex-fetch frontend**: the
`[count][first][1][0]` sub-header is the `v0` the constant-loader microprogram
reads, and the payload streams behind it as further "vertices". The frontend
needs a primitive word and an attribute descriptor to size that stream. Our
stream never wrote `0x10b0`, `0x10b4` or `0x10b8` before the block loads — the
first thing we wrote to any of them was after them — so at packet 8 the
frontend was running on whatever the previous context had left: on a machine
that had just run the vendor GL client, `0x10b0 = 0x0006305c` (bit 18 set, the
legacy per-vertex-register path) and the real 2-attribute descriptor list. The
engine accepted the packet and never retired it. [INF, from the alignment +
[BIN] for what the preamble is]

That the *first* `0x41` after the first bind is where it stops is exactly what
this predicts: the bind itself is an ordinary register list and completes; the
first packet that has to be fetched is the one that hangs.

Note the direction of the primitive word: the vendor uses `0x0002305c`
(bit 18 **clear**) for the `0x41` phase and `0x0006305c` (bit 18 **set**) for
the real `0x97` draw. Our stream used `0x0002305c` for the real draw — the
loader-phase value — which would have been the next failure even if the
preamble had been there. [CAP]

### 3.2 Our own constant loads ran under the wrong microprogram

The splice point was chosen as "after the last packet whose opcode is `0x41`".
Three different things share opcode `0x41` (`docs/parhelia-opcode-41.md`), and
the last of them in the prologue is the **base-register** packet `0x4106000f`
at vendor 104 — which is bound to loader C, not to the constant loader. Our
identity MVP and viewport therefore executed under the program that installs
`b0..b3`/`a0..a3`, i.e. they were written as base registers, not constants.

The fix keys on the sub-header instead: a constant block load is the only
`0x41` form that carries `[count][first][1][0]` with `pk[3] == 1`, `pk[4] == 0`
and `count*4 + 5 == len`. Both other forms (`0x41020007` bind table,
`0x4106000f` base registers) fail that test.

### 3.3 Order of the tail

Our state block wrote `0x12b0` (surface select) *before* the render-target base
and config, and put the combiner slot writes at the head of the image with 31
unrelated register writes between the `0x1374 = 0` reset and the slot writes
the reset arms. The vendor does the exact opposite in both cases (116–119 and
122–125). Neither was the stall, but both are gratuitous divergence and both
are now emitted in the vendor's order.

## 4. Questions the task asked, answered from the capture

**(a) Does each `0x41` need its own bind?** No. **One bind covers a run.**
Vendor packet 94 binds loader B and packets 95–102 — eight consecutive block
loads — follow with no bind between them. A bind is re-issued only when the
*program* changes: 92 (bind table), 94 (constants), 103 (base registers). [CAP]

**(b) Must the constant-loader bind be re-issued after an intervening register
list?** Not observed to be needed within a run — but the vendor never puts one
there, so this is untested. What the vendor *does* do is terminate the run with
a bind-shaped packet that has no `0x1100` (packet 105:
`1374=0; 10f0=0x20000028 ×2; 1130=…`), which reads as an explicit unbind. [CAP]

**(c) Does our per-packet call splitting break a required adjacency?** No.
`tools/build-ring-replay.py` splits the vendor's header-ful sections into one
call per packet as well — including the run 95–102, which the vendor emitted
inside a single secondary section — and that replay renders. Per-packet
splitting is therefore proven not to break the run. [CAP + the working replay]

**(d) Must the base-register packet precede the constant loads that use `b1`?**
No — the vendor emits it *after* them (104 vs 95–102). The block loads address
the constant file by an absolute `first` index; `b0..b3` are relative-addressing
latches read by the *vertex* program, which is bound later still (packet 108).
Ordering constants-then-bases is what the working stream does and what we now
do. [CAP]

## 5. The fix, as implemented in `tools/gen-tri-stream.py`

1. **Emit the block-load preamble.** `vreset` (`0x10b0 = 0x60`;
   `0x1000..0x101c = 0`, `0x1040..0x104c = 0`; `0x1064 = 0xffffffff`,
   `0x1060 = 0`; `0x10b4 = 0`) and `vpre` (`0x1000 = 0`, `0x1040 = 4`,
   `0x10b0 = 0x0002305c`; `0x10b4 = 0`, `0x10b8 = 0x83c00000`) are inserted into
   the lifted prologue immediately before the bind that precedes the first
   `0x41` packet. Both reproduce the vendor's packet boundaries and tag packing
   byte for byte (`0x212c1000`, `0x21212e2d`, `0x21211819`, `0x2121212d`).
2. **Splice our constants by sub-header, not by opcode** — `is_blockload()`.
3. **Restore the real vertex format after the loads**, in the vendor's order and
   with the vendor's values: `0x10b0 = 0x0006305c` first, then
   `0x1000 = 2`/`0x1040 = 4` and the zeroed stream table, `0x1064 = 0`,
   `0x1060 = 0x280cc`, `0x10b4 = 0`, then the descriptors.
4. **Re-emit `0x1000 = 3`, `0x1040 = 4` immediately before the draw packet** —
   the pushbuffer's memory-class word, which `mtx1017+0x4bf` re-emits per draw.
   [BIN]
5. **Tail in vendor order:** render target, `0x1324`, `0x12b0`, DWGCTL,
   `0x12f4`, then `0x121c`/`0x1374` reset directly followed by the combiner
   slots, then the class word, then the draw.

## 6. Result

    before:  33 calls,  425 dwords,  27 packets aligned with the vendor
    after :  55 calls,  504 dwords,  48 packets aligned with the vendor

Every packet from vendor 90 through vendor 128 — the whole WARP-bind,
block-load, vertex-format, render-target and draw spine — now has an exact
counterpart in our stream, in order. The alignment of the fixed stream, with
only the deliberate differences left:

    G         0  REG:11c0,11c4,11c8,11cc,11d0,11d4,11d8      our raster defaults
    =   49    1  REG:1328,132c,12fc,12f8
    =   50    2  REG:12e8,12e4,12f0,12ec
    =   51    3  REG:12c8,12cc,12d0,12c0,12c4
    =   52    4  REG:12d4,12d8,12dc
    G         5  REG:1318,1314                               vendor has it at 65
    G         6  REG:12b8                                    from the mtx398 static block
    =   53 …  7  … 61 … 15   (WARP reset + vertex-fetch reset, exact)
    =   90 … 16  … 115 … 43  (preamble, binds, block loads, vertex format, exact)
    G        44  REG:1238,123c                               our RT slot 3, vendor slot 0/1
    =  118   45  REG:1324
    G        46  REG:12b0                                    without the 0x707 fence
    =  120 … 47  … 128 … 54  (DWGCTL, 12f4, combiner, class word, draw, exact)

and the two inserted loads at G29/G30, now inside loader B's run:

    =  102   28  BLK:17d/2
    G        29  BLK:000/4        our identity MVP
    G        30  BLK:160/2        our viewport
    =  103   31  REG:1130,1374,10f0,10f0,1100

A machine check confirms the placement: every `0x41` in the image is now
covered by MODE `0x20000028`, the two injected loads under the same
constant-loader entry as the vendor's own eight, and only the base-register
packet under the third entry.

## 7. Remaining structural differences, all deliberate

| difference | why |
|---|---|
| vendor 0–48, 62–89, 126: clear passes, per-light blocks (`0x1164..0x1198`), three 2D blits, fence write-backs (`0x1090`/`0x1094`) | frame bookkeeping aimed at the client's window; we render one triangle and want none of it |
| we write RT slot 3 (`0x1238/0x123c`), vendor slots 0 and 1 | `--slot`; the surface select `0x12b0 = 0x3fff8842` is the same |
| no `0x1300 = 0x707` + `0x12bc` fence pairs anywhere | DWGSYNC is ordering only, not needed to draw (`parhelia-3d-integrated.md` §4). If the `0x12b0` rebind ever misbehaves, vendor packet 119 is the pair to restore |
| `0x1130` = `0x0700200c`/`0x0200200c` in ours, `0x0700300c`/`0x0200300c` in the vendor's span | the value is copied verbatim with the lifted prologue; the two come from different captures with different GL state. Bit 12 is undecoded [INF]. It stays self-consistent as long as the prologue and the WARP entries come from the *same* capture — which they must anyway, since the entries are bus addresses into that boot's program heap |
| `0x12b8 = 0x00070601`, `0x11c0..0x11d8` written by us and not in the vendor's span | they belong to the `mtx398` static block the vendor carries header-less in the primary ring between calls (`parhelia-3d-integrated.md` §3); replaying only the sections omits them, so we emit them ourselves |
| our two extra `0x41` loads | the whole point: our own identity MVP at `c[0..3]` and our own viewport at `c[0x160]/c[0x161]` |

## 8. The comparison, reproducible

Walker (decode a replay triple into per-packet signatures) and aligner:

```python
def regs_of(pk):                       # opcode-0 register list
    out, j = [], 1
    while j < len(pk):
        tag = pk[j]; j += 1
        for k in range(4):
            idx = (tag >> (8 * k)) & 0xff
            if idx == 0x21: break      # ends the TAG, not the packet
            if j >= len(pk): break
            out.append((0x1000 + idx * 4, pk[j])); j += 1   # 0x20 -> 0x1080, consumes
    return out

def sig(pk):
    op = pk[0] >> 24
    if op == 0:    return 'REG:' + ','.join('%04x' % r for r, _ in regs_of(pk))
    if op == 0x97: return 'DRAW'
    if op == 0x41:
        if len(pk) >= 5 and pk[3] == 1 and pk[4] == 0 and pk[1] * 4 + 5 == len(pk):
            return 'BLK:%03x/%d' % (pk[2], pk[1])
        return 'BLK*:%08x' % pk[0]
    return 'OP%02x' % op
```

Walk `(ring, sec, reloc)`: for each reloc row `(ring_index, offset, len, …)`
emit `sig(sec[offset:offset+len])`, and for the ring dwords between the
previous call's five and `ring_index` emit the header-less bare tags they
carry. Then `difflib.SequenceMatcher` over the two signature lists.

Rebuild and re-align with:

    python3 tools/build-ring-replay.py captures/live-ring-0528      # vendor triple
    python3 tools/gen-tri-stream.py captures/live-ring-0528/secs2.txt \
            captures/live-ring-0528/gen.hex                          # ours

`build-ring-replay.py` needs a `ringall2.txt`/`secs2.txt` pair from the **same**
capture run; the pair currently in the tree does not match (the ring's calls
point at `0x1764…`, the sections are dumped from `0x191c…`/`0x18d0…`), so the
vendor side of this comparison was done against the already-extracted
`ring7.hex`/`sec7.hex`/`reloc7.txt` — the triple that was proven to render.

## 9. Not addressed here

The prologue is still lifted: the bind table at vendor 93 and the three
`0x1100` entries are bus addresses into the live client's WARP program heap and
are only valid on the boot they were captured on (`parhelia-3d-integrated.md`
§6). Generating our own programs and binding them is item 1 of §7 there, and is
unaffected by this fix.
