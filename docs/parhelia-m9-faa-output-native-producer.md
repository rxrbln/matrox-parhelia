# Private ordinary FAA output producer

The new producer uses the actual retained `101f8` internal `ac1f0` output
request and its independent final-queue flush. The original CPU-copy call
remains stopped at the first missing GPU input (`4e2fc`). A separate real
`23b80` request produces its own completion/footer and post, stopping before
the `23bc3` GPU status load. No completion is supplied to either caller, and
this does not close public `a74b0` admission for the sampled primary.

The signed source is `normal-faa-output-flush-{input-manifest,source,contract,
addresses}-101f8.json`. It emits 5,584 primary bytes before the independent
flush and 5,596 after its genuine 12-byte footer. Its final timeline is
`140`, with 51 address sites and 25 timeline sites. The old sampled-resolve
template's 5,676 bytes, 53/26 sites and `150` final timeline do not describe
this operation.

`tools/m9-native-faa-output-template.py` first checks the whole fresh source
manifest, then coalesces actual resource backing aliases and joins each
command/geometry slot to its live MDL mapping. It derives 31 native owners
and 124,928 source-known input bytes. The ordinary kind6 output is a separate
80-page SYSTEM owner; its pixels stay entirely absent from the firmware.
Each actual flags30 SYSTEM owner has its own live class1 mapping and B PTE
grammar, with known `[0, mapped_pages, 0]` companion metadata. Mapped prefixes
bound every GPU operand; unused host-allocation tails are not copied or
promoted to inputs. Direct secondary starts use the actual selector1, while
their counted end fields preserve the independently produced tag2 encoding.

The producer locates the companion clear by actual `b60ab/b6101/b6143/b61ae`
CPU writes. Its same-run `b6028` flag is zero, its real resource extent is
151 pages, and the source words are compared against the actual isolated
vendor instructions. Native alignment regenerates page groups and counts;
the already relocated suffix moves once and the submitted length follows
the resulting packet size.

The native primary submission now follows the actual source class1/B owner
through its retained mapped GPUVA. The private helper validates consumed page
leaves, publishes coherent command writes and derives400=base|1 and404 from
the rebuilt stream length. It uses the existing real synchronous completion
wait/extension. Persistent private requester ownership rejects later ordinary
arm/rearm/submission instead of assuming the physical slot is still selected.
The owner and mapping survive completion or timeout through DMA teardown.
Source and actual-helper proof live in kernelm9x/tests/faa-virtual-primary.md.

The source `106c` producer is a mapped flags110/A counter proxy. Native code
therefore retains a one-page class1 alias of the transport's existing
completion DMA page, and publishes that alias with tag1. It allocates no
independent counter and never frees the transport's shared page. Source
`1068/1070` retain their separately observed physical-tag2 fields at offsets
`20/40`. The ordinary diagnostic initializer uses the real previously observed
retirement as its seed. A separate default-off fresh source-context admission
preserves the CPU-initialized seed10 without claiming prior GPU retirement.
Both paths derive timelines from their admitted live transport state, and
the primary submit helper preserves the actual counter publication.

After real native retirement, explicit `PARHELIA_M9_ACCESS_FAA_OUTPUT` READ
accepts only the exact original color GEM object, matching transport
generation, and the bounded `c8210140/100f0` 320x240 output contract. It reads
the retained output DMA alias through the actual `8b020 -> 8c4a0` address
calculation. Hardware format64 is A8B8G8R8, so user rows are RGBA. This is
separate from the sampled F022 host-aperture roundtrip. The image oracle
preserves that 76,800-pixel positive control, then explicitly reads the
ordinary output and writes its RGBA pixels to PPM.

The native fixture `m9x/tests/faa-output-template-cpu.py` compiles the actual
encoder and variable-clear helpers with the generated ownership tables. It
checks all 51 address sites and 25 timeline sites at two distinct native
placements, compares page clears with vendor instructions at all eight page
alignments, checks footer/length and complete prefix/suffix/secondary/geometry
preservation, leaves unknown output bytes untouched, and rejects damaged
source clear words and undersized owners before mutation. The source
checker, reader/companion fixtures and complete kernel module build pass.
The first physical qualification of kernel `df1f49e` reaches actual native
retirement `0x1d0` but fails: all76800 visible ordinary output pixels are
black. Main/AA fragment hashes are unchanged from the earlier failed runs.
See [the hardware evidence](../tests/m9-faa-ordinary-output-20261006/README.md).
Subsequent SYSTEM mapped-prefix observations, virtual-primary fetch and
verified WC CPU mapping have been physically qualified below, with images
still failing.
Application FAA remains disabled.

Kernel `03c30b3` also retires the source-faithful virtual primary, but all
visible and padded output CPU-alias bytes remain zero and unchanged. The
source-required write-event channel reaches its native rebased target.
[Physical qualification still fails](../tests/m9-faa-virtual-primary-20261006/README.md);
no extra cache operation or hardware-enable value is justified without source
proof. Application FAA remains disabled.

The subsequent verified WC mapping test (`20df96d`, boot9666bcd7) also
retires1d0 with zero ordinary output. All five source-covered SYSTEM owners
have live WC attributes before publication. Source cache fidelity is closed
for this run, [but rendered pixels still fail](../tests/m9-faa-wc-20261006/README.md).
The source-context experiment8664d0c separately omits the extra Linux
context-initialization predecessor and retires at140, but likewise produces
all black pixels and unchanged main/AA stores.
See [the actual source-context test](../tests/m9-faa-source-init-20261006/README.md).
Full vendor cold initialization equivalence and actual downstream resolve
dependencies remain under source audit; no application FAA success is claimed.
