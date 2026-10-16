# Earlier mode admission for the PDEV conversion constructor

The original mode caller and count query now execute with source-known CPU
inputs. The query **does not write its count out-parameter**; the fixture stops
before the selector consumes that stack local and leaves `86660` uncalled.
This is an emulator initialization boundary, not a demonstrated
cause of the native FAA image failure. No GPU completion or pixels were
supplied. Existing hardware1d0/1f0 retirement results remain separate.

The shipping image hashes and nine decoded spans are checked by
`tools/check-m9-faa-copy-constructor-admission.py`. The source ordering is:

1. Original GDI caller `105f0` reads DEVMODE bpp/width/height/Hz at
   `10618/10631/10627/1063b`. Its32-bpp table branch at `1067b` emits code64.
   The fixture executes this caller and stops at its actual `2dea0` call before
   another PDEV initializes. It retains the produced CPU request before a
   later emulator call can reuse its stack.
2. `2dea0` calls `64bd0` at `2df09`, before mode setup and the module loop.
   `64bd0` requests `2334d8`, prepending `ffffffff` to the actual six-DWORD
   caller request. `MxMM.sys:60263/60268` registers `6053c` for this request;
   its `6056d` call enters the real `6e049` mode selector.
3. `6e061` requests the original mode count via `6dd52`. The actual branch at
   `6dd7e` sees AL1 from `6dcff`, while `6dd98` sees error1 from the typed
   mode-record query `6093c`. It skips the count store. `6dd52` nevertheless
   returns AL1 unconditionally at `6dde1`. The observer records **no stores**
   to the actual output DWORD during this call. It stops at `6e066`, before
   the selector can consume stack residue. The initial mode-only experiment's
   local0/error11 was not a proved empty collection or valid selection reply.
   Mode setup and the constructor stay uncalled.
4. Only a successful selection may feed its live index to `2ff50`. The latter
   issues real `232014`, `2334cc`, and `2334c8` requests before publishing mode
   fields. `300e5..300f6` derives `+78=4` bytes for32 storage bits;
   `3024a/3024f` publishes numeric format66 to `+80`. It is not a pointer.
   Only successful mode setup permits the real `86660` constructor, whose
   three wrappers must be retained before original `b1290` loads them.

The mode selector consumes only the first four caller request DWORDs
(width,height,format-enum,Hz). The final two remain the original live caller
stack bytes; the fixture does not replace them with captured values. It sends
the source-produced packet through actual `6c755` dispatch, preserves the real
result and output-byte count, and propagates only actual CPU stores to existing
shared owners. It never bulk-refreshes a shared allocation over MD writes.

`tools/faaemu/m9_pdev_copy_resources.py` refuses existing mode/conversion slots
or a published queue. It restores the previous I/O handler on every exit.
There are no forced d0/d4/d8, wrapper readiness, conversion result, mode count
or mode records. The observed request changes CPU allocations and startup
history, so prefix/resource-effect equivalence is explicitly **not proved**.
The old hardware receipt is not applicable to this fixture. A successful future
constructor requires a separately exported and qualified whole fixture.

## Reproduction

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9-vendor-faa-copy-constructor.py \
  --out /tmp/m9-faa-copy-constructor-20261007.json --require-constructor
# Expected exit1: unwritten count output, constructor not admitted.
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-copy-constructor-admission.py \
  --proof docs/traces/m9-faa-copy-constructor-admission-20261007.json
```

The retained proof hashes its implementation modules, vendor images and actual
PCI input artifacts. It records unchanged zero mode/conversion slots and zero
mode-device MMIO events. The checker rejects six contradictory admission or
receipt claims. These controls verify source ordering and the failed admission;
they do not stand for successful constructor execution or pixel readback.

The next missing prerequisite is the **genuine typed mode-record producer**
consumed by `6093c/6dd52/6ddef`, with its original caller inputs and
earlier startup order. Supplying a fabricated count, injecting a format code,
or creating wrappers after the pending wait would bypass that prerequisite.

## Correction to the initial mode-only checkpoint

Commit `fd7bdb2d` recorded a local0 followed by source error11. Its description
of an empty mode collection was too strong: the shipping helper can return AL1
without initializing that local. This follow-up adds actual output-store
attribution, stops before the unwritten count is consumed, and replaces the
retained proof with that strict boundary. It does not manufacture a count or
convert a failed query to successful initialization.

The complete earlier caller also emits `232024` at `2defb` before `64bd0`.
A separate exploratory execution of that genuine control returns0 but changes
the later typed-record path; it does not close successful mode admission.
It belongs to the next startup investigation, not this mode-only proof or an
existing FAA hardware receipt.
