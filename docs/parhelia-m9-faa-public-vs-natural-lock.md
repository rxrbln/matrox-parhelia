# M9128 public VS: natural Lock after the real DRAW observation

The genuine M9128 public-VS graph reached native point `0x160` on boot
`a07b7245-b3bd-406e-944b-14cc8b4d89ef`. Its same-boot retained counters were
`+00=0x161`, `+20/+40/+60=0x10`, and transport was idle without a wedge. The
source continuation admits that exact capsule, restores the original caller
at `252b9`, and supplies `0x161` only at the draw's desired `0x170` query.
It returns the genuine public DRAW result (`EAX=1`) and enters the normal
`a8a60` Lock through the retained resource wrapper.

The Lock writes every byte of source primary `[5004,8576)`, a 3572-byte
suffix, before its final wait. It posts END `8564`, repeats END `8564`, then
posts END `8576`. The last source query is common `+00` at `4e1d1`, desired
`(0x190,0)`, and is deliberately unread. No returned Lock, F021 mapping, or
pixels have been supplied. A native graph with the existing four-byte clear
alignment delta would post END `8568`, repeat `8568`, then `8580`; it must
derive these from its live primary cursor and actual clear length.

The two intervening common `+20` reads at `4e2fc` and `4e32d` have desired
`(0,0)`. Original M9128 instructions mask the low counter to 16-byte
granularity, update a cached wrap count, and test this zero floor; they do
not request a new positive retirement point. CPU controls using hypothetical
low words `0`, `0x10`, `0x20`, `0x100`, and `0xfffffff0` produce identical
Lock bytes, relocation sequence, END sequence, and final unread `0x190`
boundary. Those controls exercise the source branch; they are not hardware
replies. The only actual `+20` value used is the same-boot `0x10` observed
at `0x160`.

The typed fixture is
`captures/m9-faa-graph-public-vs-20261007/natural-lock-160/plan.json` and
`plan.bin` (SHA-256 `bb5fb2de14dcc28936460811355ba8fa04984760bcc8a7e49421059a02e9de27`).
It has eight live address fields: owner 22 at offsets `0`, `0x284`, `0x2c4`;
graph sentinel owner 8 at `8`, `12`, `16`; and primary owner 1 twice at zero.
Six source null bindings remain null. Five timeline fields are source
`0x171`, `0x181`, `0x180`, `0x180`, `0x191`, all derived from the live draw
epoch. Two extent words encode the bounded 320x240 shape. The builder checks
the original CPU writer of every byte and rejects unjoined source changes;
the checker reconstructs the source image and verifies two other live owner
placements and two other epochs. Native production must validate retained
owner capacities/domains and generate all address, timeline, and extent
fields from live state under the capture/replay policy.

Run `python3 tests/m9-faa-graph-public-vs-20261007/check-natural-lock.py`
from the RE worktree. The exact actual `0x190` receipt is still required
before resuming the original Lock return or reading a F021 view.
