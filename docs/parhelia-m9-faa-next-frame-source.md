# M9128 second public DRAW on the retained FAA graph

The first graph Unlock has a real M9128 receipt: boot
`5f86969d-acd4-4d9c-abf3-a27c7a13bdac`, raw counter `0x191`, transport
submitted/completed `0x190`, generation 0, queue 0 and wedged 0. The source
controller checks that capsule before emulation. It also checks that the
same-boot earlier Lock observed channel `+20=0x10`. The original Unlock
producer writes only the channel-0 timeline fields; its independent `23b80`
finish can therefore consume the observed status inputs without inventing
new GPU replies. It stops the independent finish at its status load and
discards that caller's stack, preserving the original graph and context as
the established INIT/DRAW/Lock controllers do. An optional side probe shows
that resuming its post-wait finalizer reaches miniport MDL release; that is
not a prerequisite to submit a new public DRAW on the retained graph.

The next genuine `beeb0` call uses the same context and retained owners. Its
caller changes the three triangle positions to `(64,192)`, `(256,192)`,
`(160,48)` and supplies diffuse `0xff0000ff`. The already bound first-frame
constant-red pixel shader remains in force, so the diffuse change is a
source-input control, not a claim that output color changes. The original
`17e480` preprocessing launcher executes once. This DRAW stops at original
`252b9` requesting `0x1d0`, with the GPU load unread. A separate genuine
`23b80` finish posts one END and stops before its `0x1e0` status load.
Neither point is a second-frame hardware receipt; neither the second DRAW
nor a new normal Lock has returned.

The CPU source appends 24 bytes to the retained primary, `8644→8668`: its
two 12-byte indirect packets address secondary ranges `[1372,1980)` and
`[1980,2040)`. The second is followed by the independent final END. The
secondary owner receives 668 new bytes at `[1372,2040)`, with 11 active
typed owner fields, three null typed slots, and five timeline tokens for
`0x1a0`, `0x1b0`, `0x1c0`, `0x1d0`, `0x1e0`. The retained geometry/system
owner receives 60 caller vertex bytes at `[464,524)`. No new graph owner or
program allocation is observed. The native first Unlock had a four-byte
page-clear expansion, so an eventual native producer must start from its
live primary cursor `8648` and derive final cursor `8672`; the source
`8668` is not a portable native endpoint.

`captures/m9-faa-next-frame-20261007/native-blobs.bin` contains a 692-byte
source-state template: 24 primary and 668 secondary bytes. Every observed
source address and timeline word is zeroed. The accompanying plan records
the live owner and offset encoding for four direct primary fields, 11
secondary fields and five timeline fields. Kind 19 uses the owner-26 base
with tag 1 despite a logical source offset of 32; kind 20 is the companion
encoder's `4`, not a generic pointer. Geometry bytes are omitted from the
blob and represented as typed `<float x,y,z,rhw; u32 diffuse>` caller values.
Native admission still requires live owner capacity/domain/retention checks,
exact post-Unlock identity and point, safe CPU writes/fences, and actual
`0x1e0` retirement. The plan is source-only and has `executable=false`.

Reproduce without target access, using the original M9128 vendor image and
the archived receipt:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/m9_faa_graph_unlock190_continue.py \
  --unlock-receipt-dir tests/m9-faa-graph-unlock-20261007/hardware-20261007 \
  --out /tmp/m9-faa-next-frame/second-draw.json
/home/ai/tmp/faaemu/venv/bin/python tests/m9-faa-next-frame-20261007/check.py \
  --source /tmp/m9-faa-next-frame/second-draw.json --self-test
```

The large full trace is regenerated in `/tmp`; only the stripped native
template, plan, and checker are committed. Captured VRAM, PFN, GPU virtual,
CPU or MMIO addresses are evidence of the original source run, never native
parameters.

## Native second preprocessing now retires on M9128

Canonical DRM `e8ee451` (WIP `271df45`) and helper RE `1ea3576d` implement
this retained-graph continuation with caller-owned live XYZRHW/diffuse words.
The encoder derives11 resource fields, four indirect endpoints and five
relative timeline values. It accepts the authenticated live primary cursor,
preserves the full completion epoch, and rejects invalid inputs before writes.
All six graph BOs receive the final fence before the single END. Posted failures
keep the private and borrowed owners quarantined through safe teardown.

Actual boot `b0dc74fa-3207-46aa-b51b-7ed6182ca9fb` completed INITb0, first
DRAW/Lock170 and readback, Unlock190, then second preprocessing/finish1e0.
The new vertex positions are (64,192),(256,192),(160,48). Source bytes8668
become native8672 through the live INIT clear delta; the graph has12 posts.
Immediate and later snapshots show raw1e1, submitted/completed1e0, next1f0,
generation0, queue0, wedged0. A repeated next-draw request returns ESTALE.

Evidence is `tests/m9-faa-next-frame-20261007/hardware-20261007`, checked by
`check-hardware.py` with eight altered-receipt refusals. Module SHA
`326d24d2c2098953e0a5302d0360e359d1ce596e5d442def10e59bce9af19533`;
helper SHA `c9d1e9b899309cc56482a2f78e06b57cf6e93789a77db2c2728399b1797017a0`.
The raw image in this capsule is the **first** Lock image, taken before
Unlock and the second draw. It still passes the triangle oracle, but supplies
no second-frame pixel evidence. Original second DRAW return, raster finish,
normal Lock and its resulting image remain the next boundaries.


The same retained diagnostic graph now completes a changed second image.
Native renderingfinish210 and reused normalLock240 retire. After the actual
240-gated vendor Lock returns the same retained F021 mapper (REc97f6d75),
DRM8552a94 admits only the exact retired second Lock state/point/cursor/owner.
Freshboote0647539-cb37-43b9-84c5-202f5cd53b2e produced second14065 colored
pixels/528 partial edges,4704 changed packed pixels,zero geometric/channel
errors. Pixel/receipt mutations pass in tests/m9-faa-second-view-20261007.
This proves two changed geometry draws on one graph, not continuous app FAA.
