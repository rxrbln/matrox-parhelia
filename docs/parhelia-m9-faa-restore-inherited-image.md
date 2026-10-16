# M9128 ordinary restore initializer

The actual Windows allocator and queue activation select initializer `0x656c0`,
type one, with 1,024 records. Its complete CPU-produced `0x181000`-byte image
matches the native FAA virtual restore grammar. This closes the initial image
and selected variant, not GPU-saved state or Windows cold/PDEV admission.

The retained passive capture is
`captures/m9128-device-inputs-20261006/normal-faa-restore-inherited-image.json`.
Check it against the vendor binary and current native prefix/record arrays:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-restore-inherited-image.py \
  --capture captures/m9128-device-inputs-20261006/normal-faa-restore-inherited-image.json \
  --native-source /home/ai/src/parhelia/src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm/m9x/m9_requester.c
```

The checker also supports `--out`, using the actual M9128 inventory/resources,
`--runtime-tools` and `--bridge-tools`. It invokes the existing allocator with
registry `System.MmFlags=0x101f8`, `DirectX.D3DEnable=1`, then the actual ordinary
queue activation. Passive instruction hooks record the selected initializer,
full-fill inputs and every record DWORD store. It supplies no GPU completion.

At `0x653ee..0x653f5`, the actual CPU fills the entire owner with `0x45454545`.
Call `0x653f8` produces the 248-byte prefix. The branch at `0x653fd` chooses
`0x65910` only for type two; the observed type-one owner calls `0x656c0` at
`0x65418`. No alternate initializer was observed. The known fill is thus an
actual CPU producer, not inferred from zero-filled emulator backing.

At `0x658a4`, 20,480 observed DWORD stores cover all 20-word records. Record
placement is owner `+0x700+i*0x600`; work placement is live GPU base
`+0x200+i*0x600`. Record one alone uses variant `0x0c000000`; the remaining
records use `0x62000000`. Fields 9, 14 and 19 use the live resource's tag one;
field 10 is `work+0x400|1`. These match native's private virtual-restore
preparation. The checker rebuilds pointers from the observed live base and
native immutable arrays, then compares the complete image, including filled
gaps and tail. Historical captured addresses are never native inputs.

The full-image SHA-256 in this run is
`dcdb24d2f050271a6dfc172dac862885c43c0ea5ed7417a3c286308b727aacba`.
It is placement-specific evidence, not a portable image to upload.

## Separate device-enable boundary

The conditional allocator/queue fixture does not execute all of Windows
`DrvEnableSurface` (`0x10820`). That caller separately enters `0x10d10`, which
issues zero-length request `0x23201c` at `0x10d29`. The actual miniport route
is `0x6c770 -> 0x708de -> 0x70938 -> 0x70a41`. Its admitted chain calls
`0x5ed49`, `0x5e909`, `0x5f036`, `0x5d475` and `0x5ea66(1)` with intermediate
failure checks. This is distinct from request `0x233054`, which was already
closed as miniport owner bookkeeping.

That separate chain touches head/profile/resource collections and display
state; its complete actual board admission and hardware effects are still
unproved. It is not evidence for an omitted FAA-only enable bit or a license
to copy inherited MMIO values. Requester arming, partition controls, service
windows and the restore image are already accounted for by their own source
producers. The remaining boundary needs a genuine device-enable/cold-state
contract with board inputs, rather than a changed restore variant.

Further static consumer pins narrow two members of that chain. `0x5ed49`
iterates collection type three and calls `0x1cfbd`; the latter increments a
CPU collection DWORD after `0x1d02c` returns its backing pointer. It neither
restores GPU context bytes nor writes an engine enable. `0x5d475` constructs
an operation descriptor and calls `0x5d6f5`; the selected head index must be
less than four (`0x5d754..0x5d75f`). Consumer `0x5af6d` selects its state at
profile `+0x21fb14+head*0x74`. Its four-byte branch at
`0x5b035..0x5b064` copies three 256-entry 16-bit tables, using source offsets
`+0x18/+0x218/+0x418`. This is concrete head/table state, not a program,
pool22 or FAA-address publication. These added pins are static source
evidence; the retained dynamic run did not invoke that device-enable chain.
Other members remain open, so this bounded classification does not claim
the entire request is irrelevant to rendering.

Consumer `0x5e909` loops the four selection flags at profile `+0x2fec`.
For each selected head, `0x5e980` clears one bit in the mapped `0x8e8` word:
heads zero through three select `0x20`, `0x40`, `0x04000000`, `0x08000000`.
The original instructions are checked for all 16 selections using poisoned
CPU scratch cells by `tools/m9emu/check-faa-head-resource-consumers.py`.
Those cells are explicit synthetic ABI inputs, not MMIO observations or
read replies added to actual-input bootstrap. Native initialization already
writes IRQ-low mask `0x8e8` zero; this consumer supplies no missing enable.

`0x5f036` builds operations through miniport `+4 -> profile+0x1ec`.
Setter `0x11ab8` requests operation six with a 20-byte payload at `0x11ae1`,
then stores the caller's four rectangle DWORDs and trailing zero. Commit
`0x11b1d` calls `0x2536b` with that same owner. The latter validates a CPU
record's state byte three, advances the current/previous record indexes,
changes their state bytes and calls `0x292ae -> 0x2a179(2)`. This is a separate
miniport mode transaction, whose actual downstream hardware work is not
exported by the conditional MD frame. The checker pins these producer and
commit instructions; it does not execute or supply status for that whole
transaction. Its existence alone does not establish a FAA prerequisite.

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-head-resource-consumers.py
```
