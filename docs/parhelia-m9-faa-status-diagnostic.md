# M9128 FAA status diagnostics and actual queue drain admission

The genuine public-red source flow intentionally skips the optional engine
status drain. Its queue constructor gets `0x80` from configuration property
`+0x300`; the resulting caller descriptor and queue `+0x5c` are zero. The
retained same-run observer saw six original flushes and one independent
final flush test `+0x5c` bit1 and branch past the MMIO reads. No missing
setter or backend wait is demonstrated for this admitted flow.

`MxMD.dll` SHA256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`
contains the complete producer chain:

- Raw property constructor `ba2ba..ba2e2` requests `System.BmDebugFlags`
  (`BmDebugFlags` string `1e75b0`, `System` prefix `1e70f0`) with default
  `0x80` and publishes its object at raw configuration `+0x108`.
- Derived constructor `5fb76..5fbab` wraps that raw property and publishes
  it at derived configuration `+0x300`. Getter `62310` returns `+0x10`.
- Queue publisher `1a2bc/1a2d7` invokes that getter twice. Its low bit0
  becomes caller descriptor bit1 and bit1 becomes descriptor bit2;
  the actual returned `0x80` therefore contributes neither wait flag.
- Queue constructor `22e49` selects queue `+0x5c`. The actual original
  `22ebb` copies fourteen DWORDs from its caller descriptor. The passive
  write trace records the zero initializer and this genuine copy, with
  no subsequent write to the flags through the two missing-input stops.
- `24530` is reached from `24eea` only when the flush argument is nonzero.
  `24671` tests flags bit1. Only its admitted branch waits for hardware-owner
  CPU count `+0x200` to reach zero, reads BAR `8f0` at `24690`, then repeats
  `246a3` while `(8f0 & 0x00030000) != 0`.

The same run executes the actual public constant-red requests and original
`ac1f0 -> afbe0` output path up to the first live GPU counter read `4e2fc`.
It leaves that read unexecuted. A separately admitted `23b80` request then
flushes the retained queue and stops at `23bc3`, also before its counter
load. The probe does not make the original copy return or release its
owners. The independent flush does not substitute for original caller
success. All seven flushes observed flags zero, visited `246af`, and never
visited `24690/246a3`.

The checker executes the unchanged conditional-drain instructions on a
separate CPU-only ABI fixture. Explicit synthetic RAM controls prove flags0
performs no status read; flags2 waits for either bit16 or bit17, ignores
bit24 for this predicate, and exits when the masked bits become zero.
These controls are never supplied to the actual source frame or hardware.
A frontend point140 alone does not state this predicate's current value.
The debug option does not justify adding a new mandatory native GPU wait.

## Interrupt and diagnostic boundaries

The M9128 Windows miniport uses raw high pending `8f4` at ISR `108fd`, raw
low pending `8e0` at `10a06`, and low mask `8e8` at `10a18`. Acknowledgments
are separate writes to `8f8` and `8e4`. Its diagnostic dump reads `8f0` at
`12ea4` and `8f4` at `12eb9` without a semantic fault decoder. The additional
`8f0` reader `7345d` folds the word into CPU arithmetic and global state;
it does not branch on an error bit. The `3c4` bit31 consumers belong to the
separately audited cold/HMC transaction, not an FAA fault-status join.

The existing [interrupt proof](parhelia-m9-nonvblank-irq-boundary.md) joins
high bit18 and bit16 to synchronized queue notifications, bit24 to the
service worker, and bit17 to logical selector11. No internal selector11
consumer or source-backed address-fault / invalid-command / FAA-error
meaning is established. Bit25 is reset/acknowledged but has no joined ISR
consumer. A newly pending bit can discriminate a changed interrupt state;
it cannot, by itself, identify one of these error causes.

Native kernel `516990fbb2ddb67bd89f9ea03316301f0629a767` checks `8f0` only
against `FFFFFFFF` in its requester wait and polls the actual completion
word. `m9_kms_irq_desc` and `m9x_irq_handler` consume low vblank only; probe
sets high mask zero. This is narrower than vendor notification coverage,
but no source-closed error recovery operation is missing on this evidence.
Reviewed source hashes:

| file | SHA256 |
| --- | --- |
| `m9_kms.c` | `272d2ea700058b3e8b62f1e0734e134b6ae638cee5ee22e34cddda8466a2c914` |
| `m9x_drv.c` | `fa66ec2149fa44cc6e1028834e7cd10c8bca83c0a6257baaf7047bc3537c3f48` |
| `m9_requester.c` | `999d0817a6f4c725ea7538b6096fcd6a04b3c8a0b3f1760c928d0d2667a9bb5d` |

## Bounded read-only observer

`tools/m9-faa-status-read-only.c` performs exactly eight volatile reads:
two sequential passes over `8f0/8f4/8e0/8e8`. All four have original vendor
read consumers above. It excludes ACK registers, high mask `8fc` (the
Windows consumer uses its cached mask), register selectors, translations
and unrelated display/MC banks. It requires exact PCI0540/rev1/subsystem22c0
and the caller's expected boot ID before opening BAR2 `O_RDONLY` and mapping
its first page `PROT_READ`.

The root hardware owner may use this once before submission and once after
actual retirement under the existing device locks. The phase argument is
caller labeling, not a completion test. Samples are sequential, not atomic.
Output retains raw words and only the proved optional drain mask, low
pending-and-mask, and invalid-device-read comparison. A still-set drain mask
would separate unfinished optional engine drain from frontend retirement.
A newly pending unnamed bit would preserve a useful boundary without
pretending to decode a fault. This is an observer, not a proposed GPU fix.

Built helper `/tmp/m9-faa-status-read-only` SHA256
`1797bb2c972bf814dac1305bb67e85ca8edfab36c6313ed4ef0318a92b3f8b7c`;
source SHA256
`e357495b4d9c65cdd3f96f75b92e00e641faf593955934ea851a0ec40d78761c`.
No hardware operation was performed by this agent.

```
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-queue-status-admission.py \
  captures/m9128-device-inputs-20261006/faa-queue-status-admission-public-red.json \
  --runtime-tools /home/ai/src/parhelia/xf86-video-parhelia/tools/faaemu
