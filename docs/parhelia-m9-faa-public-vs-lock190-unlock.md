# Public-VS Lock 0x190, F021 view and next Unlock

The real M9128 combined public-VS run on boot
`ea08564f-19cb-4044-95a6-b91d5825a9ef` retired INIT `0xb0`, HEAD
`0xc0`, natural DRAW observation `0x160`, and normal Lock `0x190` in that
order. After Lock, transport reported submitted/completed `0x190`, raw
`0x191`, common lanes `0x191/0x10/0x10/0x10`, queue zero and no wedge. The
helper wrote a complete 307200-byte F021 readback, but the 20-second outer
timeout returned 124 before the helper's final result was printed. The
helper's saved-view `printf` is buffered and a further two-second sleep
precedes graph RELEASE, so this receipt does not locate a release hang. It
is not a successful complete helper session. The independent raw SHA-256 is
`a3279ef8fde3fe105d006f411559c71f8812bce48cc4e6a7fd449afab4dbeec5`.
The live FS range fix bound the authenticated cyan `(0,1,1,0.5)` constant;
the pixel oracle found 9801 colored pixels, 9361 full and 440 partial, with
zero interior, exterior, channel and utility-red errors. The native fix and
real pixels are separate from the unresolved release hang.

The RE controller has a distinct **retirement-only** admission for that
exact partial capsule. It verifies same boot, module/helper and source-input
hashes, each actual phase, the Lock ioctl and independent wait, retired
counter lanes, both transport snapshots, and the trusted 3572-byte Lock
template. It keeps helper `rc=124` and `raw_qualified_for_pixels=false` in
the source continuation. A full-success admission still requires `rc=0`
and the saved-view line. Neither path supplies fabricated GPU statuses.

With only the qualified `0x191` common value, the original `4e1d1` Lock
wait returns `1`. Source F021 publication is `C4=aperture|1`,
`C8=f0210140`, `CC=00010100`, then `C0` for 80 pages. All 80 CPU-root
pages join retained primary owner 1 at matching offsets; 75 visible pages
cover 320×240×4 with pitch 1280. The emulator reads mapper metadata and
page tables, never GPU pixel bytes.

The genuine same-wrapper `a8ad0` Unlock then returns `1`, releases
FRONTEND_STATE at `0x31c=0`, writes source primary `[8576,8660)` (84 bytes),
and posts END `8660` twice. Its two address fields both refer to retained
primary owner 1 at offset zero. A separate genuine queue finish adds the
12 source bytes `[8660,8672)` and posts END `8672`; the status load at
`23bc3` remains unread with desired common `0x1b0`. No finalizer resource
release or new completion is inferred.

The full 96-byte Unlock image is byte-identical to the existing typed graph
Unlock template after deriving its two owner-1 addresses, two timeline
fields from Lock epoch `0x190`, and bounded 320×240 extent. It therefore
needs no new literal packet profile. The next native continuation can use
the retained public-VS graph and the existing encoder, with its own exact
admission/fence ownership; native ENDs are `8664`, repeated `8664`, then
`8676` because this graph's primary clear adds four bytes. Hardware point
`0x1b0` and later repeated rendering are still unproved.

The source and typed reuse proof are under
`captures/m9-faa-graph-public-vs-20261007/natural-lock-190-retired/`.
Run `python3 tests/m9-faa-graph-public-vs-20261007/check-natural-lock190.py`
for the source proof. Add `--receipt-dir PATH` to revalidate the exact
partial hardware capsule and live cyan pixels independently.
