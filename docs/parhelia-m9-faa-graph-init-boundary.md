# Source-owned initialization of six retained M9128 graph owners

The fresh prepared fixture now has a separate **initialization-only**
boundary: source context construction returns, then the caller stops before
public clear, draw or output. A genuine independent `23b80` finishes the
owned queue and stops at `23bc3` before reading GPU completion. It produces
4880 primary bytes and target `b0`; no counter reply or pixels are supplied.
This is a new source profile and is not an existing hardware receipt.

The first 4868 primary bytes are byte-identical to the qualified prepared
`c0` prefix used by the current prepared180 flow. The c0 suffix contains a
public CLEAR42 of the primary. That primary clear is **not initialization
policy** in Mesa's six-owner request, whose primary clear extent is zero.
Using the complete c0 stream as INIT would silently add that clear. This
new prefix keeps the original constructor work and has its own source footer.

## Commands and owners

The source calls genuine `a77c0→ac7e0` to create/enable the graph, associates
the actual surface, and runs the returned public context constructor. It
does not directly splice an AA binder or pretend a draw completed. The
complete prefix also includes first-queue/context setup; it is a bounded
fresh-first-context initializer, not a packet sequence to replay on every
new surface in an already running app context.

| Caller graph role | Plan owner | Mesa logical bytes | Required page capacity | Explicit clear bytes | CPU-known bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Primary | 1 | `50000` | `50000` | 0 | 0 |
| Main | 2 | `136080` | `137000` | 0 | 0 |
| Auxiliary | 3 | `13080` | `14000` | 0 | 0 |
| AA companion | 4 | `96020` | `98000` | `97000` | 0 |
| HZ | 5 | `3000` | `8000` | `3000` | 0 |
| Graph sentinel | 8 | `80` | `1000` | 0 | 20 |

The companion's `97000` clear extent is source resource `+1bc`, separate from
Mesa's `96020` helper footprint. These indices are plan coordinates. Bind each role to the corresponding live
caller BO, at zero view offset. Validate six distinct retained local owners,
source-compatible capacities/layouts and the 320×240/format66 request. Pin,
reserve and retain their references through real retirement and subsequent
draw/resolve; borrowed owners are never freed as private diagnostic allocations.
Use each BO's live GPU base for encodings and local/CPU mappings. Preserve
logical footprints separately from rounded capacities: e.g. main `136080`,
not `137000`, supplies the later source AD extent. No source pointer/PFN/base
is a native address.

The sentinel initializer is exactly five source-written DWORDs:
`a4a3a1a1,28041968,0,0,0`. Its remaining allocation is opaque. Mesa already
writes those five words; native INIT can verify them or perform the same
bounded known upload. It must not clear the rest. Main/aux receive no explicit
clear request; this does not assert that all engine initialization effects
are absent from those owners.

The actual explicit GPU clear inventory remains:

| Target owner | Logical extent | Source route |
| ---: | ---: | --- |
| Graph HZ 5 | `3000` | `b4fe0→b5030` |
| Private helper HZ 7 | `3000` | `b4fe0→b5030` |
| Graph companion 4 | `97000` | `b4fe0→b5e70` |
| Private context helper 10 | `800` | `b4fe0→b5030` |
| Private context helper 11 | `20080` | `b4fe0→b5030` |

Four actual nonnull `24930/tag1` operands are in primary offsets
`470` (owner5), `1184` (owner7), `1248` (owner10), and `12b8` (owner11).
The companion page clear is outside the relocation callback: header at
`11d4`, payload `[11d8,1224)`, 151 logical pages. Regenerate its page runs,
masks and word count from the **live companion base**, then move the already
patched suffix and adjust primary length. The 4880-byte source footer is
`4545e3e6,1,b1` at `[1304,1310)`; its b1 is a source timeline value, rebased
from the native timeline base rather than used as a completion reply.
Thirteen exact timeline stores are recorded by the plan. There is no
counted secondary stream or direct secondary endpoint in this INIT.

## Private context state cannot be discarded

The six caller BOs are only the surface graph. Retain the other source-owned
context resources as private state. In particular:

- Renderer pool owner22 is profile56, requested320×1, logical `580`, page
  capacity1000. It has **zero CPU-known bytes**. The same resource/backing
  identity becomes the later prepared180 kind2b pool operand; the plan
  retains those exact future consumer joins. Never initialize its opaque
  bytes from an archived GPU snapshot.
- Context sentinel owner21 has its own known20-byte initializer and backing.
  It is distinct from the borrowed graph sentinel owner8; do not alias them.
- The private helper HZ/context helper targets above keep their own backing
  and clear commands. Known RS/metadata images and first-queue command/common
  owners remain private; unknown resource tails stay unknown.
- Earlier conversion startup retains unknown d0 owner27, logical/capacity
  `80000`, before the first post. Its genuine descriptor/mapping is captured
  from `5b3c0→5b750`. The two contiguous d4/d8 OS allocations have no GPU
  consumer in this prefix; their future consumer admission stays separate.

The full plan therefore has 28 retained owners, not six. Its source blob
contains122288 known bytes: graph sentinel20, context sentinel20, RS115160,
metadata128+32, two1024-byte known images, and primary4880. No vertex/fragment
program, constants, geometry, new output or GPU-produced pool bytes are
invented. The current graph allocation/lifetime layer must retain an opaque
kernel context token identifying this private state if INIT becomes an ioctl;
a fence alone cannot supply the missing pool/context ownership to app draws.

## Separate bounded INIT receipt

A native INIT operation can borrow those six owners, allocate/retain the
proved private state, patch the four live operands and all timeline fields,
regenerate the companion clear and submit the complete new prefix. The
existing mapped virtual-primary/common-counter contracts and actual source
startup mapping remain prerequisites. Return an INIT receipt only after real
retirement of its derived target, tied to the same six BOs, driver generation
and private context token. Failures after posting must retain/quarantine the
live owners until safe reset/teardown. Never set Mesa `graph.initialized`
from allocation success, CPU vendor return, guessed counter or an older c0
receipt. An INIT receipt proves initialization completion, not rendered pixels
or arbitrary app state support.

No target test was run for this new profile. The signed source producer and
strict checker regenerate the plan/header/blob exactly. Eleven refusal controls
cover aliased/undersized caller roles, added primary clear, opaque pool/tail
uploads, sentinel aliasing, fabricated receipt and wrong frontier. Actual
native encoder/page-clear C passes two placements×eight alignments with
prefix/suffix, other-owner and unknown-byte preservation. These host checks
qualify the bounded initializer structure, not general context reuse or a
physical INIT receipt.

Artifacts: `captures/m9-faa-graph-init-20261007/` (source, manifest, addresses,
startup, native plan/blob/header). Reproduce with
`tests/m9-faa-graph-init-source-20261007/capture.sh`, then the established
Unicorn/pefile interpreter and `tests/m9-faa-graph-init-source-20261007/check.py`.
The plan generator is `tools/m9-faa-graph-init-plan.py`.
