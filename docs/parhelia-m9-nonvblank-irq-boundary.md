# M9/Phoenix non-vblank IRQ and recovery boundary

This note records the remaining static interrupt joins in the saved M9 Linux
and Windows drivers.  It separates exact service consumers from names inferred
from bit positions.  Addresses are image virtual addresses for `MxMM.sys` and
section-relative text addresses for `KernelLibraries.o_shipped`.  `[BIN]`
means directly encoded in a saved binary; `[DER]` is a conclusion from two or
more such observations.

Recheck the byte-level joins with:

```sh
python3 tools/m9-nonvblank-irq-evidence.py
python3 tools/m9-irq-selector-evidence.py
```

The checked artifacts are:

- `MxMM.sys`, SHA-256
  `bf743961b4846e90f32cbdf868d69240d7c403fcbd0a8d9cd6d8783ef9bd62c6`;
- `KernelLibraries.o_shipped`, SHA-256
  `bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`.

## Exact controller register and ordering contract

The saved paths distinguish raw pending state, write-one-to-clear
acknowledgement, and enable masks as follows **[BIN]**:

| bank | raw pending read | W1C write | enable mask |
|---|---:|---:|---:|
| first | not isolated in either handler | `0x8d8` | `0x8dc` |
| low | `0x8e0` | `0x8e4` | `0x8e8` |
| high/cascade | `0x8f4` | `0x8f8` | `0x8fc`; Linux caches it at interrupt object `+0x8c` |

Calling `0x8d8`, `0x8e4`, or `0x8f8` a readable status register is not
supported by these binaries. Linux handler `m9x547` reads high raw `0x8f4`;
when its bit 31 is set, it reads low raw `0x8e0` and low mask `0x8e8`. It
acknowledges exactly `raw_low & mask_low` through `0x8e4`, and independently
acknowledges `raw_high & cached_mask_high` through `0x8f8`. The Windows ISR
uses the same `0x8f4` raw, `0x8e0` raw, `0x8e8` mask, cached high mask, and
per-source W1C writes **[BIN]**.

Per-source enable leaves first acknowledge the selected W1C bit, then OR it
into the mask. Disable leaves first remove the bit from the hardware mask and
high-bank cache, then acknowledge it. Linux controller shutdown `m9x704`
writes zero to `0x8fc`, `0x8e8`, and `0x8dc`, clears the high-mask cache, and
then writes all ones to `0x8f8`, `0x8e4`, and `0x8d8` **[BIN]**. This pins the
mask/ack behavior needed by a fail-closed interrupt implementation without
assigning a semantic name to any source.

## Windows subscriptions and consumers

The generic dispatcher at `0x1971b..0x199e4` is a notification framework, not
a recovery path [BIN].  Registering the first subscriber enables its selector
through `0x19bee`; removing the final subscriber masks and acknowledges it.
`0x199e4` walks the pending records and directly calls or schedules each
subscriber.  It does not program an engine-reset register.

The general miniport interface at `0x1a7d7` can register a caller callback for
every hardware selector 0 through 12 [BIN].  Its 15-entry input jump table at
`0x1a957` maps inputs 0..12 to the same selector and the final two inputs to an
auxiliary selector 13.  Vtable `0x484a4` leads to `0x1a998`, a four-instruction
thunk which calls the function stored at object `+0x4c` with argument `+0x50`.
This API makes an event observable to a miniport client, but supplies no
semantic fault name or recovery operation.

There are only two calls to the fixed-mask subscription constructor `0x19d5c`
in executable `.text` [BIN]:

| caller | logical mask | selectors | internal consumer |
|---:|---:|---|---|
| `0x1b824` | `0x100` | 8 | vtable `0x48528` -> `0x1b990` |
| `0x236f3` | `0x605` | 0, 2, 9, 10 | vtable `0x48f3c` -> `0x23751` |

Selector 12 has one additional directly built object at `0x23929`, registered
at `0x23944`; vtable `0x48f40` leads to `0x239a3` [BIN].  The remaining direct
calls to `0x1971b` construct vblank or caller-configured notification objects.

These joins classify the previously unnamed sources as far as the artifacts
permit:

| source | selector | exact saved consumer/action |
|---:|---:|---|
| low `0x4` | 1 | ISR notification record; optionally forwarded by the general caller-callback API |
| low `0x8` | 3 | ISR notification record; optionally forwarded by the general caller-callback API |
| low `0x10` | 8 | `0x1b990` reads display MMIO `0x1ebc`, extracts bits 15:14 and bit 8, updates output state, then calls the installed output callback |
| high `0x4` | none | W1C acknowledgement only in the ISR |
| high `0x8` | none | completion for the rectangle-transfer service at `0x1b374`; callback or completion-word wakeup |
| high `0x40000` | 9 | invokes synchronized command-queue callback `0x23751` |
| high `0x10000` | 10 | invokes `0x23751`; if no queued record is retired, its selector-10 branch reads device `+0x1fc` and writes that saved endpoint to MMIO `0x404` |
| high `0x20000` | 11 | ISR record and optional general caller callback; no fixed internal subscriber in the saved miniport |
| high `0x01000000` | 12 | separately registered worker `0x239a3`; updates service register `0xf0c`, builds/copies work, and can kick register `0x31c` with `4` |

