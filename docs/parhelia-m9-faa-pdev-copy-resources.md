# PDEV resources required by the original sampled-copy continuation

This is a static shipping-source closure for the NULL `PDEV+d4/d8` blocker in
`b1290`. It identifies the genuine constructor and ownership path. It does not
prove returned device initialization, a prepared conversion, GPU execution,
or output pixels. The shipping `MxMD.dll` SHA256 is
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.

## Producer and real caller

The normal device initialization calls mode setup `2ff50` at `2dff0`, then
iterates the initializer table `1f2508..1f2668` at `2e03a..2e056`. Table row
`1f25c0` contains initializer `86660` and destructor `86890`. This module is
absent from the conditional fixture's `m9_md_runtime.initialize_resources`
module list (`849a0,273a0,7abc0,1f990`). That omission explains unconstructed
slots in the fixture; it is not evidence of a missing native FAA register.

`86660` constructs **three** PDEV resource wrappers, not merely two ad hoc
copy resources. `8672c` takes the address `PDEV+d0`; `86841` publishes the
returned wrapper through that pointer; `8684d` advances it by four bytes.
The loop ends after twelve bytes at `8685d`. Thus it publishes `+d0`, `+d4`
and `+d8` without a literal `mov [PDEV+d4]` instruction.

The policy comes from the live hardware properties at `PDEV+8 -> +4 -> +184`.
The initial modes for the last two slots are both `3` (`8666d..8667a`). The
first slot normally uses mode `2`, size `0x20000` (`866ef..866f7`). The
`0x100004` predicate and genuine `IOCTL23212c` result can select a different
first-slot policy; the `0x200004` predicate changes the last two slots to mode
`1` and size `0x80000` (`86705..86728`). These are source policies to evaluate
against live inputs, not configuration values to replay from an older capture.

For mode `2`, the module allocates `0x4270` bytes and invokes `8adc0`.
Other modes follow the actual master-device predicate `2e6f0`. A master
constructs the backing at its retained hardware owner `+3618 + slot*0x4270`;
a non-master borrows that existing backing (`867c9..86834`). The backing
constructor consumes the live PDEV `+80` and `+24` fields and calls `487d0`
at `8ae3f`. These fields require genuine mode/device provenance before extending
the conditional fixture. This audit does not supply their values.

The subsequent [mode-admission execution](parhelia-m9-faa-copy-constructor-admission.md)
closes their type: `+80` is a **numeric format code**, not a pointer. For an
accepted 32-bpp mode the source publishes `+80=66` and `+78=4` bytes.
The current mode query fails to write its count out-parameter, so that
execution does not admit a mode or run `86660`. A previously observed zero
stack local is not evidence of an empty mode collection.

For each backing, `868d0` constructs a `0x3ec` wrapper, records the PDEV at
`+240`, hardware at `+244`, and backing at `+248`. It initializes wrapper
`+234` to zero (`868dd`). Non-NULL publication therefore does not establish
prepared conversion readiness. The later preparation method `86a60` separately
consumes live PDEV `+78/+80`; it is outside this pinned initializer result.
Destructor `86890` traverses the same three slots. `86990` distinguishes
allocated mode-2 backing from shared backing and releases it through the real
source ownership path; dummy zero objects do not reproduce this contract.

## Consumer ordering and next boundary

`b130f` obtains the PDEV from the source resource's `+238`. `b1315` loads
`PDEV+d4` into ESI and `b131e` loads `PDEV+d8`, retaining both before the
staging copy `57d90` at `b1338`. The latter includes the pending wait in the
observed original continuation. Creating the resources after that wait would
not repair the already-loaded ESI/stack values. The genuine module belongs
before the original copy caller; this result does not authorize a forced pointer
or a replacement successful conversion result.

At `b135a` and `b1369`, the consumer tests the wrappers' `+234` fields. A real
wrapper whose readiness remains zero follows `b1ab6`, conditionally cleans
staging through `58080`, and returns the retained result. It does not itself
map pixels. The original `48c60` dispatcher must choose any subsequent handler.
A successful constructor is consequently useful for continuing the genuine
caller, but neither its constructor status nor its zero-readiness branch is a
GPU-completion or readback receipt.

## Reproduction

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-pdev-copy-resources.py \
  --image /srv/t2/xf86-video-parhelia/xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll \
  --proof docs/traces/m9-faa-pdev-copy-resources-static-20261007.json
```

The checker verifies the whole image hash, eleven decoded instruction spans,
the initializer/destructor table entry, exact publisher/consumer instructions,
and six contradictory-policy controls. The JSON includes the checker hash.
No vendor initializer is executed by this checker, and no GPU reply is supplied.
The required next evidence is genuine mode-input provenance and returned
`86660` execution before entering the original copy stack, followed by its real
next unknown input boundary.
