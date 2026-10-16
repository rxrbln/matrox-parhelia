# M9128 cold MC producer and bounded postcondition observation

The original Linux `0540` MC defaults and setup producers can execute on the
actual SPI-verified M9128 PInS without any supplied hardware read. This closes
their board-dependent write inventory; it does not establish the Windows cold
caller, a completed Linux cold boot, or an FAA failure cause.

`check-faa-cold-mc-writes.py` executes unchanged `m9x9754` (`2d2640`),
`m9x21748` (`2d28c0`), their original patch enumeration/lookup, indirect encoder
and branches. The fixture's sole owner pointer and raw PInS are admitted CPU
inputs. CPU reads outside those fields and the execution stack are rejected.
Accessors are write-only recording sinks; their writes never change a model
or become a readback shadow. The original verifier (`2d26f0`) stops before
its first hardware read: call `2d2714`, accessor `2b8ad0`, register `f28`.
No GPU or MC status value is supplied.

The input is the physical capture `spi-pins-physical.json`, SHA256
`08435025305a774318cef7542d8e5fa4c6b223480007b7b108a7877f1a1cae2d`:
`102b:0540`, revision 1, subsystem `102b:22c0`. Its completed SPI read at
`7460` provides all 1024 bytes, including the patch table. This uses the M9128
board rather than an M9148 PInS or old vendor trace.

The retained inventory records 76 accessor writes and four delays. Each event
has the actual original call/jump PC, accessor PC, return PC, register and
computed data. Each patch producer also records its caller, fallback, class,
and the independently decoded physical PInS record. Direct patches have full
masks in this admitted board case. Indirect data is written to `f1c` before
the original `11000 + f00 + index` selector is written to `f18`; those two
events remain separate and ordered.

Some important computed outputs are:

| Field | Actual board producer | Fallback in the same producer |
| --- | ---: | ---: |
| `f58` | `42088000` | `32000000` |
| `f5c` | `10601060` | `00000000` |
| `f2c` | `02c8eca7` | `02c8e8a7` |
| `f30` | `33a63f74` | `23a63474` |
| `f34` | `0268d605` | `026655a3` |
| `f38` | `0000006d` | `0000002b` |
| `f04` | `0448803e` | `0440803e` |
| indirect `ff` | `00000005` | `00000001` |
| indirect `1a..1d` | `00000000` | `ffffffff` |

Actual PInS `+46` bit 0 is one, selecting the tail that needs no MMIO-zero
read. `f14` is derived from the original patch, packed by the original code,
and follows the ordered `42 / 02000042 / 04000042 / 01000042 /
05000042 / 09000042 / 42` writes. Then `f10` becomes `000c7300` and `f14`
becomes `10000042`; the tail publishes `f08=04f3c000` and `f0c=00200000`.
These are computed outputs of this bounded board case, not portable native
constants or permission to apply the sequence piecemeal.

The outer transaction's original calls are `2f5f75→2d2640`,
`2f5f7f→2d2510` (indirect control zero), `2f5f87→2d1d40` (system clocks),
`2f5f8f→2d28c0`, and `2f5f97→2d26f0`. It first saves/holds four display
clocks and disables four display controllers, then restores those eight
words on both return paths. The separately executed producers in this
fixture do not bypass or claim execution of that enclosing transaction.
The current native address service's later `f04/f0c` updates and per-mode
pixel PLL do not constitute execution of this complete cold transaction.
Whether firmware has already established equivalent MC/system-clock state
is an observational question; ordinary SYSTEM output and FAA color response
do not prove every cold prerequisite.

`m9-cold-mc-read-only.c` provides that bounded first observation. It checks
the actual PCI identity, records boot ID, opens `resource2` read-only, maps
only its first 4096 bytes with `PROT_READ`, and reads exactly 19 direct MC
fields: `f04/f08/f0c/f10/f14/f28/f2c/f30/f34/f38/f3c/f40/f44/f48/f4c/f54/
f58/f5c/f60`. It performs no selector write, interrupt acknowledgment, MC
write, reset or submission. These direct fields are source controller data
or the original verifier's read; the probe deliberately excludes indirect
selector/data reads because selecting a record requires writes. An idle
read of `f28` only classifies the current source predicate: `ffffffff`
is the special early return; otherwise `((word >> 16) & 11)==1` is the
initial status condition. It cannot prove the destructive aperture test or
earlier initialization chronology.

Reproduce the CPU proof from this worktree:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-cold-mc-writes.py \
  --image /home/ai/src/parhelia/xf86-video-parhelia/m9xdriver-x86_64-1.4.3-80847-20140908-build_98/xserver/1_13_0/m9x_drv.so \
  --pins-capture captures/m9128-device-inputs-20261006/spi-pins-physical.json \
  --out /tmp/m9-faa-cold-mc-writes.json