The complete ISR at `0x10875..0x10f02` accounts for every raw source that it
tests [BIN].  Its low-bank map is:

| raw low bit | ISR record/action |
|---:|---|
| `0x1`, `0x4`, `0x2`, `0x8` | acknowledge and build selector 0, 1, 2, 3 records respectively |
| `0x100`, `0x400` | acknowledge and build selector 4, 5 records |
| `0x200`, `0x800` | acknowledge and build selector 6, 7 records |
| `0x10` | acknowledge and build selector 8 record |
| `0x1000`, `0x10000000` | acknowledge only |
| `0x2000`, `0x20000000` | acknowledge only |
| `0x4000`, `0x40000000` | acknowledge only |
| `0x8000`, `0x80000000` | acknowledge only |

Selector 4 additionally samples MMIO `0x1e24` bit 25 and stores its inverse in
notification record `+8`; selector 6 performs the same operation on `0x1f24`
[BIN].  Selector 8 samples `0x1ebc` as described above.  These are display
status additions to the notification records.  They do not branch to the
requester, a 3D service, or reset code [BIN].  This also closes the apparent
`0x1e24`/`0x1f24` lead: the addresses name per-head display blocks, not hidden
engine-fault status [DER].

In the high bank, `0x40000`, `0x10000`, and `0x20000` build selectors 9, 10,
and 11; `0x8` completes the rectangle-transfer service; `0x01000000` builds
selector 12 after serialization; and `0x4` is acknowledged without building a
record [BIN].  High bit 31 is only the condition for entering the low-bank
decoder.  No other high bit is tested by this ISR.

The `0x404` write is the strongest command-engine relationship recovered from
an unnamed IRQ [BIN].  It is a requester resume/repost action: the value comes
from saved device state and the callback does not rebuild engine state or issue
a global/3D reset [DER].  Selector 9 shares the synchronized queue consumer but
does not take the selector-10 repost branch.  Selector 12 performs auxiliary
work rather than recovery; its ISR-side helper at `0x196be` only writes
`0x01000000` to W1C register `0x8f8`.

Selectors 5 and 7 therefore exist and can be forwarded through the general
caller callback, but have no fixed internal consumer in the saved miniport.
The eight paired low bits in the acknowledgement-only rows and high `0x4`
remain existence/W1C evidence only.  None joins a requester-failure handler or
3D reset [BIN].

## Linux boundary

Linux low-level handler `m9x547` at `0x3fd90` computes enabled low and high
active masks and acknowledges them through `0x8e4` and `0x8f8` [BIN].  Core
handler `m9x1012` at `0xdec0` rejects the interrupt unless raw `0x8f4` bit 31 is
set, and translates only low `0x1`, `0x2`, `0x100`, and `0x200` into slow-work
bits.  Those four sources are independently joined to the four vblank API
selectors.  No high source, low `0x4`, low `0x8`, or low `0x10` reaches a Linux
slow callback [BIN].

The individual Linux enable/disable/W1C leaves for the unnamed sources are not
referenced by a relocation or function pointer in the saved object.  A
high-only source may consequently be acknowledged by `m9x547` while
`m9x1012` returns `-1`/`IRQ_NONE`; it cannot invoke hidden 3D recovery in this
stack [DER].

String-table audits reinforce this boundary without naming hardware.  The
Linux diagnostics include size mismatch, invalid buffer, and vblank messages;
the Windows strings include configuration and display services.  Neither
artifact contains a word-level `fault`, `hang`, `timeout`, `recover`, or
`recovery` diagnostic [BIN]. No interrupt caller reaches the known requester-
local restart sequence, and neither stack exposes a callable 3D/global-engine
reset. The complete saved consumer join therefore closes the static
classification negatively: no source can be identified as requester fault or
3D fault, and no bounded reset/recovery sequence can be recovered from these
binaries. Hardware fault injection is required to give an unnamed source
either semantic label.

## Smallest decisive captures

Two isolated tests are needed because a bad requester address and a bad 3D
program need not report through the same source:

1. Submit one bounded requester record whose final endpoint is deliberately
   unmapped. Enable one unnamed source at a time. Capture first-bank mask
   `0x8dc` (the raw read address paired with W1C `0x8d8` is not recovered),
   low raw/mask `0x8e0/0x8e8`, high raw/mask `0x8f4/0x8fc`, `0x314`,
   `0x400/0x404`,
   `0x500..0x524`, and the memory fence immediately before submit, at IRQ, and
   at a fixed timeout.
2. Repeat with one otherwise valid draw containing the smallest known-invalid
   3D opcode or program termination.  Keep addresses valid so that a 3D decode
   failure can be distinguished from requester/address failure.

For each test, log every newly asserted raw bit before acknowledgement and the
effect of the already decoded requester-local stop/start sequence.  Mask the
source before acknowledging it.  If local restart fails, stop the experiment;
the saved drivers provide no defensible stronger reset sequence.  These two
captures distinguish requester fault, 3D fault, service IRQ, and silent
timeout with the minimum number of intentionally failing submissions.
