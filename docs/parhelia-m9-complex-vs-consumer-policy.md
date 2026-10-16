# Complex vertex consumer mask policy

The genuine bump GLSL compiler/finalizer archive and the qualified native bump
clip result are separate evidence. This note closes a bounded CPU consumer join;
it does not establish a full live EE19 producer/consumer descriptor binding.

The pinned MxMD.dll SHA256 is
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
The new [actual helper report](../captures/m9x-complex-vs-consumer-20261007/proof.json)
uses both genuine retained variants from
[the compiler archive](../captures/m9x-complex-glsl-finalizer-20261007/README.md).
All original program and constant-declaration bytes are retained; address-bearing
record pointers are relocated to fresh bounded host owners. No original pointer
is sent to a runtime helper as an address.

## Source path

`17edb0` initializes its render entry's `+58c` to zero at `17ee3c`.
`17faf0` reads the actual variant's texture count at `+23c` and paired route
count at `+28d`. Both counts are zero in both finalized bump variants. The actual
enumerator returns success with texture/route counts zero and leaves the entry's
`+58c` unchanged. This path does not call `187860` or `18c2d0`.

The filter helper has the noninitializing entry-mask writes at `18b31c` and
`18b337`: when working record `+78` is nonzero, it takes two selector fields
from its input descriptor and ORs their bits into render entry `+58c`.
The same helper clears working `+78` at `18b1db`; resource virtual callbacks and
other texture policies remain outside this no-vertex-texture proof. This is not
an assertion that the texture exclusion path is universally dead.

`17ef10` is the corresponding program consumer. For every set `+58c` bit `p`,
it prepends one instruction with the actual source expressions selecting
register `2*p` and its sibling `2*p+1`; it preserves the original arithmetic
suffix. It then calls real `14eba0`, publishes the retained program pointer and
slot count to common shadow `+f0/+ec`, and advances the CPU program serial at
`+e8`. The serial is not a GPU completion value.

`17f040` at `17f052..17f06f` takes the compiled variant's `+397[23:8]`, removes
entry `+58c`, and writes common shadow `+10[15:0]`. Higher bits derive separately
from declared input/route groups and compiler flags. `f4f50` emits this shadow
at indexed register `88` in the actual `c18a8988` packet group. `180640` maps
constant declarations; `181440` sets other policy fields. Neither is presented
as executed in this probe.

## Reproducible scope and controls

[check-m9-complex-vs-consumer.py](../tools/check-m9-complex-vs-consumer.py)
executes real `17faf0`, `17ef10` (including `14eba0`), `17f040`, and `f4f50`.
Only bounded host allocation/free services are supplied. The explicit controlled
host inputs are a vertex role, empty initial command shadow and four owned empty
texture blocks, positive program capacity, CPU serial 40, and emitter mode 1.
The checker isolates common packet group 1; it does not invent the program
relocation callback or submit a draw. It rejects nonzero texture/route metadata
before entering the vendor helpers.

Both distinct heap placements produce identical program and packet bytes.
The 90-slot variant remains `ff00` at register `88` with `+58c=0`; actual program
preparation publishes 91 slots. The one-slot variant remains `ffff`, publishing
four slots. Controlled `+58c=100/200` cases execute the real prepend path and
emit the corresponding excluded bit. A hook observes the instruction insertion
before real finish scheduling, so finish-marker changes cannot obscure the
arithmetic suffix check. These cases are source-policy controls, not statements
that a real texture binding should select those bits.

The full EE19 field/view/address-domain join is still open. No runtime adjustment
is justified by this helper proof alone, and no standalone `+58c` mask toggle
implements the vendor contract. Existing coupled output-pair closure, default
cap 2, and the prior physical qualification stay unchanged.
