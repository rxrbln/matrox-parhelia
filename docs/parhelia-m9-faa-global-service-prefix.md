# Independent M9128 global service initialization audit

The exercised Windows MD global service prefix is already represented by native
initialization. This audit identifies no missing FAA enable or clock write and
proposes no register change. It covers the actual MD interval, not the unresolved
complete Windows FindAdapter/cold/warm transaction.

The source capture is
`captures/m9128-device-inputs-20261006/normal-faa-output-flush-addresses-101f8.json`,
SHA256 `2010ae4ea1e641eac2567a8e4426f33e9c07446faaabc08f14790ea1c425c6ff`.
It retains 625 actual MD MMIO events over 98 offsets. Those counts are an inventory;
only the bounded global prefix below is checked for exact native equivalence.

| Actual event | Original producer | Native source operation |
|---|---|---|
| 0/1, `f04` read/write | `195e4/195fa/195ff`: clear bits 28/29 | `m9x_address_prepare`, aperture-property RMW |
| 2..5, `344/348/34c/350` | `19614..1963a`: zero | four service clears in the same order |
| 6, `354` | `19646`: `03020100` | service map |
| 7..10, `334/338/33c/340` | `19656/62/72/82`: `0/40/80/c0` | four service lanes in the same order |
| 11, `330` | `19692`: `1000` | service base |
| 12, `30c` | `25b71`: zero | service control clear |
| 13/14, `3a8` read/write | `25b7d/89/93`: clear 17, set 16 | service-mode RMW |

The native `fffcffff` mask and vendor `fffdffff` mask yield identical results
after OR `10000`. The checker includes 66 pure integer basis/inverse controls;
these do not serve as MMIO replies. The fixed constants are part of the original
136-byte instruction span at `19614..1969c`, pinned by its hash, rather than
arbitrary register values selected from a capture.

Later actual event 51 writes `30c=1000`; events 54/55 read/update `f0c` for
prefetch. Native `m9x_address_construct_manager` likewise sets service control,
finishes manager activation, then performs the prefetch RMW. This proves the
named ordering and expression; it does not establish all table/cache transients.

There is an alternative `196b7..196e1` branch that writes `f0c=8000` at `196d7`.
It requires derived property `+338` getter bit 3, property `+18` zero and getter
bit 26. That store is absent from the actual 101f8 interval. It is not a sourced
FAA-enable recommendation. The checker pins this entire conditional span.

Other MMIO families in the interval concern service bank rows, owned MMU/window
publication, frontend state and requester dispatch. Their existence does not
turn this bounded comparison into proof of a complete hardware bootstrap.
The earlier cold MC observation agrees on 17/18 direct source-written fields;
`f14` upper readback bits remain unclassified. The original FindAdapter still
has an uncaptured GPIO boundary, and callback publication does not prove PLL
invocation. Those exact limits remain in the cold/miniport documents.

The read-only checker pins five original instruction spans, checks all thirteen
prefix writes, manager-before-prefetch source order and four deliberately broken
native variants. It changes no source graph, register, GPU state or reply.

```sh
<faaemu-python> tools/m9emu/check-faa-global-service-prefix.py \
  --capture captures/m9128-device-inputs-20261006/normal-faa-output-flush-addresses-101f8.json \
  --native-dir <linux>/drivers/gpu/drm/m9x \
  --dll <vendor-MSerX>/MxMD.dll
```

This evidence narrows a global service omission hypothesis. It neither explains
the private FAA black output nor qualifies FAA image correctness.
