<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Public draw association and dirty graph

The passive observer follows the existing public constant-red, black-clear,
triangle and original output-stack recipe. Its only supplied GPU inputs are
that recipe's source-qualified hardware counter receipts. It changes no
caller, surface, association list, dirty flag or driver code. The existing
recipe rejects any changed source manifests and stops before pixel reads.
This is CPU lifecycle evidence; the corresponding native FAA output remains
black.

The first real `139636 → 101050 → 1013c0` sees an empty association list.
`101050` then calls its ordinary insertion producer `101350`. Before the
actual public draw returns, there is exactly one returned node containing
`(primary wrapper +18, companion wrapper +20)`. Both normal virtual
publications (`1010fe` slot `+5c` and `10110a` slot `+74`) reach `a7200`.
Thus the association is present. An empty list is not a missing prerequisite
in this capture.

`a7253` writes the source outer `+18=1`; `a7277`, `a7297`, `a72fc`, and
`a730b` publish its four backing graph words. These are guest CPU writes
through the returned interfaces. Both genuine `17e480` launches observe the
dirty flag. The explicitly internal `ac1f0` operation subsequently runs while
that same node and dirty flag remain live. Neither `101290`, `a7390`,
`a7440`, nor `aa750` executes before this output operation or before the
actual `8b390` pixel-copy boundary. This proves the omitted public operation
within the bounded recipe, not that inserting it would fix rendering.

The distinction matters when choosing a natural caller control. `1013c0`
finds a node if either interface identity matches; replacing both interfaces
with an unrelated FAA target does not find the old node. Source `101050`
uses `101290` only when that lookup finds a conflicting pair. Its normal
first insertion does not flush anything. Another genuine call to `101290`
is wrapper cleanup at `a479a`, itself called from `7a5aa` during association
replacement and `5100f` during resource teardown. A useful positive control
must reach one of these actual producers and preserve source ownership; a
manual list update or arbitrary direct binder call would not close it.

The source resource's `+3fc` remains zero throughout this recipe. Its real
publisher `51d46..51e16` requires profile class4, format31/32 and request bit
`20000000`. That branch describes the embedded graph at resource `+3e8`,
whereas the actual public draw here publishes the separate outer graph at
resource `+420`. The public `a74b0` read branch gates its embedded-graph
`ac1f0` call on `+3fc`. It is not legitimate to set that flag on the sampled
surface or to claim that the current internal call has acquired public Lock
admission. The correct caller/direction for this returned sampled graph
remains the next source investigation.

Reproduce on the host, without target access:

```sh
sh tests/m9-faa-public-draw-lifecycle-20261007/capture.sh
sh tests/m9-faa-public-draw-lifecycle-20261007/check.sh
```

The checker pins the shipping MD image and actual publisher/lookup/gate
instructions, both returned interface publications, all five graph writes,
list insertion and retained dirty state. Missing publication, missing list
node, an invented public flush and false public read admission are rejected.
No new hardware profile or product fix is proposed by this checkpoint.
