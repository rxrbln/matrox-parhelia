# Actual unfinished ordinary FAA output inputs

The signed `normal-faa-output-input-manifest-101f8.json` checkpoint replaces no
baseline template. It follows the real internal `ac1f0` read request against
the retained outer FAA owner triple. It is an explicit diagnostic caller below
the public copy admission gate. The original call executes real allocation,
`abc20` resolve dispatch, `ac0e3/ac139` lifetime bookkeeping and `afbe0` CPU-copy
entry. A passive pre-read observer stops before the first actual live common
buffer load; it supplies neither that load's value nor pixels or completion.

The accompanying source and contract JSON files retain actual vendor
instruction pins, input hashes, MDL ownership/PFNs, prior metadata writers,
CPU-write chronology and the copy/address-builder contract. The producer is
`m9_faa_output_manifest.py`, wrapping the render agent's
`tools/m9-vendor-faa-read-contract.py`. Pass the latter's actual-input arguments
plus `--contract-driver`, `--manifest-out`, `--mm-flags 0x101f8` and
`--continue-to-gpu-input`. The independent checker is:

```
tools/m9emu/check-faa-output-manifest.py MANIFEST --source-capture SOURCE --contract CONTRACT
```

There are 90 actual typed relocation stores, 35 live raw NT MDLs, 34 mapped raw
prefixes and one separately retained counter PFN proxy. Actual guest CPU
writes establish 7268 bytes: primary 5584, secondary 1376, constants 120 and
geometry 188. These are the unfinished output stream's actual bytes, not the
old 92-operand / 5664-byte primary. The unchanged source programs comprise 240
bytes / 30 slots, SHA256
`2efd8e0bb9c16ca8bdc67ddec0990bf995b7ef329d09ae8741b6a76324cb66ec`;
the database comprises 115160 bytes, SHA256
`af36245806515bd7b0e38a986586d33611a84ed76dae845a42ce8776ba89f174`.
Existing CPU maps, exact PTEs and sparse write validity establish those source
bytes. Unwritten tails and all GPU-produced inputs remain unknown.

The genuine temporary resource has allocation kind 1, profile class 2,
logical format 6 and hardware-format field **100 decimal / 0x64**. The actual
layout is 320 by 256, with a 320 by 240 read rectangle and extent 327680. The
actual destination selector is `[GPUVA|1,c8210140,100f0]`. Its 80-page mapped
MDL prefix uses descriptor flags `30`, compact PTE nibble `b`, and produced
companion metadata `[0,80,0]`. No destination pixel byte is source-known. The
native producer must derive the selector, dimensions, extent, allocation and
mapping from its own retained typed resource; these historical values are
bounded diagnostic observations, not portable addresses or parameters.

`system_owner_mapping_policies` records every observed mapped owner. Queue,
geometry and the temporary output use GPUvirtual addresses, tag 1, flags `30`
and PTE nibble `b`. The separate counter proxy uses flags `110`, nibble `a`
and only the actual 159-byte common-buffer subspan has raw backing ownership.
Replacing a GPUvirtual address with physical DMA while retaining tag 1 is
unsupported. The original baseline's tag-2 physical queue/geometry domain
cannot be transplanted into this fresh stream. The capture selects no native
owner or replay domain and invents no contiguous alias.

The original copy reaches `4e2fc`, `mov ecx,[ecx]`, reading common-buffer offset
`20`. The observer records the actual owner, virtual/physical offset,
registers and instruction before executing the read. No `55550`, `4e130` or
`23bc3` event precedes this boundary in the retained copy trace. The exported
primary remains 5584 bytes, and no final retirement capture or successful
return is supplied. A separate explicit queue-finish caller may establish a
private submission boundary, but would not establish the original copy's
return or the public API contract. All owners remain live; the temporary's
final release has not executed.