cc -O2 -Wall -Wextra -Werror tools/m9-cold-mc-read-only.c -o /tmp/m9-cold-mc-read-only
```

Only the root hardware owner may run the read-only probe under the existing
device locks. No native cold write candidate is included in this checkpoint.

The root owner subsequently ran that helper on actual boot
`e6d15c3d-4b9e-4a4d-99af-e44810d2080f` and verified binary SHA256
`f386e997c8cb3feb36d77bb2ad30993963d90c91dca203a2ec3ddd128ef2c582`.
The exact JSON is retained as `cold-mc-current-observation.json`.
`check-faa-cold-mc-observation.py` derives the source's last direct writes
from the instruction inventory and compares the exact register set: 17 of
18 source-written direct values agree. `f28=00010000` satisfies the
original verifier's initial status condition, without proving its aperture
test or cold history.

The sole differing direct field is `f14`: source's last write `10000042`,
current read `00d00042`, XOR `10d00000` (bits 28, 23, 22 and 20). In this
full-mask PInS case, the original `2d23c0` lookup reads CPU patch records,
not hardware `f14`; setup never polls `f14` after writing. The source uses
`f28` for the status condition. It does not establish whether those upper
`f14` readback bits are persistent controls, status or effects of later
firmware activity. This difference therefore does not justify a register
write or explain FAA output. The closely matching MC image weakens a
missing direct-MC-image explanation while system-clock and enclosing cold
lifetime proof remain separate.

The original system-clock prefix is now separately executed by
`check-faa-cold-clock-inputs.py` on the same physical PInS bytes. The known
CPU field `board+42c` is `PInS+140`, producing `e24=70000000`, followed by
the actual 100-microsecond delay. It stops before the first `e00` read at
call `2d1dd9`/return `2d1dde`; neither that read nor later PLL status polls
receive a reply. The original pure predicate `2d1660`, executing its actual
jump table on the nine desired selector words at `PInS+11c`, admits system
synthesizers 0, 1 and 2 and rejects 3 and 4. Their PInS target fields are
each 500000 kHz. This establishes the board's desired source inputs, not
hardware clock programming, frequency readback or a separate FAA clock.

The probe now additionally supports `--system-clocks`, reading exactly 13
source-bound fields: `e00..e24` in four-byte steps, plus `e30/e34/e38`.
This mode changes the bounded read list only. Source SHA256 is
`e0e7d1898306fc50f4dfa6ce1f51f83e8ae0d16f2adb69a8ecf03e95d77d4d81`,
compiled helper SHA256
`347ab1f8c8f6ca5c2f129ac0b48b81709e04a9357fa3ec08e618dc5d490241ac`.
The prior 19-field observation retains the earlier exact helper identity.
These are Linux `0540` cold-producer inputs and boundaries; the Windows MD
assumed-cold/callback admission remains a separate open contract. A later
current-board clock read cannot substitute for a PLL status read following
an unexecuted cold write.

The Windows counterpart is independently executed by
`check-faa-windows-cold-mc-writes.py`. Its bounded ABI owner contains the same
verified raw PInS at `owner→board+250`. Original defaults `41892`, setup
`4191e`, patch lookup `41dff/383c5`, indirect producer `41dcc` and tail
branches execute unchanged. All 76 emitted register/data pairs and the four
10-microsecond delays equal the Linux actual-board producer in order.
Original verifier `41c61` stops before its `f28` read at call `41c7d`,
accessor `2a3a3`, return `41c82`. No unknown CPU owner field or hardware
status is supplied. `windows-cold-mc-writes.json` retains each original
call PC and the pinned body hashes.

The actual Windows `0540` dispatch sets `EBX=540` at `289c8` and calls
transaction `2ebd1` at `28aed`. Its original calls are defaults at `2ecf2`,
indirect control zero at `2ecff`, system clocks `401d8` at `2ed05`, setup
at `2ed0c`, and verifier at `2ed13`. The transaction saves/holds the same
four clocks and disables the same four display controllers before these
calls; it restores their saved words afterward. This closes the source
grammar for both vendor families used here. It does not supply the missing
FindAdapter GPIO/HMC inputs, invoke an unobserved callback, or claim the
actual Windows cold dispatch was admitted on the native boot.

Root's additional 13-field observation on the same `e6d15c3d` boot is retained
as `cold-clock-current-observation.json`. All nine desired selectors and the
source oscillator word agree. Feeding only that actual oscillator word to
the original CPU predicate `2d15e4` branches to `2d163f`, skipping its
oscillator reinitialization writes. All three selected synthesizer words are
`010063dd`: the proved divider encoding decodes to fields `30/16/13/1`.
At the producer's 27000-kHz reference this derives approximately
498461.538 kHz, versus the board target of 500000 kHz. That is an arithmetic
interpretation of the observed word, not a measured clock, a claim about
PLL post-write status or evidence of an FAA-specific bank. The old M9148
capture's different divider word was not used as an expected M9128 value.

Together these checks rule out a missing published direct MC image or
missing desired system-clock selector image as an observed discrepancy on
this boot. They do not establish every indirect MC record, oscillator
frequency measurement, full cold transition or a private FAA unit enable.
The only observed direct-MC distinction remains the unclassified `f14`
upper readback bits; no source-proven rendering dependency has yet joined
them to sampled mode 9. The observations therefore support further producer
and consumer tracing, not a cold-register write candidate.
