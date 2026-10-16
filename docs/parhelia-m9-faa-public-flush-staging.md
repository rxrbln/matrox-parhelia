<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Public FAA dirty flush and sampled-primary staging

Static shipping MSerX `MxMD.dll` source establishes two distinct destination
contracts. The public dirty-resource refresh `a7390 → a7440 → aa750` binds the
existing sampled primary. The internal `abc20` helper binds its explicit first
argument, which the successful `ac1f0` allocation branch supplies as a separate
ordinary output. Both use compiler mode9; destination ownership and AA policy
are not interchangeable. This proof executes no vendor caller and supplies no
GPU inputs, completion, pixels or successful public read return.

## Destination and publication

At `a7466..a7497`, `a7440` takes the resource's outer owner at `+420`, adds8
and passes that array to `aa750`. Its established roles are primary, companion,
main and sentinel. `aa750` joins:

| Owner | Producer | Published shadow |
| --- | --- | --- |
| Array0 sampled primary | `aaba6..aac08`, selector9 cached at resource `+3c+74/+78` | AA `+38/+58` |
| Array1 companion | `aab1c..aab84`, selector13 | AA `+68/+70` |
| Array2 main | `aa95b..aa9ca`, selector14 | AA `+80/+88` |
| Main's independent `+414` auxiliary | `aa9ca..aaa2d`, selector14 | AA `+94` |
| Optional companion `+38c` HZ | `aaa7d..aab1c`, selector12 | AA `+74/+7c` |
| Array3 sentinel | `aa809..aa82d`, actual resource getter5 | Fragment `+74/+78` |

The destination lookup may call `467e0` to populate the cached selector9 view.
This copies derived view bytes into command shadows; it is not an in-place
conversion of the primary's underlying layout. `abc20` performs the analogous
selector9 lookup on argument1 at `abf4e..abf9f` instead.

`aab86..aaba3` computes AD from the main's retained backing logical extent,
divided by32 and merged into21 bits. `aacad..aacea` establishes AA `+14=1f`
and `+20=0`; `abc20` establishes `1d/ffff` at `abdcd..abe02`. The public path
sets context `+10=9` at `aad70` and calls the actual renderer through slot0c at
`aae05`. Its subsequent calls at `aae3b`, `aae4e` and `aae66` publish usage
for the main, companion and primary backing resources. Their timestamp is
derived from live queue state with an added0x10; it is not a readback of GPU
completion.

After `a7440` returns, `a7390` calls `ac150` at `a7405`. That helper clears the
three resources' outer dirty `+18` flags (`ac160/ac171/ac181`), and `a7416`
clears the caller's outer flag. These writes record publication/ownership
bookkeeping. They are not a retirement wait, a successful CPU copy or rendered
pixel evidence.

The early shortcut at `aa857..aa92e` has additional whole-shape/profile and
rectangle `+18` predicates before its copy/reset. The `a7440` caller sets its
rectangle `+18` to zero (`a7459/a747d`), so the test at `aa875` excludes this
shortcut for that caller. The later copy/reset at `aae68..aaf05` is separately
gated by whole-shape and nested dirty/ownership state. Neither is an
unconditional missing reset inferred from their absence in another trace.

## Staging fields and ordinary Lock

The resource setup and access instructions distinguish these fields:

| Field | Source setup | Access consumer |
| --- | --- | --- |
| Resource `+3d0` | Initial resource setup writes it at `50e07`; a later branch for internal storage selector `+234=2/3` sets1 at `52ead` | `55850` tests it at `5598c` |
| Resource `+3c8` | Initial setup writes it at `50e01`; `52eb7..52ebe` passes its address to `57b20`, whose actual allocation result is stored through the supplied slot at `57b52` | `55995` passes its address to `57d90`; `559a7` loads the resulting staging resource |

`57b20` creates a separate resource, copies source geometry/profile fields and
sets the new internal storage selector to1 (`57be6..57beb`). This pins the
producer's staging ownership; it does not assert which setup branch a future
caller took, invent the returned owner, or establish a universal allocation
domain solely from that selector.

When `+3d0` is set, ordinary Lock `55850` calls
`57d90(&primary->staging)` at `5599e`. The copy helper includes the normal
`48c60` dispatch at `58042`. After a successful copy and a non-null staging
owner, `559b4..559c5` maps that staging resource through `2cb40`. If `+3d0`
is clear, `559c4` instead supplies the original resource to the same mapper.
An unsuccessful copy or absent staging owner does not admit that mapping.
The existing public wrapper route to this consumer is
`a8a60 → a66c0 → a8d60 → 55850`; caller admission and real post-wait return
remain separate dynamic proof obligations.

The audited direct stores in `aa750` do not overwrite `+3d0/+3c8` or rewrite
the raw primary layout. This bounded claim is not an exhaustive proof of every
indirect helper's side effects. The source distinction nevertheless rules out
treating a dirty-flag clear, destination publication or raw BAR copy as the
ordinary staging-copy/mapper contract. CPU decoding must follow the actual
returned layout; the source does not make tiled memory linear by naming it
an output.

## Reproduce the bounded proof

Shipping image SHA256:
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-public-flush-staging.py \
  --image /srv/t2/xf86-video-parhelia/xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll \
  --proof docs/traces/m9-aa750-public-lock-static-20261007.json
```

The checker verifies21 instruction spans against the whole image hash, checks
the particular instructions implementing the destination/state/staging joins,
and pins itself by SHA256 in the retained result. Five contradictory result
controls reject a substituted output owner, substituted AA policy, absent
staging owner, invented completion or invented raw-layout rewrite. No product
code is changed. This is source-path evidence, not performance or rendering
qualification; the real public flush capture and subsequent retired staging
read are owned by the separate lifecycle investigation.