cc -std=c11 -O2 -Wall -Wextra -Werror tools/m9-faa-status-read-only.c -o /tmp/m9-faa-status-read-only
```

The capture producer is `tools/m9emu/probe-faa-queue-status-admission.py`.
Pass `--driver` the formal `m9-vendor-faa-read-contract.py`, `--flush-helper`
the scoped `m9_faa_retirement.py`, `--probe-out`, and the normal public-red
producer's exact runtime/bridge/scatter/PCI/DC arguments. It installs only
passive observers and the unchanged pre-read counter boundary. The actual
configuration, owner lifetimes, queue writes and independent footer remain
source-produced.

## Actual normal-render control, 2026-10-07

Root collected the guarded helper after a successful GL2 bump run and a
clean normal FAA-off run on boot6d6d22e6-4788-4491-a665-9b12701edd38.
The unmodified JSON and receipt are retained in
`tests/m9-faa-status-normal-20261007`. JSON SHA256 is
`74ec03f7620a54db2a7090246ad106145b4d8c5d6bd8076d012beaed8e57ccc5`;
the receipt verifies the helper SHA above and real submitted/completed84ac0,
raw84ac1, empty queue and wedged0. This is a NORMAL control, not a private
FAA result or a vendor cold-state observation.

Its first8f0 sample is01000000; second is0. Both have optional-drain30000
clear, high pending8f4=7100, rawlow8e0=5003fc6c and lowmask8e8=1, yielding
zero enabled low pending. Neither sample isffffffff. These raw pending words
are not attributed to address faults, invalid commands or FAA errors. No
before/after FAA comparison was part of that normal-control checkpoint.

## Actual FAA before/after observation, 2026-10-07

Root subsequently ran the unchanged516990f public-red candidate on fresh
bootd3f40c04-11a2-4807-8cf9-f5f99c3d03e2. The bounded observer reads8f0 twice
before submission (0,0) and twice after real140 retirement and the output
reader's post-wait kick (0,01000000). The optional-drain mask30000 is clear
in all four samples.8f4=7100,8e0=5003fc6c,8e8=1 match the normal control.
Actual FAA output is still black,0colored/0partial/8326bad interior pixels;
CPU positive mapping passes76800pixels and transport reports raw141,
empty queue and wedged0.

This observation finds no persistent optional-drain condition at those
points. It does not exclude transient conditions or decode unknown fault
bits. No new wait, acknowledgement, register toggle or shader change follows.
Complete source/artifact identities, logs and actual PPM are in the
[hardware archive](../tests/m9-faa-status-hardware-20261007/README.md).
All proof was copied before the mandatory recovery power cycle.
