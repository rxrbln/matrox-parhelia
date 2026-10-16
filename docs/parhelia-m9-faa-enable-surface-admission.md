<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Genuine public PDEV admission publisher

The retained source fixture's `PDEV+30 == 0` is an unexecuted initialization
publication, not an inferred chip or FAA mode bit. Shipping `DrvEnableSurface`
at `10820` writes it to one at `10963`, after genuine `194f0`, `1a1c0`,
`1a590`, `1ff90` and `20b20` return successfully. Its failure cleanup writes
zero at `1091e`. Public `CreateSurfaceEx` tests this field at `bf18f` before
proceeding to the normal association dispatch. Assigning one in a fixture
would bypass these real dependencies.

Three independent fresh-owner controls now execute the unchanged shipping
entry. They use actual M9128 retained runtime/property owners, registry
`101f8`, the existing strict OS miniport bridge, and the existing ordinary
CPU thread identity contract. No GPU/status/counter/pixel input is supplied.

* With no queue constructor, `1a590` reaches `230fb` with null EDI. The
  observer stops before the null owner read; `PDEV+30` remains zero.
* The real paired startup-table entry at `1f2578` selects `1a260`. Executing
  that constructor publishes a live queue and allows activation/resource
  enable to complete. Without target managers, `20a80` then reaches
  `21630` with null ESI; the observer again stops before the read and the
  public flag remains zero.
* Executing the existing genuine `20810` target publisher (paired startup
  entry `1f2620`) before `DrvEnableSurface` supplies its real head/shared
  owners. The unchanged entry then executes exactly one observed write,
  `10963: mov [ebx+30],1`. It continues through `25c90` and `27900` to
  `10d10`, where strict miniport request `23201c`, input/output sizes zero,
  remains unjoined. This control stops before forwarding that request.

The earlier `1a1c0` request is closed as a CPU owner publication. Its actual
`233054` input is `[0,0,1,0]`; MM `7190e` selects `72643` at `71a4b`.
The live function pointer at `47b74` selects `6b16c`, whose `6b177` store
writes the real miniport owner into shared profile `+1a8c`. The third
argument is not the value written by this setter. Only the observed four
CPU-written bytes are imported into the already shared MD owner. No bulk
graph replacement, fake success, GPU input or unknown byte-count reply is
needed. The real handler returns zero and the MD caller consumes its own
unchanged continuation.

This proves the actual admission flag producer and two prerequisite owner
boundaries. **It does not prove a returned `DrvEnableSurface`, public copy
or Lock admission, a required FAA flush, or rendered pixels.** The old
explicit `ac1f0` experiment remains below the unjoined public read gate.
The public draw association list is independently proven populated; this
initialization boundary must not be recast as a missing list node.

The checker pins both shipping images, exact writer/consumer instructions,
source startup-table entries, actual MM setter selection/store, returned
queue/target owners and the unexecuted next request. Five controls reject
an invented writer, false full return, wrong shared publication, missing
queue constructor and an unqualified I/O reply. All commands are host-only.

```sh
sh tests/m9-faa-enable-surface-admission-20261007/capture.sh
sh tests/m9-faa-enable-surface-admission-20261007/check.sh
```

This source prefix proposes no extra command, reset or admission-flag fix
for the still-black final FAA output.
