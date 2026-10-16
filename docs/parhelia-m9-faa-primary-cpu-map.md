# Genuine sampled-primary CPU mapper

The primary's rendering descriptor is not its CPU mapping descriptor.
`tools/m9-vendor-faa-primary-map.py` allocates ordinary and sampled 320x240
primaries through genuine `a3870`, using requests2 and01000002 and actual
M9128 PCI/resource inputs on the conditional allocator fixture. It builds
engine selector9, then invokes genuine resource map `2cb40(resource,out,1)`.
There are no pixel reads, GPU results, completion replies or target accesses.
The map returns real OS-owned guest CPU aliases.

| Primary request | Engine selector9 | CPU mapper selector1 |
| --- | --- | --- |
| `2` | `{GPU|1,cc210140,000100f0}` | `{GPU|1,f0210140,00010100}` |
| `01000002` | `{GPU|1,cc220140,000100f0}` | `{GPU|1,f0220140,00010100}` |

For both real allocations, resource staging flags `2a4` and `2b8` are zero.
The retained mapper at resource`+1f8` invokes `27170 -> 26830 -> 467e0(1)`
on its own layout at mapper`+10`. Its actual source shape is320x240, span320,
allocation height256; source format is66 and layout kind is1/2. Selector1
uses allocation height256 and width at layout`+18c`. Its format path through
`47350` dispatch table `47694/47670` sends66 to `474d2`, which explicitly
replaces the descriptor format with78. Thus the F0 format byte is produced by
vendor code rather than inferred from the rendering descriptor's bit layout.

The actual host-aperture publisher uses those exact selector1 fields:

| CPU store | Field | Ordinary MMIO | Sampled MMIO |
| --- | --- | --- | --- |
| `25d96` | `GPU|1` | `004` | `014` |
| `25dd2` | `f0210140` / `f0220140` | `008` | `018` |
| `25e0e` | `00010100` | `00c` | `01c` |
| `25e73` | inclusive CPU page range | `000` | `010` |

The two actual ranges each cover80 pages, matching the327680-byte backing.
The different slot numbers are allocator choices for these two simultaneously
mapped owners, not a semantic ordinary/sampled slot rule. Descriptor backing
physical addresses match the engine GPU base after clearing its low tag, and
CPU aliases match the genuine map return values. No pixel layout is simulated.

`tools/check-m9-faa-primary-map.py` independently pins the PE dispatch/literal,
actual cached triples, source layout, MMIO instruction/offset/value stores,
returned aliases, and page extent in
`docs/traces/m9-vendor-faa-primary-map-20261006.json`.

A native exact CPU-mapper probe should join `{GPU|1,f0220140,00010100}` to its
own retained allocation and address-translation namespace. Copying engine B1
`cc220140` into an aperture descriptor, dropping the source address selector,
or using height240 is not this vendor map contract. The existing native
CC210140/CC220140 positive CPU roundtrip can demonstrate those views work on
the target; this finite vendor proof does not invalidate that measurement or
prove that changing the CPU descriptor will repair a black GPU result. A
positive initial background and post-retirement image must still be measured.
The caller must preserve the CPU aperture's known safe slot range/priority and
serialize it with other users rather than replaying the vendor slot allocation.
