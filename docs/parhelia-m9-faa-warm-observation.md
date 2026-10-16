# M9128 warm-state observation boundary

Two read-only board fields classify the saved vendor's 0540 warm helper:
BAR2 DWORD `1c10` and DWORD `1fdc`. The fresh-before-module M9128 observation
on bootdf3986cd is now retained: `00000001` and `04000000`, so the source
warm-helper early-return predicate is false. Root's exact-source checker
passes with `--require-fresh`. This does not prove that the native driver
omits the later transition, full MC/PLL initialization or the cause of black
FAA pixels. See [actual fresh-board evidence](../tests/m9-fresh-warm-entry-20261006/README.md).

The exact Linux X image is SHA256
`3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`.
Its board-chip dispatcher at `2eb7b0/2eb7c8` selects `2f2c10` for WORD chip
`0540`, separately from the `2539` sibling. The helper calls accessor
`2b8ad0` for `1c10` at `2f2c33` and `1fdc` at `2f2c43`. It tests low two
bits of `1c10` at `2f2c48..2f2c55`; equality to two selects `2f2e98`, which
tests byte two of `1fdc`, bit zero (DWORD bit 16). Both predicates true take
the direct epilogue at `2f2ea3` without the clock/display transaction. A
failed predicate goes to `2f2c5b` and the existing source transition.

This hardware predicate is inside a separately admitted warm caller.
It does not replace the initialized CPU bucket/provider contract documented
in [the inherited admission checkpoint](parhelia-m9-faa-inherited-init.md).
In particular, a matching pair does not prove that the software enable
wrapper selected the warm helper, that cold MC/PLL initialization succeeded,
or that FAA rendering works. A nonmatching pair does not license copying
the helper's later writes out of their complete board-owned transaction.

## Existing observation and minimum new boundary

The parent collected the actual fresh boundary in
[observation.json](../tests/m9-fresh-warm-entry-20261006/observation.json):
M9128 PCI `102b:0540`, subsystem `102b:22c0`, boot
`df3986cd-a4c0-4a4a-a5bc-3fa13a41b5d3`, kernel `7.2.5-t2`, native module absent,
before X/service submission. The read-only helper SHA256 is
`9f5937ff0bb1eb38492806869fe9009b75f248b2a868331401c1d5277c7afc82`.
The words are `1c10=00000001`, `1fdc=04000000`. The source direct-return
predicate is false. This qualifies that exact branch observation, not execution
of the warm or cold transaction. The retained raw output repeats `1c10` through
a display alias; both observations agree, and no additional target read was
performed to remove that formatting duplicate.

See [the full warm/native ordering audit](parhelia-m9-faa-warm-native-order.md)
for the source-selected transaction and its distinct native ordering.

The retained `adapter-refresh-idle.json` observes `1c10=00000002` and
`1fdc=04010000` on M9128 LP PCIe x16, PCI `102b:0540`, subsystem `102b:22c0`,
boot `815d4dcc-e895-45c3-85ec-b54fda4fce04`. The stated scope is an idle
BAR2 read after native batching, with established locks held and no writes.
It satisfies the source warm predicate at that observation boundary.
It cannot establish the state before native initialization on that boot or
on a subsequent power cycle.

The minimum new collection is the same two aligned DWORD reads on one fresh
physical boot, before loading the native module or submitting a service.
Retain actual device/subsystem identity, boot ID, collection scope, zero
writes, and the established idle/exclusive locks. These are the source's
bounded status reads, not a request to perform a cold/reset transaction.
The collector should explicitly mark `lifecycle_boundary` as
`fresh-before-native-module` only when that chronology is actually retained.
Root owns collection; this checkpoint performs no target operations.

An arbitrary idle `3b0`, `3c4` or GPIO grant word would not instead close
the uninterrupted Windows cold prefix. Those source reads follow their
own preceding hardware request writes. A separately recorded idle value
without the same device, boot, and request chronology remains an observation
at another boundary, not a valid response to the pending emulated request.
The warm pair is useful specifically because its actual source reads occur
before this helper's first hardware write.

## Host-only check

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-warm-observation.py \
  --image /home/ai/src/parhelia/xf86-video-parhelia/m9xdriver-x86_64-1.4.3-80847-20140908-build_98/xserver/1_13_0/m9x_drv.so \
  --observation captures/m9128-device-inputs-20261006/adapter-refresh-idle.json
```

This reports the existing warm predicate true and fresh boundary false.
Adding `--require-fresh` rejects that existing post-service observation.
The checker verifies exact source bytes and M9128 device/subsystem identity,
then evaluates the pinned expression against the independently retained
words. It never supplies a bootstrap/provider reply or invokes the warm,
cold, MMIO, or GPU path.
