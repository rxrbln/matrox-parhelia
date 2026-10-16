# M9128 retained second DRAW after the actual 0x1e0 receipt

The independent M9128 target run on boot
`b0dc74fa-3207-46aa-b51b-7ed6182ca9fb` retired the private
`FAA_GRAPH_NEXT_DRAW` prefix at point `0x1e0`: debugfs reports raw channel 0
`0x1e1`, submitted/completed `0x1e0`, next `0x1f0`, empty queue and no wedge
immediately and two seconds later. The signed firmware digest is
`e2a645a12dfe80149109b5687e05fe04edcf4d55786ae51c0f0f783e18e4d85d`;
module and smoke identities are pinned by the controller. This is a prefix
receipt, not a public DRAW return or pixel result.

`m9_faa_next_frame_receipt_continue.py` reconstructs the same genuine source
caller and restores its saved stack and registers at original wait `252b9`.
Only after validating that separate target capsule does it place the observed
channel-0 value into the emulator's common buffer. The original second public
DRAW then returns with `EAX=1`. It writes source secondary owner 29
`[2040,2732)`, appends 12 bytes to source primary owner 27
`[8668,8680)`, and writes one `COMMAND_END` at source primary end `8680`.
The corresponding native end would need to derive its live cursor; the current
320x240 native path's clear delta is four bytes, so its present cursor would
be `8684`. The source's two new secondary timeline words include `0x1f1`
and `0x201`; the complete packet is not yet a native producer.

An explicit genuine `23b80` finish on the retained queue then writes
secondary `[2732,2748)` and primary `[8680,8692)`, posts a second END at
source `8692`, and stops before its status load at desired point `0x210`.
The next genuine normal Lock on the same surface starts after restoring the
original DRAW caller stack, writes source primary `[8692,12228)` (3,536 bytes),
then stops at another `252b9` read for that same unqualified `0x210` point.
The observed `0x1e1` cannot cover either read. No Lock `COMMAND_END` occurs
before its read, no new GPU status or pixel bytes were supplied, and the
independent finalizer stack was not resumed. Later Lock publication, pixels,
and another Unlock remain open.

The controller's source-only mode verifies the saved original `0x1d0` wait
without supplying a future status. Its receipt mode checks boot continuity,
state 7, actual point and transport, signed firmware/module/helper identities,
and the source plan before emulator execution. The companion checker executes
both modes and seven altered-capsule refusals. It does not treat previously
observed nonzero counter lanes as fresh second-DRAW replies.
