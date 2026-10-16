# M9 first clean-room no-op timeout

The first guarded kernel submission timed out because the completion record and
the CPU poll referred to different writeback words.  The saved Windows miniport
closes this association exactly: `MxMM.sys` `0x21f50..0x21f82` allocates the
four-byte object at `object+0x30`, writes its GPU address from `object+0x34` to
MMIO `0x106c`, and clears the dword through `object+0x30`.  Synchronous helper
`0x22533..0x225c2` emits `{4545e3e6,3,target|1}`, posts it, kicks `0x314`, then
polls that same `object+0x30` dword [BIN].

The timed-out open-driver revision instead programmed:

```text
0x1068 = fence_bus     | 2
0x106c = (fence_bus+4) | 2
poll     fence_cpu[0]
```

Therefore a successfully executed record updates `fence_cpu[1]` while the
driver waits on `fence_cpu[0]`.  A 100 ms timeout with a responsive host is the
expected symptom.  Linux supplies an independent join: `m9x604` at
`0x2de9..0x2df7` explicitly emits index `0x1b` (MMIO `0x106c`) with its fence
address before the `e6/e3` completion tail.  Captured ioquake3 command streams
have the same form, for example
`captures/m9x-linux-ioq3-texture-20260914/decoded-724.txt:152` [BIN][CAP].

## Correct first repair

Bind the word being polled to `0x106c`.  The least invasive arrangement is:

```text
completion = fence_bus
secondary  = fence_bus + 4

0x1068 = secondary  | 2
0x106c = completion | 2
poll     fence_cpu[0]
```

Before another submission, initialize the two words to different sentinels and
log both on timeout.  Also read back `0x1068` and `0x106c`.  If only word 0
reaches `target|1`, the mapping is proven on the new stack.  If an unchanged
binary is tested once for diagnosis, word 1 reaching the target proves the same
fault without changing any engine state.  Do not make success depend on word 1
after applying the repair.

The expanded vendor footer is a useful second diagnostic after the minimal
record works:

```text
4545451b  completion | 2
45e3e6e6  3  2  target|1
45454543  target
```

It binds index `0x1b` in-stream, but it is unnecessary for the minimal record
when `0x106c` has already been programmed correctly.

## What was already correct

The record has exactly three dwords.  `0x404` is the untagged exclusive end, so
`command_bus + 12` is correct and the fourth `0x45454545` dword is padding
outside the submitted interval.  `0x400` and the fence address use low tag 2;
`0x404` does not.  Writing `0` to `0x31c`, posting `0x404`, kicking `0x314`, and
masking the returned low nibble follow the two saved vendor implementations.

Probe clears PCI bus mastering and `m9_transport_arm()` re-enables it after
programming `0x400`, but before any `0x404` doorbell.  Since `0x400` only arms a
base and command fetch starts at `0x404`, this order does not explain the
timeout.  Moving `pci_set_master()` before requester programming and checking
PCI_COMMAND bit 2 makes the invariant clearer, but it should be treated as a
cleanup rather than the root fix.

The interrupt masks are also unrelated to this completion.  Both vendors poll
coherent memory and use `0x314` as a progress kick; no completion IRQ is needed
for this test.

## Remaining prerequisites and bounded diagnosis

The reduced open path does not yet own the complete Linux `m9x109` register
set.  The vendor additionally writes `0x10ec`, `0x1094`, `0x1098`, and
`0x44c/0x450/0x454`.  The last three controls now have an exact property
packer and fallback image in `parhelia-m9-primary-requester-controls-re.md`;
only the target's seven property inputs remain unjoined.  The optional context
resources and general vendor engine initialization also own the address/service
state at `0x330..0x364`, `0xf04/0xf0c`, `0xf64..0xf7c`, and
`0x1320..0x1334`.  These are real implementation gaps, but none should be
filled with copied runtime constants for this retry.  The tested device was
BIOS POSTed, and the open probe preserves those registers.  First correct the
proved `0x106c` association.

On the next bounded attempt, snapshot these values before arm and at timeout:

```text
PCI_COMMAND
0314 031c 0400 0404 0448 044c 0450 0454
08f0 1068 106c 1094 1098 109c 10ec
fence_cpu[0] fence_cpu[1]
```

Interpretation is deterministic:

* word 0 reaches the target: the first transport test passed;
* word 1 reaches the target: the old channel mismatch remains;
* neither changes and `0x400/0x404/0x106c` read back correctly: next compare
  the saved prerequisite registers and `0x8f0 & 0x00030000`;
* `0x8f0 == 0xffffffff`: fail with device removal/unreachable MMIO;
* bus-master bit is clear: stop without ringing another doorbell.

Keep the 100 ms bound.  Do not initialize the dormant `0x500..0x524`
auxiliary requester, raise the timeout, or add opaque context values to make
this one record pass.

## Reproduction

From the `xf86-video-parhelia` root:

```sh
python3 tools/m9-first-noop-timeout-evidence.py
python3 tools/m9-fence-record-evidence.py
python3 tools/m9-requester-drm-contract-test.py
```

The first verifier pins both vendor images by SHA-256, the Windows
allocation/bind/poll RVAs, the Linux `0x106c` packet binding, and the reduced
record.  When the adjacent Linux source tree exists it also reports whether
the known `0x106c`/polled-word mismatch is present or repaired.
