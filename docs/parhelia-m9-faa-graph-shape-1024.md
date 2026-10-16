# M9128 1024×768 FAA graph source boundary

## Current hardware result, 2026-10-08

DRM36073bf/Mesa1468df5 pass32 real glxgears FAA frames at1024×768,
with GPU transforms,32 Unlocks and32 GPU presentations; finalaed0,
queue0, generation0, wedged0. No reference interior pixels are missing,
although shading differences remain. About1.4FPS in the unpaused portion.
[Evidence and screenshots](../tests/m9-faa-gears1024-hardware-20261008/README.md).
Quake3 GL1 FAA remains the next priority; older source-only scope below is
historical, not the current1024 rendering status.


The original M9128 `MxMD.dll` CPU producers accept a second normal sampled
primary at 1024×768. The fresh first-context controller reaches a real
INIT-only queue finish before public draw; a separate run returns one public
diffuse triangle on the same newly constructed graph and then stops before
output. Both independent finish calls stop **before** reading GPU completion.
Neither source desired counter (`b0` for INIT, `120` after the public triangle)
is a hardware receipt. No 320×240 receipt is imported into these runs.

The controller change passes caller width and height through the genuine
surface request and public viewport/triangle. With default 320×240 inputs,
the regenerated frame, operands, resources, clear calls, and finish are
byte-identical to the archived INIT manifest, and its native source blob is
byte-identical to `c17f0e8a…`. The 1024×768 trace is a controlled source
comparison, not an executable native profile.

| Graph role | Requested source shape | Source resource extent | Backing/page capacity | Format |
| --- | ---: | ---: | ---: | ---: |
| Primary | 1024×768 | `301000` | `308000` | `66` |
| Main | 1024×3178 | `c6a080` | `c6b000` | `66` |
| Auxiliary | 1024×198 | `c6080` | `c7000` | `66` |
| Companion | 1024×768 | `601000` | `608000` | `3d` |
| HZ | 128×192 | `19000` | `20000` | `78` |
| Sentinel | 5×1 | `80` | `1000` | `78` |

Independent original `45eb0` layout evidence gives the companion's logical
footprint `600020`. Its resource clear extent is rounded to `601000`, and
its retained backing capacity is `608000`; these three quantities remain
distinct. The genuine selector outputs for the public triangle are primary
`cc220400`, companion `7a220400`, and HZ `f0010400`. They are descriptors,
not addresses. This source shape cannot be admitted by the current fixed
320×240 kernel PREPARE request merely because Mesa can calculate a larger
layout.

The companion page-clear producer writes 193 contiguous payload DWORDs over
1537 logical pages. The clear region grows from `[11d8,1224)` at 320×240 to
`[11d8,14dc)` at 1024×768, adding 696 bytes. Source INIT primary length
therefore grows `4880→5576`; the two later clear-sensitive relocation slots
move `1248→1500` and `12b8→1570`, while the earlier `470` and `1184`
slots stay in place. The native-plan extractor validates the entire 1024
clear payload against the actual `b5e70` instruction arithmetic, then runs
the same vendor instructions at two distinct synthetic page placements.
Both cover 1537 pages in 193 words, with different encoded payloads. The
test never uses a captured GPU base as a native input.

A separate CPU fixture extracts the current kernel C address encoder and
page-clear rebaser, compiles them against the 1024 source-only plan, and
checks two native placements × eight page alignments. It preserves the
original prefix, shifted suffix, other owners and unknown bytes, and tests
corrupt/undersized refusals. The fixture gives its local clear scratch array
193 words to admit the observed source packet. The shipping kernel still
defines `M9_FAA_CLEAR_MAX_WORDS=20`, so this proof identifies a required
native capacity change; it does **not** make the current module accept the
new shape or qualify its GPU behavior.

The genuine public diffuse triangle returns from its CPU callback and adds
88 source primary bytes: `5576→5664`. The extractor sees 30 retained source
owners, 41 address-bearing fixups and 21 timeline stores in that later
source-only snapshot, versus 27/4/13 at INIT. This is the normal public
triangle in a fresh graph frame, **not** the qualified 320×240 red shader
continuation on an already retired INIT. Its source `120` finish remains
unread; no pixel or returned normal Lock is established.

The next Lock source step needs a new-shape retired INIT/DRAW receipt and a
separate normal Lock continuation on the same retained graph and original
caller. The existing Lock controller is correctly tied to the 320×240
hardware epoch and cannot supply those 1024×768 GPU inputs. A future native
implementation also needs live-owner allocation and mapping at these
capacities, dynamic companion clear/primary suffix relocation, descriptor
and AD joins, the private context's complete ownership, and actual new-shape
retirement before a Lock or pixel claim.

Reproduce the source-only trace, profile, and nine refusal controls with the
original M9128 image and recorded device inputs:

```sh
sh tests/m9-faa-graph-shape-1024-20261007/capture.sh /tmp/m9-faa-shape-1024
PYTHONPATH=tools/faaemu:tools/m9emu /home/ai/tmp/faaemu/venv/bin/python \
  tests/m9-faa-graph-shape-1024-20261007/check.py --self-test
M9_FAA_NATIVE_SOURCE=/absolute/path/to/m9_faa_native.c \
  sh tests/m9-faa-graph-shape-1024-20261007/check-native.sh /tmp/m9-faa-shape-1024
```

The committed compact `source-profile.json` contains shape metadata and
hashes, not captured GPU addresses or a command stream for replay. The full
three source manifests can be regenerated by `capture.sh`. The vendor DLL
SHA256 is `b6905b6e…`; source/device hashes are pinned by the checker.
