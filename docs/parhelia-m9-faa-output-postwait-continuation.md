# Original FAA output CPU-copy continuation

The original internal `ac1f0` helper now returns **1** when its interrupted
CPU stack is continued with explicitly translated inputs from the qualified
public-red hardware test. It reads the observed black ordinary output and
releases its temporary. No second resolve, new command-buffer words, or
backend-status register reads occur after the former `4e2fc` boundary.
This closes omitted command-buffer generation or backend-status polling for
this bounded continuation. The real post-wait MMIO314 kick is still a native
ordering difference, and its possible hardware effects are unqualified.
Supplying archived zero pixels after that emulated write cannot prove the
same pixels would remain zero after executing the write on hardware.
This does not fix FAA or establish the unjoined public Lock admission.

The [hardware log](../tests/m9-faa-prefix-hardware-20261007/proof/dmesg-after.txt)
has distinct BEFORE and RETIRED observations at lines756 and818. The RETIRED
line supplies common `+00=141/0` and `+20/+40/+60=10/0`, after real point140.
The same log records all327680 ordinary-output bytes as zero, with
`nonzero_bytes=0` and FNV`d3209dc5`. These are observed inputs; the continuation
does not derive them from the CPU seed or request a successful GPU reply.
The archived strict store report joins that log to the whole public-red
source plan. The continuation requires identical source typed operands,
mapping policies, shader/state input images and all pre-flush MDL spans.

The original CPU context and entire guest stack are saved before the actual
first read. The strict source caller still stops normally. A separate genuine
`23b80` call appends and posts its real footer, stopping before `23bc3` reads
a counter; its footer bytes and MMIO match the independent public-red
baseline. The original context and stack are restored, with that real queued
work retained. There is no assigned helper return or changed vendor code.

The receipt shim admits only the four observed8-byte common-counter pairs
through the unique live159-byte source common-buffer owner, and the observed
zero pixels through the actual kind1/flags30 output's80 owned PFNs, compact
PTE`b` mapping and CPU alias. The real source desired timeline and native
receipt both equal140, so no invented point bias is applied. Every other
common-buffer offset remains unsupported. Existing strict aperture/MMIO
hooks also remain active: an additional unknown GPU read stops the control.
Receipt bytes are explicitly classified as hardware inputs, never as
source-known CPU initialization or emulated GPU execution.

The observed continuation is:

```
4e250: query common+20 twice; 4e31c calls66950 CPU deadline bookkeeping
4e130: write common+00 twice; 4e157/4e1f2 call66970/669b0
2cba8: MMIO314=1
8b390: CPU copy, actual read1e5383, 76800 DWORDs/307200 bytes
2c840: release CPU mapping access
ac3ef: temporary release, repeats the same query/write waits
19dd0: real EngDeviceIoControl233070, input8/output1
7190e -> 71a88: actual MDL unmap/free/descriptor release
ac3fd: returns1 to the original caller's RET_MAGIC
```

The CPU formatter reads only307200 image bytes within the327680-byte mapped
extent; the retained address builder includes padding in that extent. Its
output is the observed zero307200-byte image. All34 other live MDL buffers
retain identical whole-buffer hashes, and the write observer sees no writes
to those buffers. There is one frontend kick before CPU copy, then the real
resource-unmapping `31c=f,0,1,5,0,2` sequence. There are no MMIO reads and no
additional `abc20`, queue finish, program or rectangle-trigger calls in the
continuation. The frontend kick is a concrete source transition, but this
proof establishes no rendering-cause relationship for it.

The actual `aff90` caller passes `2cb40(temporary,0,1)` at `b012d..b0132`.
The literal last argument1 forces the `2cb81` branch: query wrapper555c0
calls4e250 and must return0, then write wrapper55550 calls4e130 and must
return0. `2cb9f..2cba8` writes314=1
through the actual resource/hardware MMIO owner. This occurs before the
allocation-kind dispatch at2cc33, so it applies to this observed kind1
output. Alias selection and CPU serialization follow. The private native
reader currently performs `rmb`, `dma_rmb` and copies the retained DMA alias
after its retirement gate; it lacks this post-wait kick. A prior submission
or allocation kick is a distinct ordering event. Reproducing this genuine
post-map transition behind the existing private owner/retirement guards is
a source-faithful candidate; correct pixels still require a fresh hardware
test. This control itself performs no physical MMIO or kernel change.

The existing allocation bridge rejects233070. This control adds a narrowly
typed release bridge for the already-retained ordinary output only: the
actual input must be its CPU alias and MDL; the real shipping MM handler
executes the existing OS MDL host's unmap/free calls. It returns genuine0 and
a returned-byte count1. The handler does not initialize its output byte, so
the bridge never reads or copies that byte. No GPU status is supplied by this
OS cleanup bridge. Other I/O remains on the unchanged strict bridge.

The [proof](../captures/m9128-device-inputs-20261006/normal-faa-output-receipt-continuation-public-red.json)
retains counter reads, live PFN joins, actual calls, buffer hashes, CPU-copy
statistics, release calls and exact MD/MM instruction bytes. Its
[pre-boundary manifest](../captures/m9128-device-inputs-20261006/normal-faa-output-receipt-continuation-pre-inputs-public-red.json)
keeps the source-only state separate. The accompanying contract still says
the original strict call is unfinished; it is not silently promoted to a
GPU-successful source capture. The continuation proof carries the explicit
hardware-input classification and actual original-call return.

Reproduce from the RE root with the established Unicorn/pefile interpreter:

```
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/m9_faa_output_receipt_continuation.py \
  --contract-driver tools/m9-vendor-faa-read-contract.py \
  --receipt-log tests/m9-faa-prefix-hardware-20261007/proof/dmesg-after.txt \
  --receipt-plan captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-native-plan.json \
  --hardware-store-report tests/m9-faa-prefix-hardware-20261007/retained-stores.json \
  --baseline-manifest captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-input-manifest-101f8.json \
  --pre-boundary-manifest-out captures/m9128-device-inputs-20261006/normal-faa-output-receipt-continuation-pre-inputs-public-red.json \
  --continuation-out captures/m9128-device-inputs-20261006/normal-faa-output-receipt-continuation-public-red.json \
  --public-constant-red --runtime-tools tools/faaemu --bridge-tools tools/m9emu --scatter-tools tools/m9emu \
  --pci-inventory captures/m9128-device-inputs-20261006/pci-inventory/inventory.json \
  --pci-resources captures/m9128-device-inputs-20261006/pci-resources.txt \
  --dc-snapshot captures/m9128-device-inputs-20261006/dc-banks-idle-e4611b75.csv \
  --out captures/m9128-device-inputs-20261006/normal-faa-output-receipt-continuation-contract-public-red.json \
  --source-out captures/m9128-device-inputs-20261006/normal-faa-output-receipt-continuation-source-public-red.json
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-output-receipt-continuation.py \
  captures/m9128-device-inputs-20261006/normal-faa-output-receipt-continuation-public-red.json
```

This control is local source interpretation, consumes archived receipts,
performs no hardware access and proposes no GPU-state patch.
