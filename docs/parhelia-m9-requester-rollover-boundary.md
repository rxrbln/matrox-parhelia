# Matrox M9 requester rollover and the `0x404` doorbell

This note separates two register interfaces which use related command-buffer
addresses but are not one ring descriptor.  Evidence comes from the saved
Linux `KernelLibraries.o_shipped` and Windows `MxMM.sys`; addresses below are
ELF text offsets or PE virtual addresses.  `[BIN]` denotes direct disassembly
evidence and `[DER]` its narrow consequence.  No hardware was used.

## `0x404` is an endpoint doorbell, not a limit

Linux `m9x109` at `0x2d150..0x2d1bd` translates the command object's CPU start
pointer, stores the resulting GPU address in the primary allocation object at
`+0x50`, clears its low two bits, adds the backing class from resource `+0x34`,
and writes the result to MMIO `0x400` [BIN].  At submission, `m9x1039`
`0x2d7d7..0x2d83f` translates the final CPU producer, stores that endpoint in
the same `+0x50`, and writes it to MMIO `0x404` [BIN].  Thus the two registers
are a tagged start/control value and a final executable address.  There is no
length or upper-bound value written to `0x404` [DER].

The compare immediately before the Linux doorbell is software allocation
bookkeeping, not an address comparison [BIN]:

```
primary = command->allocator->primary       // command +48, allocator +10
primary->submitted_endpoint = translate(command->producer) // primary +50
if (primary->active_nodes <= command->node_allowance)
        MMIO[0x404] = primary->submitted_endpoint;
command->node_allowance = 0;
```

The field names describe only the demonstrated roles. `m9x669` initializes
the primary object's `+0x54` to zero, and allocation methods `m9x110` and
`m9x599` increment it once per node linked into the circular list rooted at
`+0x68`; teardown methods return it to zero.  The command constructors
`m9x654` and `m9x1086` initialize command `+0x58` to zero, and `m9x1039`
clears it after the comparison.  The saved ELF has no direct nonzero
immediate-displacement store to this command field.  Static evidence does not
identify the path, if any, that grants a nonzero node allowance.  A new driver
must not translate this comparison into a hardware ring-size rule [DER].

Windows independently fixes the endpoint meaning without this Linux-only
allocator policy.  `MxMM.sys` `0x224c8` copies CPU start to current producer
and writes object `+0x24` to MMIO `0x400`.  Function `0x224f3` computes
`current_producer - cpu_start + gpu_base` from object `+0x2c/+0x20/+0x24` and
writes exactly that address to `0x404` [BIN].  It performs no access to
`0x500..0x524` and no size comparison.

## The `0x500..0x524` requester rolls independently

The requester descriptor's software-owned words have a different producer
chain [BIN]:

- Linux `m9x759` `0x2b290..0x2b380` writes a tagged transition/base address to
  `0x500`, zero to `0x508`, and the raw live pointer to `0x504`.
- Linux `m9x620` `0x285d0..0x28799` appends either a 16-byte or 32-byte
  descriptor record when required, then publishes the translated software
  producer through `0x504`.
- Linux `m9x1200` `0x2b1c0..0x2b27a` publishes and quiesces, writes the
  two-dword transition record `{2, resource_gpu_base}`, resets the software
  start/producer and end guard to that resource's CPU mapping, republishes
  `0x504`, and quiesces again.
- The Windows equivalents `0x145a5` and `0x146bc` derive `0x500/0x504` from
  their requester object's aligned start, CPU mapping and GPU base.  They do
  not access `0x400/0x404`.

Those formulas also establish the units.  Both drivers subtract CPU byte
pointers and add the result directly to a GPU byte address; neither shifts or
scales the result.  `0x500` and `0x504` therefore contain byte addresses with
four-byte stream alignment, rather than dword indices or byte counts [BIN].

Neither rollover path writes `0x404`, and neither `0x404` submitter reads or
writes the descriptor words.  The only demonstrated relationship is that both
interfaces carry GPU addresses belonging to command transport [DER].  In
particular, `0x50c/0x510` cannot be named as mirrors or limits of `0x404`.

## Field ownership, including first use

The complete literal-MMIO census supports the following names without
inventing the undocumented internal pipeline [BIN]:

| register | software operation | narrow role |
|---:|---|---|
| `0x500` | seeded with a tagged address; thereafter only read or restored | tagged requester progress/base word |
| `0x504` | repeatedly written with the raw translated producer | published producer endpoint |
| `0x508` | initialized to zero; bit 1 is set before save; bit 0 is polled | requester control/status (`bit 0 = quiescent`, `bit 1 = park` [DER]) |
| `0x50c` | polled with `0x500`; otherwise only read or restored | hardware progress companion, exact direction unknown |
| `0x510` | only read or restored | address-like saved companion, exact direction unknown |
| `0x514` | only read or restored with `0x50c/0x510` | opaque companion state |
| `0x520/0x524` | only read or restored, always restored first | opaque pair |

`0x500` is more specific than a static base: after initialization no software
path advances it, while both vendor quiescence loops require two stable samples
of it.  Calling it a hardware consumer pointer would still overstate the
evidence because no saved capture shows its direction.  `0x504`, by contrast,
has an unambiguous software producer and is the ring tail exposed to hardware.

