# M9128 fresh warm admission and native ordering

The actual fresh M9128 observation selects the saved Linux vendor's full
0540 warm transition. Native KMS implements part of that transition, and the
retained FAA logs show its basic handoff before submission. No present evidence
joins the remaining display transaction to FAA pixel shading or resolve, so
this checkpoint does not propose additional register writes.

The board observation is [the parent's signed fresh record](../tests/m9-fresh-warm-entry-20261006/observation.json),
commit `31410a91`, boot `df3986cd-a4c0-4a4a-a5bc-3fa13a41b5d3`, device
`102b:0540`, subsystem `102b:22c0`, before module load or a service submission.
Its `1c10=1`, `1fdc=04000000` fail the source direct-return predicate. This is
separate from the previously retained post-service pair `2/04010000`.

## Original transaction

The exact X image SHA256 is
`3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`.
The separately initialized CPU bucket admits `2b5650` to call `2eb7b0` at
`2b56b6`; actual chip WORD `0540` selects `2f2c10`. The helper's complete
`2f2c10..2f2eaf` body SHA256 is
`c525acc0da9bf455a00b5bf7fe1eee26fe304715c53b8eb71c0fa00e43b87db3`.
The checker executes only the original predicate instructions using the
retained words in CPU locals and stops at `2f2c5b`, before its first call.
It does not supply MMIO accessor replies or execute a hardware helper.

The failed predicate selects these ordered source operations:

1. Provider delay call `2f2c63`, argument `28487`.
2. Four output-disable requests at `2f2c6d/7a/87/94`, calling `2f2b60`
   with logical indices zero through three.
3. Two `2f11a0` indexed VGA waits at `2f2c9c/ca4`.
4. Live `1c10` bit-zero clear, live `1fdc` bit-16 set, provider delay ten,
   then live `1c10` bit-one set.
5. Live clock hold/sync updates, including live `1cac` low byte OR `f0`
   before final hold writes, then provider delay ten.
6. Live `1800` bit-13 clear at `2f2e74/84`.

The four logical output indices are not portable physical port numbers.
`2f2b60` looks up CPU bucket type nine, uses the selected byte to look up
type-eight records of size `34`, and consumes record `+18` and connector
bytes. It then dispatches either `2f0640` or `2dca60`. The first helper admits
actual connector zero/one to `1c80/1c84`; the other dispatches through further
typed output records. A complete implementation must derive these owners and
connector mappings from the correct board; copying four disable writes is
not equivalent.

`2f11a0` writes byte index `12`, `7`, `6` to `1fd4` and reads the corresponding
`1fd5` bytes. Conditional waits read `1fda` bit three, falling then rising,
with bounded loops. Those replies require their actual indexed request
chronology. A saved idle DWORD at `1fd5` cannot substitute for them. This
checkpoint therefore makes no request for an indexed target transaction.

## Native order and observed boundary

At the inspected kernel source, `m9x_firmware_initialized()` tests only PCI
command MEMORY. Probe performs address/requester setup but does not call
`m9x_mode_take_native()`. KMS `m9_pipe_enable()` conditionally invokes
`m9x_mode_program()`, whose order is handoff, pixel clock, timing, link,
video reset. Basic native handoff therefore follows probe and depends on a
real mode-program request.

`m9x_mode_take_native()` performs live flag/misc16/hold/sync updates and the
byte VGA MISC write. It does not perform the source-selected output-record
disables, indexed waits, pad OR, or `1800` bit-13 clear. Later
`m9_program_timing()` clears format bits 22..29 and interlace bit 12 in the
live DC control; it preserves bit 13. This is an exact source difference,
not a decoded FAA-enable bit.

There is also a distinct native live pad write in transmitter enable:
`m9x_tx_enable()` clears `M9_PAD_GATE_OFF(index)` after link training.
Consequently omission of the warm pad OR does not establish the eventual
pad state. The retained constant-green physical log reports pad `e1` after
head-zero enable, and basic handoff `flag2/misc63→68` at time `62.586374`,
before source-owned FAA admission at `65.796334`. That log disproves the
narrow claim that the entire basic handoff was absent in that failed FAA
case. It does not establish execution of the full vendor transaction.

The native omission comments refer to measurements documented explicitly on
**M9148** in [the family display note](parhelia-m9.md#vga--native-handoff),
including a pad write requiring reboot recovery. Neither that board's
measurement nor its omission policy establishes an M9128 FAA prerequisite.
Conversely the Linux vendor warm helper is not a proved Windows miniport
cold/PLL invocation or Windows MD rendering admission. These are separate
source lifetimes.

## Check and remaining boundary

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-warm-native-order.py \
  --image /home/ai/src/parhelia/xf86-video-parhelia/m9xdriver-x86_64-1.4.3-80847-20140908-build_98/xserver/1_13_0/m9x_drv.so \
  --observation /home/ai/src/parhelia/xf86-video-parhelia/tests/m9-fresh-warm-entry-20261006/observation.json \
  --native-dir /home/ai/src/parhelia/src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm/m9x \
  --log /tmp/m9-faa-constant-green-stage-20261006/proof/dmesg-after.txt \
  --out /tmp/m9-faa-warm-native-order-20261006.json
```

PASS pins the complete helper bodies, original branch and call joins,
native function order, bit-13 preservation and the retained log ordering.
It records the current native file hashes; it changes no kernel or capture
inputs. The optional log is an earlier failed physical case, not the fresh
observation boot. Neither is used as a provider reply.

If a future parent-owned read-only observation narrows the state after native
mode programming, the relevant live fields are `1c10/1fdc`, `1cac`, `1800`,
`1c00/04/08/0c` and `1c24` on the same identified boot before first FAA.
Those reads can determine whether a source postcondition differs at that
boundary. They still cannot establish the required indexed-wait history,
complete MC/PLL success, or a display-to-FAA dependency. The latter producer/
consumer join remains the precise unknown; arbitrary cold/reset or pad writes
are not authorized by this audit.
