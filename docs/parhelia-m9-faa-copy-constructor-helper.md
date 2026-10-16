# Bounded source execution of the PDEV conversion constructor

**The original `86660` constructor returns1 and publishes all three wrappers.**
Their `+234` initialization markers remain0. Both real `b1290` guard instructions
set the zero flag, so this result closes the NULL-wrapper prerequisite but does
not admit conversion or pixel readback. The proof is CPU/source-only. It supplies
no GPU receipts, shader outputs or opaque record interpretations.

## Minimal dependencies and genuine producers

This experiment deliberately bypasses full mode admission. It uses the retained
conditional M9128 CPU owners, the existing source-published PDEV `+24` index and
OS I/O handle, and live board properties. It does not complete `2dea0`,
`2ff50`, or the public mode caller. It cannot reuse the old FAA prefix or its
hardware receipt; a caller using this earlier initialization needs a fresh whole
source capture and physical qualification.

The format inputs are sufficient for this constructor and its later preparation:

- Original `105f0` reads a controlled DEVMODE1024×768/32bpp60Hz and produces
  request format code`0x40` (decimal64) at `1067b`. Execution stops at its
  genuine `2dea0` call before a second PDEV initializes. No mode reply is supplied.
- The helper executes the source format branch `300ca..300f9` with that actual
  request code in EBX. It follows `300cf→300e5`, stores32 storage bits at
  `300ed` (`+74`), and derives4 bytes at `300f6` (`+78`). The dead local load
  at `300ea` reads a bounded host CPU frame; its value is not published at the
  stopped boundary. No selected mode dimensions or index are injected.
- The helper executes `30151..3025b` with the same code. Its `3015a` branch
  enters the original immutable publisher `3024a`, storing numeric format
  **`0x66` (decimal102)** to `+80/+84` at `3024f/30255`. Earlier notes' bare
  `66` used hexadecimal driver notation; it is not decimal66 or a pointer.

The original constructor reads actual PDEV `+8/+20/+24/+28/+80`, hardware owners,
and live profile `+184`. Sixty distinct owner/offset/instruction reads are retained
in the proof. The observed properties `0x350207` select the original policies;
there are no forced policy bits. `86660` emits `23212c` with actual input
`[0x2d,0]`. The helper forwards it through real MM `6c755` dispatch, enforces
its8-byte ABI, and propagates only actual CPU stores to shared owners.

MM `70f47` calls `71428`. The two output DWORDs `[0,1]` are fully attributed to
**`71462/71464`**, and the real returned-byte count is8. They are constructor
policy data, not completion. A missing property can take the source's zero-value
branch at `71453`; the helper retains that result rather than inventing a query
reply. Both bytes and source writers are independently checked before returning
this output to MD.

## Actual owners and consumer guards

The constructor executes three real `8adc0` backing constructors and three
`868d0` wrapper constructors. The observed backing modes are `[2,1,1]`:
slot0 owns a source-allocated`0x4270` backing; slots1/2 use retained hardware
storage at `hardware+0x3618+slot*0x4270`. The three source-allocated wrappers are
`0x3ec` bytes, distinct, and published at d0/d4/d8. Every complete wrapper and
backing fits its retained allocation; these are live owner-relative addresses,
not replayed capture addresses. This execution emits no device MMIO or system-DMA
requests.

Original `868dd` initializes each wrapper's `+234` marker to0. The helper then
executes the two **read-only** consumer comparisons `b135a` and `b1369` with the
actual d4/d8 objects. Both set ZF. It stops before the conditional cleanup branch:
this fixture has no original staging/copy job whose cleanup could be continued.
No whole `b1290`, conversion, mapper or pixel return is claimed.

The next genuine method is `86a60`, which consumes backing state and source
PDEV `+78/+80`, calls the backing's vtable`+28` method, then `48a40` and
`48950`. It must be entered with its real caller arguments and successful source
resource ownership. Constructor success and non-NULL wrappers do not replace
that lifecycle or permit writing a nonzero marker.

## Reproduction

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9-vendor-faa-copy-constructor-helper.py \
  --out /tmp/m9-faa-copy-constructor-helper.json
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-copy-constructor-helper.py \
  --proof docs/traces/m9-faa-copy-constructor-helper-20261007.json
```

The checker verifies the entire shipping image hashes, eleven existing constructor
spans plus six new decoded branch/query/preparation spans, implementation and
actual PCI artifact hashes, four original format stores, both reply writers,
three backing/wrapper calls and owners, and both consumer guards. Nine negative
controls reject full-mode/old-receipt/ready claims, altered format or reply
attribution, truncated backing extents, and a changed guard result. The earlier
strict failed mode-query and head-control proofs remain separate and unchanged.
