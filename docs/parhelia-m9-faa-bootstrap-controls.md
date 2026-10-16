# M9128 FAA bootstrap controls: bounded comparison

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0-only

The virtual-primary diagnostic retired the real point `1d0`, while its SYSTEM
output remained unchanged and black. This observation does not identify a
missing hardware enable. Root's read-only snapshot on that same retired boot
`a4a2ad1d` agrees with the actual vendor-produced values for three service
controls:

| Register | Actual MxMD producer | Retired native value |
| --- | --- | --- |
| `f04` | `195e4` read; `195fa` clears bits 28/29; `195ff` store | `0448803e` |
| `3a8` | `25b7d` read; `25b89` clears bit 17, sets bit 16; `25b93` store | `00010a05` |
| `f0c` | `197cc` read; actual configuration getter; `197ee..197fa` merges its value into bits 6..9 | `00200040` |

The native `m9x_address_prepare` uses the same first two expressions. Its
`(old & fffcffff) | 10000` expression is equivalent to the vendor's
`(old & fffdffff) | 10000`: both set bit 16 and clear bit 17. Native
`m9x_address_construct_manager` merges the admitted prefetch value one into
`f0c`, matching this actual producer. This is a post-initialization comparison;
it does not prove register state before native initialization, transient order,
arbitrary configuration settings, or every hardware control.

Check the source pins, actual chronological MMIO events and retained snapshot:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-bootstrap-controls.py \
  --capture-dir /home/ai/src/parhelia/xf86-video-parhelia/captures/m9128-device-inputs-20261006 \
  --retired-mmio /tmp/m9-faa-virtual-stage-20261006/proof/mmio-control.json
```

The read-only snapshot helper is retained by root at
`/tmp/m9-faa-virtual-stage-20261006/mmio-read.c`. It checks device `0540`, opens
BAR resource 2 with `O_RDONLY`, maps with `PROT_READ`, and reads only these
three DWORDs. This checkpoint performs no physical hardware operations.

## The complete bootstrap boundary remains open

The source capture retains actual `FindAdapter` ownership at a GPIO/HMC input
boundary. `tools/m9emu/faa-trace.py` invokes `HwInitialize=56304` only if
`FindAdapter=55ebd` returns zero; that did not happen in this capture.
`m9_runtime_owner.initialize_normal` separately executes real `568e7` to
construct CPU runtime ownership and explicitly records `adapter_admitted=False`.
It does not close `FindAdapter` or establish complete hardware initialization.

Static `HwInitialize` inspection narrows what this gap means. The final call
`56629 -> 586d8` counts eligible adapters against global limits and updates
`75388` and per-adapter records at `75390`; it is an admission-accounting
predicate, not a GPU initialization call. Its registry path
`564d1 -> 5a726 -> 5a745` queries and publishes CPU configuration values.
The global PLL callback `179a2` is separately published at table slot `+88`
by `17ead`. Its actual invocation and cold/warm selection remain unclosed,
as documented in `parhelia-m9-cold-init-re.md`; publication is not invocation.
No new reset or enable write follows from this static evidence.

The actual MD interval does include service maps, both MMU views, direct CPU
root, restore, flush and requester publication. Their register families are
represented in native initialization. Family-level coverage is not proof of
every value or ordering. Native also runs the retained M9148 Linux sequence-0
first-context activation before the M9128 Windows FAA stream; complete
equivalence of inherited context state remains a separate open audit. A
retired sequence-0 fence does not by itself close that semantic boundary.