The stable-snapshot rule is independently present in user mode.  `MxMD.dll`
`0x1ac40..0x1ad5e` follows its device object's MMIO mapping and uses the same
sequence as `MxMM.sys`: it samples `0x508`, repeatedly takes consecutive
`0x500/0x50c` pairs until both match, then accepts the snapshot only when a
final `0x508.bit0` is set.  Its fallback path polls only `0x508.bit0` [BIN].
Consequently `0x500` and `0x50c` are the only words statically demonstrated as
transition-sensitive live progress state.  Nothing in either loop compares
them with the software tail at `0x504`, and neither loop consults
`0x510/0x514/0x520/0x524`.  Completion is signaled by `0x508.bit0`, not inferred
from pointer equality [DER].

Windows gives an exact first-use sequence.  Constructor `0x14463` installs the
sole rollover method `0x14540`, clears the saved eight-word image, selects
four-byte stream alignment, and leaves the requester inactive.  `0x14633`
then obtains the per-device requester at device `+0xc238`; on its first use it
allocates exactly one `0x1000`-byte block through `0x14aa9`, whose private
allocation selector is `2`.  Attach helper `0x144d9` maps selector 2 through
the table `{2,3,0,3}`, hence this saved build initializes requester tag
`+0x64` to zero, initializes the embedded software stream, and calls `0x145a5`.
That call orders the first hardware writes as `0x500`, `0x508=0`, `0x504`.
Later acquisitions reuse the page and repeat initialization only when the
per-device requester state at `+0x1a60` is not `2` [BIN].  Selector 2 is an
internal allocation selector; this sequence does not give it a public Linux
memory-type name.

There is no hidden priming record on first use.  The initial `0x500` address is
the aligned-tail formula already documented below and the initial `0x504`
value is the same formula without the tag.  Records enter the page only through
the ordinary software-stream writers.  If a write does not fit, the stream's
sole virtual method invokes `0x14540`, which publishes and quiesces, appends
`{2, allocation_gpu_base}`, resets the stream, publishes the new `0x504`, and
quiesces again [BIN].

The producer/consumer census is closed for the saved binaries.  Windows has
one additional read-only diagnostic path, `0x125fa` (`0x12d4b..0x12ded`),
which copies all eight requester dwords verbatim into a diagnostic record.
It derives no fields.  Linux has only `m9x759`, `m9x620`, `m9x1016`, `m9x399`,
and `m9x714` as literal requester-register users; Windows has only the
initializer/publisher/quiescence/save/restore family plus that diagnostic
snapshot.  The sixteen saved X-server `m9x_drv.so` variants contain no kernel
MMIO producer for this bank and all use the same shipped kernel object [BIN].

The exhaustive direct-register audit remains the static boundary for the five
hardware-owned descriptor words: outside restore, neither saved kernel binary
has a software write to `0x50c`, `0x510`, `0x514`, `0x520`, or `0x524`.
Quiescence samples `0x500` and `0x50c`; save/restore preserves all eight words;
Windows conditionally restores the `50c/514/510` triple when
`(saved_50c ^ saved_510) & ~0xf` is nonzero.  These facts show address-like
tagging and rollover-sensitive state, but do not reveal which word is a fetch
pointer, boundary, or pending transition [DER].

The Windows predicate establishes one additional limit: the upper 28 bits of
`0x50c` and `0x510` are compared as 16-byte-granular address-like values, while
their low nibbles are ignored for the decision.  The low bits may be tags or
control state, but static code does not assign them individual meanings [DER].
Linux uses the stricter full-dword inequality before restoring the same triple.

Restore ordering splits the opaque image into three observable groups [BIN]:

1. `0x520/0x524` are restored first and unconditionally;
2. `0x50c/0x514/0x510` are restored as one conditional triple;
3. `0x500/0x508/0x504` are restored last, with the software producer at
   `0x504` published last.

This order is sufficient to preserve in a replacement driver, but it does not
identify either member of the first pair or the direction of the triple.  The
Parhelia and P-series miniports shipped beside MSerX use a different register
layout in this region, and their diagnostic dumps provide no transferable
requester field names [BIN].

## Smallest capture that resolves the remaining fields

Static analysis has no producer from which the direction or arithmetic of
`0x50c/0x510/0x514/0x520/0x524` can be derived.  Instrument MMIO reads/writes
and force exactly one `m9x1200` requester wrap with a fence after it.  Four
timestamped samples are sufficient:

1. quiescent state before the final pre-wrap descriptor;
2. immediately after its `0x504` publication;
3. immediately after the transition record and post-wrap `0x504` publication;
4. quiescent state after the fence retires.

Each sample must contain `0x400`, the most recent value written to `0x404`, all
eight words `0x500..0x524`, the software CPU start/producer/end-guard pointers,
the backing GPU base/class, and the two dwords at the old-ring transition
record.  Record every `0x504` write and any `0x404` write in order, and record
the first value of each hardware-owned word after initialization and before a
descriptor is published.  A single wrap distinguishes stable limits from
moving fetch/current pointers; the pre-publication and post-fence samples
distinguish software publication from hardware consumption.  No rendering
workload or second wrap is needed unless the first wrap leaves `0x50c` equal to
`0x510`; in that case repeat once with ring bases whose upper 28 address bits
differ.  Merely changing the low nibble cannot exercise the Windows restore
predicate because that predicate masks those bits.

Until that capture, preserve `0x50c..0x524` as opaque saved hardware state and
keep requester rollover separate from construction of the `0x404` endpoint.
