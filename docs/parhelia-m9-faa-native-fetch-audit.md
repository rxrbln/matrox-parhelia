# Bounded normal FAA replay fetch audit

`tools/check-m9-faa-native-fetch.py` audits the retained normal capture without
GPU execution. It checks the actual counted secondary ranges, preserves every
counted group and uncounted draw launch, joins all timeline operand stores, and
compares public vertices to the bytes actually copied into system DMA. Run it
against `captures/m9128-device-inputs-20261006/normal-faa-input-bytes.json`.
`--plan` additionally rejects a generated replay plan omitting any timeline
operand; the required inventory is 26, not the original 22.

The two direct primary calls fetch secondary intervals `[0,25c)` and
`[25c,560)` from the actual `1042000` system-DMA owner. A count N denotes the
following header and N DWORDs; a counted group can contain multiple ordinary
headers. The `23000001` launch at the end of each interval is an uncounted
three-DWORD record. Every byte in those two intervals is CPU-known. The launches
have numeric ranges `[0,4)` for preprocessing and `[0,3)` for rasterization.

Twenty actual `2557b` completion stores publish `21,31,...,151`; the two
`25338` waits publish `f0` and `130`. Four more timeline operands occur outside
those observers:

| Primary owner offset | Actual CPU store | Value |
| --- | --- | --- |
| `124` | `f5898` | `30` |
| `4f0` | `f5898` | `50` |
| `fbc` | `f5b30` | `50` |
| `1510` | `f5898` | `140` |

The shader upload producers emit `4545e3e6 / 04000001 / token`. Their calls
at `f5893` and `f5b2b` use queue vtable slot `70`. Actual queue constructor
`22e3c` installs vtable `1f2c60`, whose slot `70` points to `25860`:

```text
25860: mov eax,[ecx+2c]
25863: mov ecx,[eax+4]
25866: mov eax,[ecx+38]
25869: mov edx,[ecx+3c]
2586c: ret
```

This returns the actual platform timeline low/high, the same source read by
completion production. The four low-tag-zero fields need the same timeline
translation as the other tokens. Keeping their captured absolute values would
break replay after an earlier diagnostic advanced the native timeline.

The generic register decoder can consume the primary image as 382 lexical
packets, but that does not validate its inline shader upload grammar. In
particular it misses completion fields at `122c` and `1240`. This audit uses
actual CPU store provenance for counter coverage rather than treating that
lexical parse as an executable fetch theorem.

The public vertex input is byte-identical to the final DMA vertex suffix:
`(80,180),(240,180),(160,60)`, XYZRHW with green diffuse. The geometry base
`10c4080` has a 128-byte viewport header before the three 20-byte vertices at
`10c4100`. Little-endian bytes `0000a042` encode float80, not float50. The
triangle area is9600 pixels before raster rules and fractional edge coverage.

The primary startup binds HZ `10b9000`, second HZ `1111000`, context scratch
`1131000`, and sentinel `1119000` as clear targets. It never binds sampled
primary `e81000` until the raster and resolve stages. A first-image diagnostic
must therefore initialize its externally owned primary through the sampled
layout2 surface path and order those writes before submission. An ordinary
linear surface or all-green-edge triangle oracle does not describe this frame.
The final resolve is exactly `45fcf3ec / 0 / 0 / 00f00140` (320x240).

The known 240-byte program has the compiler endpoint literal
`ffff801c/a01fc00f` at slots2,3,29, and actual shader entry slots0 and4.
Those literals do not prove hardware END, cache-line prefetch, or read bounds.
The 115160-byte database and 240-byte program have unspecified allocation
suffixes, which must remain unknown. Every full allocation is privately owned,
so a speculative fetch within that allocation remains contained, but useful
initial state and exact hardware fetch semantics are separate questions.

Counter activation joins `1068` to common DMA+20|2, `106c` to common DMA+0|2,
and `1070` to common DMA+40|2. The captured default queue restore descriptor
uses tag1 at `10ec`, unlike the older native local-mode3 tag0 arm. This needs a
native domain join; it must not be silently equated with FAA pool6. The pool
save precedes sentinel restore and resolve in the actual CPU-produced stream,
but its GPU save footprint and subsequent pool fetch contents remain unproved.
No unknown pool or allocation suffix is filled with an invented zero image.
