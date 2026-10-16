# M9 high pending bit 25: static boundary

Audit date: 2026-09-15. This concerns **MMIO `0x8f4` bit 25**
(`0x02000000`), not the independently observed changing bit 24 of `0x8f0`.

The saved binaries do not identify this bit as a translation, DMA, or 3D
fault. They do show that Windows includes it in its explicit high-pending
clear mask. That makes the bit worth capturing across initialization and the
first successful submission; it does not establish what caused it.

## Exact binary evidence

Artifacts and hashes are the same canonical Linux object and Windows
`MxMM.sys` pinned by [the non-vblank IRQ audit](parhelia-m9-nonvblank-irq-boundary.md).
Linux offsets below are `.text` offsets; Windows offsets are image VAs.

* Linux `m9x547`, `0x3fd90..0x3fea5`, supplies `0x8f4` to the MMIO reader
  at `0x3fdcb`. It returns the complete raw word through its first output.
  At `0x3fe64..0x3fe7f` it ANDs that word with the cached high enable mask at
  device `+0x8c` and acknowledges the resulting mask through `0x8f8`.
  There is no test specifically for bit 25. The subsequent core handler
  `m9x1012` dispatches only the four already documented low-bank vblank bits.
* Windows ISR `0x10875..0x10f02` reads `0x8f4` at `0x108fd`. Its high-bank
  consumers test `0x40000`, `0x10000`, `0x20000`, `0x8`, `0x4`, and
  `0x01000000`; bit 31 gates the low-bank decoder. There is no bit-25
  consumer. In particular `0x10e1b` loads **`0x01000000`**, then builds
  selector 12; this is not `0x02000000`.
* Windows controller reset `0x10f22..0x10f88` clears high enable `0x8fc`
  at `0x10f3c`, its software cache at `0x10f45`, and low enable `0x8e8`
  at `0x10f56`. At **`0x10f67` it writes `0x032f7f1f` to `0x8f8`**.
  That explicit W1C mask includes bit 25. The instruction bytes are
  `c7 81 f8 08 00 00 1f 7f 2f 03`.
* A second Windows read at `0x12eb9` copies the whole raw `0x8f4` word into
  a diagnostic output structure at `+0x104`. The preceding read at
  `0x12ea4` copies `0x8f0` into `+0x100`; this is a dump, not fault decoding.
  Other immediate `0x02000000` uses nearby operate on the **indexed-register
  access control `0x448`**, not on high pending status.

Searching the disassembly for immediate `0x8f4` yields the one Linux reader
and the two Windows readers above. This is an immediate-reference audit;
it does not claim to exclude every possible computed-address access.

The two existing byte-level evidence tools passed again for these exact
artifacts:

```
python3 tools/m9-nonvblank-irq-evidence.py
python3 tools/m9-irq-selector-evidence.py
```

## Do not mislabel translation configuration as fault status

Linux references to `0x132c`, `0x1330`, and `0x1334` are MMIO **writes**
through accessor slot `+0x78` in the recovered translation paths:

* `m9x22`: register selectors at `0x1628f`, `0x162dc`, `0x162f9`, followed
  by write calls at `0x162ce`, `0x162eb`, `0x16301`;
* `m9x421`: selectors at `0x16a3e`, `0x16a6f`, `0x16a84`, followed by write
  calls at `0x16a62`, `0x16a77`, `0x16a8c`.

Values are derived from translation-table bases and page-rounded ranges.
These references provide no fault-address or fault-reason decoder. Zero
readback of these registers cannot, by itself, diagnose the class-1 stall.

## Capture implication

A raw bit set after a failed cleanroom submission but absent before vendor
module loading compares different lifecycle phases. Capture raw `0x8f4`,
high enable `0x8fc`, and all `0x8f8` writes at least before initialization,
after requester arm, immediately before the first `0x404` publication, and
after its completion. An initialization W1C can clear an earlier occurrence.
If successful vendor submission also leaves bit 25 set, it is not sufficient
evidence of the failure. If only failure sets it, that is useful correlation
but still not an identified fault cause. No new interrupt enabling or
acknowledgement is justified solely by this static audit.
