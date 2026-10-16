# M9128 source write-only continuation and the next missing input

A separate write-only sink advances actual CPU execution past the first GPIO
write guard, but does not reach callback publication. The strict probe stops
at an ownership-grant read. An explicitly counterfactual control with one
retained physical grant stops at the immediately following GPIO input read.
Neither run supplies a GPIO readback shadow or proves complete cold admission.

The normal capture guards remain unchanged. This extends only
[the passive admission checkpoint](parhelia-m9-faa-miniport-cold-admission.md).
The probe verifies the MSerX `MxMM.sys` SHA256
`bf743961b4846e90f32cbdf868d69240d7c403fcbd0a8d9cd6d8783ef9bd62c6`.
Its actual source PCI input is `102b:0540`, revision1, subsystem `102b:22c0`.
The actual OS mapping uses that retained miniport owner and the captured M9128
BAR2 resource `fdffe000/2000`. Those are source identity checks, not addresses
to transplant into a native driver.

## Strict sink, with no additional read input

The probe records these CPU-computed stores at actual MM instruction `2a438`:

| Register | Value | Actual return to producer |
| --- | --- | --- |
| `1c4c` | `0003007b` | `391f1` |
| `1c4c` | `000300fb` | `3920d` |
| `318` | `50000003` | `34234` |

The first two values come from the actual `391da..39208` output-mask producer,
separated by its real CPU stall requests. They are recorded without updating
the GPIO model's control word. The third value comes from `34219` clearing
bit29 of the independently admitted `318` read. It is also recorded without
updating HMC semantic state.

The next attempted read is actual `2a3c5`, register `3b0`, with caller return
`34248`. The trap supplies no value and the instruction does not finish.
Actual following instructions `34251/34254` would test grant bit1. Constructor
`17dc4`, PLL entry `179a2`, table-slot `+88` reads and `HwInitialize` `56304`
all remain unexecuted before this boundary.

## Explicit physical-grant control and its identity limit

The optional control verifies existing `ddc-hmc-physical.json` SHA256
`f06fe16cd0d29b5a3b88e665238681d90acc361bbf876ea24d7b9505ab7f7059`.
That artifact records physical PCI ID `102b:0540`, saved/requested control
`70000003/50000003`, request bit29, grant mask2 and an acquired physical read
`3b0=ffff`. The source producer and selected single read match those exact
register/mask/request semantics. This is not a portable `ffff` status constant.

The artifact has no embedded boot or subsystem identity. Although retained
in the M9128 capture family, it cannot establish an uninterrupted cold boot
join to this CPU request. The proof explicitly labels its use **counterfactual**,
and marks the missing embedded identity fields. It supplies this one selected
captured grant, not a synthesized success or a value derived from a write.

Actual CPU execution then computes `1c48=000100fe` at `2a438`, returning to
`42e4f`. `42e37..42e4a` produced it by clearing the caller mask and ORing the
caller value into its preceding captured idle read. The sink records this
store without turning allocated MMIO backing into device semantics.

The immediate next read is `1c48` at `2a3c5`, returning to `42e5d`. It is
trapped **before** any value is supplied. The actual caller arguments are
`[1,10000,1]`, and `42e63..42e69` would test the newly read word against mask1.
Neither the initial idle `ff` nor the computed store `100fe` answers that
read. GPIO line/readback semantics after this operation remain missing;
callback publication and cold initialization are still unexecuted.

The output sinks have no hardware effect. The only added read in the optional
control is explicitly identified above. Normal input helpers, ordinary FAA
captures and native initialization policy are unchanged. No native register
patch follows from these bounds.

## Check

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/probe-faa-miniport-write-sink.py \
  --check-capture captures/m9128-device-inputs-20261006/miniport-write-sink-strict.json
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/probe-faa-miniport-write-sink.py \
  --check-capture captures/m9128-device-inputs-20261006/miniport-write-sink-ddc-control.json
```

Repeat local execution with `--out /tmp/sink.json`; the independently selected
control additionally requires `--ddc-capture PATH`. Both stop on the original
CPU stack, without claiming a returned `FindAdapter`, GPU execution or pixels.