The public admission gate is separately pinned by
`normal-faa-output-admission-source.json` and
`check-faa-output-admission.py CAPTURE --normal-manifest MANIFEST`. `51e0c`
sets resource `+3fc` only after profile class 4, profile formats `31/32` and
request bit `20000000` are admitted. It calls `ac6a0` on embedded owner
`+3e8`. `a74b0` additionally requires operation `+28` and this `+3fc` flag
before calling `ac1f0` on that embedded owner. The retained normal primary's
profile class is 2 and profile logical-format field is 0; its outer `+420`
FAA graph is a distinct object. The static public callers `92970/92c09` and
`94140/94413` establish operation-field producers, not their runtime admission.
Neither `+3fc` nor any other internal state is changed to admit this case.

A distinct actual registry `101b8` internal-output control returned null from
the ordinary allocation at `ac2a0`. Its failure branch subsequently entered a
64-by-64 helper cleanup resolve and reached a separate missing common-buffer
input at `252b9`. The strict observer supplied no value. No ordinary output
owner, address domain or original API return is proved by that control; its
failure cannot justify mixing baseline addresses into the successful `101f8`
capture. The existing complete normal `101b8` input exporter was regenerated
after factoring its shared read-only observers: the resulting JSON object is
exactly equal to the original retained baseline.

## Independent actual queue flush and fresh direct fields

`normal-faa-output-flush-input-manifest-101f8.json` additionally retains an
independent explicit call to the actual `23b80` queue-finishing method on this
unfinished output stream. Its guarded source helper requires the genuine
triangle, allocated live ordinary temporary, executed `abc20/ac139`, original
missing-input boundary and actual queue `+94=1`. It does not resume `ac1f0` or
answer its completion input. The unchanged actual finish core appends twelve
primary bytes, posts the actual buffer endpoint and stops at `23bc3` before
executing its status read. The primary is 5596 bytes, total known MDL bytes
7280, and the desired source counter is **140**, not the old 150. The
temporary remains retained and neither copy nor retirement returns.

Reproduce with the unfinished exporter arguments plus
`--independent-flush-helper PATH --address-audit-out PATH`. The helper is the
render agent's source-guarded `m9_faa_retirement.py`; its SHA256 is retained.
The checker accepts the additional `--address-audit ADDRESSES` argument. Its
source, contract and address audit are separately hashed. The original
common-read boundary and the independent final status boundary are both
preserved, with no GPU reply or pixel data.

The actual fresh command inventory has 47 nonnull typed fields plus four
direct secondary endpoints: **51 address sites**, and **25** timeline fields.
The direct primary offsets are `1348/134c/1354/1358`. The two secondary
intervals cover 604 and 772 bytes. Their starts use the GPUvirtual selector 1;
their ends carry marker 2 from the actual saved-kind expression while retaining
the same mapped buffer/translation. An endpoint marker must not be treated as
proof of a physical DMA domain. All fields are joined to their actual producer
PC and unique actual CPU backing write, including the shader-loader timeline
getters that a generic packet walker misses. Native alignment changes still
require regenerating lengths and shifting every remaining field.

The actual `106c` activation takes the mapped-counter branch. Source
`23a47..23a97` examines queue `+160`, verifies that the physical counter lies
inside its backing page, and at `23a8d` produces
`[resource+3c+8] + (physical_counter - [resource+24])`. This maps the fresh
counter to its retained GPUvirtual PFN proxy and tag 1. The physical `|2`
expression at `23a94` is only the fallback. Registers `1068/1070` still use
their actual separate physical common-buffer fields `|2`. The native producer
must derive these distinct bindings from the live counter owner and selected
mapping, preserving its policy `110` / PTE nibble `a`; queue policy `30` /
nibble `b` does not admit a universal counter substitution.

The same flush run observes `b5f86` reading the actual caller/device `+1c=0`
and `b6028` using retained bit-0 padding flag zero. It records the companion's
actual GPU base and logical clear extent 151 pages, together with the exact
`b60ab` header and `b6101/b6143/b61ae` write inventory. The captured prefix has
nineteen page-run words plus its header. This remains a controlled device ABI
observation, not a universal hardware capability. The native clear must derive
every run, mask, count and suffix offset from its live companion placement;
the render agent's alignment-varied helper proof supplies that separate check.
