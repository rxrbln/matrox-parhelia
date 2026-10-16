<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Normal Lock after the original write wait

The observed fresh normal Lock `4e1d1` deadline180 belongs to the **staging
mapper**, not an observed d4/d8 conversion. Its next source contract is the
generic write-wait return and CPU mapping of that staging resource. F2 DMA
conversion remains a distinct conditional path. This checkpoint disassembles
shipping vendor instructions and reads existing source evidence; it executes
no vendor caller, supplies no completion or opaque GPU bytes, and makes no
rendered-image claim.

## Immediate continuation

The existing source report
`/tmp/m9-faa-prepared-lock-resume-20261007/report.json`, SHA256
`76717491e26394b358fddfe89fe15a7fb5d0a8cd55c22ae8de7ab6fbfff7b42b`,
records actual `2cb40` arguments `[staging,0,1]` with return address `559ca`.
It stops before `4e1d1` reads the common counter at offset0 for180/high0.
Its normal Lock has not returned; both actual direct-conversion operand
collections are empty. The dynamic capture/export is owned separately; this
static checkpoint does not resume it or substitute a new receipt.

The earlier sealed source manifest at
`captures/m9-faa-prepared-startup-20261007/normal-faa-prepared-normal-lock-wait-manifest.json`
has SHA256
`95da33667d7b04463f4a739a3fd75f51b3801196e6836e5acded8ab504ad8836`.
Its actual staging object and primary are distinct CPU resource objects but
share backing and logical GPU base, each with extent50000 and allocation
kind0. This is an owner join, not proof of linear pixels, a missing copy or a
bug. Neither captured object pointers nor their addresses are native inputs.

The shipping PE establishes the following sequence:

| Source | Contract |
| --- | --- |
| `5598c..559c5` | With primary `+3d0` set, call `57d90(&primary->3c8)`, require success/non-null staging, then call `2cb40(staging,out,1)` |
| Resource vtable `1f3258`, slot8 | Actual write-wait method is `55550`; `55596` calls `4e130` |
| `4e1d1/4e203` | Read common counter, mask finish low bits, extend its observed epoch and compare with the resource's real last-use deadline; outstanding use returns1, retired use returns0 |
| `2cb90..2cba8` | Retry the write-wait method while it reports outstanding use, then publish frontend314=1 |
| `2cc33..2cc56` | Dispatch using the actual resource kind; kind0 table entry is `2cc45`, loading its retained mapper `+1f8` and calling `27170` |
| `27175..27190` | If mapper `+1b4` is absent, call genuine `26830` to acquire/publish a view; a missing or failed map is not an alias |
| `271be..271cd` | Return mapper descriptor `+1c` plus mapper offset `+1e0`; an optional separate output receives descriptor `+18` plus that same offset |
| `559ca..55a35` | Preserve mapping result, apply only the guarded existing resource offset, and record successful Lock ownership |
| `a8dc1..a8dd1` | Return through `55850` and publish its actual alias through the caller's output slot |

The optional `2cbc8..2cc33` publication branch depends on live mapper flags
and must be observed, not chosen from an old capture. The kind0 path differs
from the direct-DMA kind2 table entry `2ccb4`, which returns allocation `+1c`.
Acquiring a formatted CPU view does not establish that raw BAR storage is a
linear pixel buffer. The actual later view descriptor, PTE/domain, allocation
owner and returned offset must be joined before consuming any pixels.

## Conditional d4/d8 conversion

`afbe0` dispatch depends on the real request and resource/profile state.
`afc79` can first try another helper; only its later applicable request38=1
branch calls `b1290` at `afc91`. This helper captures PDEV d4/d8 before its
optional staging preparation. Before emitting conversion it requires both
wrappers' `+234` readiness and the source format/descriptor conditions at
`b1375..b13a8`. A constructed wrapper alone does not prove admission.

Once genuinely admitted, `b17d4` emits `45fcf2b6`, `b1813` reads selector8's
address and `b1817` adds the selected backing's live425c ring offset;
`b1826` writes that actual F2 destination. Current iteration parity selects
d4/d8, and `b18d7..b18f0` waits/maps the previous buffer through86bc0 before
source CPU conversion. The independent full owner/ring/row contract remains
in `docs/parhelia-m9-faa-copy-dma-contract.md`. The observed mapper180 prefix
has no such consumed operands; they must not be manufactured for a native
profile merely because earlier startup allocated them.

The observed FC at `f8b1e` is a separate context `+5b8/+c4/+34` state word.
Its shipping producer/consumer load/store is pinned here. The source report
records `00f00140`; Inputs' independent producer audit identifies the packed
320x240 extent. It is not an address to relocate by numeric resemblance.

## Bounded observation plan

1. Qualify the newly emitted complete prefix and its genuine retirement.
   Resume only the original180 stack with that same-profile real receipt;
   do not force a return or borrow an earlier completion.
2. Passively retain the actual `2cba8`, mapper-kind dispatch, `27170/26830`
   owner/publication operations, exact CPU view triple/PTE selection, and the
   returned alias/offset. Stop before any unknown view or pixel read.
3. If normal Lock returns an alias without reading pixels, retain that return
   separately. A successful call or retired GPU prefix is not pixel evidence.
   Any later read requires exact-owned raw bytes plus this view's actual
   addressing contract; unchanged raw BAR hashes alone cannot supply it.
4. Only if real `b1813/b1817/b1826` consumers occur, derive live DMA owners,
   typed F2 destination/addend, ring bounds and wait channel. The retired
   coherent-DMA snapshot API can then export exact owned bytes. Feed them to
   the original guarded source CPU converter only after its own real wait,
   rather than decoding opaque records or synthesizing conversion results.

## Check

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-lock-write-wait-follow.py \
  --proof docs/traces/m9-faa-lock-write-wait-static-20261007.json
```

The checker pins the whole shipping MxMD image,14 exact instruction spans,
two dispatch tables and the responsible instructions. Four contradictory
scope controls reject invented GPU reply, pixels, Lock completion and layout
rewrite. Its optional `--report PATH --manifest PATH` reads the dynamic owner
join described above without executing or mutating source. Existing hashed
baseline modules, receipts and capture files remain unchanged. No hardware
was accessed for this checkpoint.
