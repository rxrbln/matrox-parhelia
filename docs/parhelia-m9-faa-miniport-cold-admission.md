# M9128 miniport callback publication and cold admission boundary

The source prefix does not establish a completed Windows cold initialization.
The actual-input `FindAdapter` stops without reaching construction of the
callback table containing PLL helper `179a2`. A separate examined table-publication request is
software owner bookkeeping and does not invoke that helper. No native reset or
clock-programming change follows from this evidence.

This checkpoint uses only the MSerX `MxMM.sys` SHA256
`bf743961b4846e90f32cbdf868d69240d7c403fcbd0a8d9cd6d8783ef9bd62c6`
and `MxMD.dll` SHA256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
Addresses below identify instructions in their named image, not native addresses.

## The table and the tempting owner-publication route

`FindAdapter` calls `123b2` at MM `56201`: returned `-1` enters failure; returned
`1` takes the successful existing-device branch without new construction;
other returned values call new-device helper `568e7` at `562ab`. The passive
prefix does not reach that completed admission result. At `56961/56967`
that helper calls constructor `17dc4` with `ECX=miniport+c444`. Constructor
`17ead` publishes PLL callback `179a2` at table `+88`. At `56974..56989` the
helper stores the table pointer into wrapper `miniport+c510`, then publishes
that wrapper as `miniport+c440`.

MD `1a1c0` tests actual hardware owner fields `+1a8c/+1a90` at `1a1ff/1a207`.
Its `1a21e..1a234` request is `233054`, with four input DWORDs `[0,0,1,0]`.
MM dispatcher `71a18..71a4f` routes that request to `72643` with `EBX=miniport`.
The selected input `+8` branch pushes three actual arguments at `72696..726a6`:

1. hardware owner `+1a8c` destination;
2. miniport owner;
3. callback wrapper `miniport+c440`.

The function pointer at MM `47b74` is **MM `6b16c`**, not an OS import. Its
entire actual body stores argument 2 into argument 1, returns zero and pops
three arguments. It never reads argument 3 or the callback table. The actual
getter `1795f` subsequently reads `miniport+4 -> hardware+1a8c`. Both methods
are CPU software pointer operations. They are not a cold state predicate or a
PLL callback invocation.

The checker executes the original constructor, `72643`, `6b16c` and getter in
controlled CPU owner records. Passive guest reads prove that this branch does
not dereference the wrapper or table. These records are an ABI fixture, not a
successful constructed miniport or observed hardware state.

## Actual-input invocation boundary

The retained passive run uses the existing M9128 PCI/ROM/SPI/GPIO input model
and the same common-buffer host API as the ordinary FAA producer. It adds only
observers at actual constructor `17dc4`, PLL entry `179a2`, `HwInitialize`
`56304`, and reads overlapping the published table's `+88` slot.

Actual `FindAdapter` does not return. Its last attempted BAR2 write is
`1c4c=0003007b` at actual MM store `2a438`, with actual caller return `391f1`.
The selected GPIO producer `391da..391ec` ORs the source output-enable mask
and clears the source data mask before calling the real store helper. The
existing input guard rejects the uncaptured change `80`; no response or write
echo is added. Constructor, PLL entry, slot-read and `HwInitialize` counts
are all zero before that boundary.

This establishes ordering only for that uninterrupted actual-input prefix.
The later MD aperture refresh explicitly supplies separately retained idle
inputs on the partial CPU owners; it does not resume and complete this original
`FindAdapter` stack. Its successful FAA CPU graph therefore does not establish
whether later Windows initialization invokes `179a2`, selects another cold
transaction, or relies on an inherited firmware state.

The open dependency is a source-joined, family-specific complete cold/warm
admission path after this boundary. Knowing the callback body or successfully
retiring FAA commands does not establish that path, nor prove that missing cold
programming caused the observed zero output. PCI memory decoding and the Linux
software initialized getter are separately scoped in
[the inherited-state checkpoint](parhelia-m9-faa-inherited-init.md).

## Check

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-miniport-cold-admission.py \
  --check-capture captures/m9128-device-inputs-20261006/miniport-cold-admission.json
```

To repeat the passive prefix, use `--bootstrap-out /tmp/miniport-prefix.json`.
The producer also writes its unabridged existing bootstrap proof beside that
file with suffix `.full.json`. This is local PE execution only; no target
access, additional GPU replies, CPU cold-state substitutions or native changes.
