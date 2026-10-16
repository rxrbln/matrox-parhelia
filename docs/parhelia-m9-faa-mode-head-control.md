# Earlier head publication before the conversion-resource mode query

The missing earlier **head-control prefix**, rather than a proved need to swap
miniport owners, explains why the partial constructor fixture rejected its
retained owner's head selector. A separate source execution now runs the
original control and observes the actual writer publishing head0. After the
original mode-generation control, the count helper genuinely writes0 and
selection returns11. The constructor remains uncalled. This is partial emulator
startup evidence, not a cause of the native FAA black image or successful pixel
readback.

## Exact owner and ordering

`m9_miniport_io.py:17` retains the bootstrap adapter root. Its `:68..70` forwards
bounded allocation requests with that owner. The mode-only constructor helper
also uses this owner at `m9_pdev_copy_resources.py:70/:91`. The observed root has
class1 at `+1c`, head `ffffffff` at `+20`, and is itself the first element of its
profile's `+2fcc` child chain. The existing opaque OS handle and MD head-slot
publication do not prove that the MM head assignment has executed.

Shipping MD `11940` contains the earlier mode-enumeration sequence. Its prefix
`11968..1197c` emits a zero-input/output `232304` request at `11974`. MM `5d9ec`
checks owner `+4c`; the observed initial value is `ffffffff`, so `5d9f7` calls
`5db84`. The latter traverses class1 parents and their actual children via
`12584/1258c/125a3`, and calls `5801f` at `5dbae`. That publisher reads the live
counter at shared-owner `+21f898`, advances it, and stores its old value in
`owner+20` at **`58035`**. The retained source trace observes exactly this store,
head0. No head value or child pointer is injected.

The separate controller then runs original MD `2deeb..2df01`, which emits
`232024` at `2defb` before `64bd0`. Its MM `70916` branch enters **`5fc1e`**.
`23201c`, a different request, enters `70a41`; those producer chains must not be
confused. This control changes the real typed-record query: `6dd98` now sees
success0 from `6093c`. The actual head0 passes `6dd9c`'s head<4 guard, and
**`6ddb5` writes0 to the real count out-parameter**. The existing strict
constructor helper observes this CPU store before its selector uses the value.
The selector returns status11, and the actual transport returns8 output bytes
`0b000000/00000000`. Thus0 is a written count in **this separate partial-startup
case**; it is not the unwritten local from the previous mode-only experiment.

## Scope and remaining prerequisite

The controller executes two original instruction prefixes with live owners and
the source-derived opaque OS handle. It forwards their real requests through
`6c755`, propagates only actual MM CPU stores to already shared MD allocations,
and retains new MM allocations. It supplies no mode records, query success,
wrapper pointers/readiness or GPU replies. Constructor mode fields and d0/d4/d8
remain0. No mode-device MMIO events occur in this execution.

It does **not** execute the full `11940` caller. In particular, `294e0`'s shared
locking publication, subsequent `232350/232344` controls, and later enumeration
or fallback policy are not qualified by these two prefixes. The full earlier
mode-list generation/eligibility inputs remain the next prerequisite. A written
count0 here does not prove that the target has no modes or that its physical
EDID is absent. The independent GDI request1024×768/32bpp60Hz remains a controlled
fixture input, not an actual target mode or evidence of completed Windows
startup. Head/configuration/resource effects differ from old FAA captures; no
old retirement receipt may be reused.

## Reproduction

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9-vendor-faa-mode-head-control.py \
  --out /tmp/m9-faa-mode-head-control.json
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-mode-head-control.py \
  --proof docs/traces/m9-faa-mode-head-control-20261007.json
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-copy-constructor-admission.py \
  --proof docs/traces/m9-faa-copy-constructor-admission-20261007.json
```

The new checker hashes ten shipping spans and decodes the head/count writers,
checks implementation and real PCI input hashes, verifies both actual CPU
stores, and rejects seven contradictory scope/count controls. The earlier strict
unwritten-output proof still passes unchanged. Neither checker qualifies
constructor execution, physical retirement or rendered pixels.
