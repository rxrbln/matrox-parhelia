# Ordinary FAA SYSTEM output admission audit

This bounded independent audit found no concrete output-owner domain, prefix,
metadata or lifetime mismatch in kernel `c70c607` and the actual constant-red
source manifest. It does not prove the GPU's output-store translation or decode
the low PTE bits as a generic write-permission ABI.

The source is the whole public red control, canonical RE `19b85d31`, with
physical evidence `582068a6`. The source manifest is
`captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-input-manifest-101f8.json`.
The source `output_destination.binding` retains the real `5b750` descriptor,
80 PFNs owned by the returned NT MDL, the compact records, and CPU-known
metadata. Its flags are `30`, mapped and allocated extent both `50000`,
tag one, compact low nibble `b`, companion metadata `[0,80,0]`. The output
resource's kind is one, logical format six, profile class two, hardware format
`64`, requested `320×240`, padded `320×256`. The full backing extent is
`320*256*4 = 50000`; there is no promoted unknown allocation tail.

These are **MD** instructions in `MxMD.dll`, SHA256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`,
not same-numbered instructions in `MxMM.sys`. The original `5b9c2..5ba44`
scatter branch SHA256 is
`06bc6f8f9a9f94f1af13c8bcf5d13cc1955d9a19161f8999ac25d02b0a02ba2b`.
It derives the low flags from the actual descriptor, uses the MDL PFNs,
and writes all four DWORDs per record at `5ba25/27/2d/33`. In the real output
call, the three stack-derived metadata words are known by prior guest CPU
writes and equal zero, 80, zero; they are not assumed zero from sparse table
initialization. The source `source-101f8.json` last scatter call retains this
exact producer and its prior writers.

The only nonnull emitted reference to output owner 27 is kind two at primary
offset `1410` (decimal 5136), encoder `24930`, tag one, zero owner displacement.
Its actual descriptor is `[output_gpu|1,c8210140,100f0]`. This is in the final
488-byte primary resolve block, separately from the two counted secondary
intervals. Native regeneration of 20 companion-clear words shifts that output
slot by four bytes to decimal 5140 and expands the primary from 5596 to 5600.
The final resolve remains inside the actual posted primary range. There is no
source-to-native conversion to a tag-two physical output address.

Native `m9_faa_allocate()` routes this typed SYSTEM/flags30 owner through
`m9x_address_map_faa_system()`, retains its full 80-page node, and uses the
actual flags30/WC preparation. `m9_address_binding_pte()` derives each packed
leaf from the retained DMA owner and adds the admitted flags30 bit. Before
publication, `m9_address_publish_binding()` checks all compact leaves;
`m9_address_secondary_publish()` copies those leaves, emits `[0,N,0]`, checks
every produced word, and only then publishes the secondary post-list. The
compact/secondary sequence is `31c=f,0,1`, then `5,0,2`; the mode-one finish
also emits `314=1`. A staged owner before manager construction is committed
after construction and service finalization, rather than lost in the initial
table clears.

The real red log supplies output GPUVA `4007c000`, primary GPUVA `400cc000`,
posted end `400cd5e0`, and real point `140`. Output's interval is
`[4007c000,400cc000)`; the following primary starts exactly at its end.
The logged live roots `401000/505000/915000` derive output compact cells
`5011f0..50132c`, companion records `9057c0..905cbc`, and containing-storage
root cells `916404` and `917414`, whose identity values would be `501008`
and `905008`. These are explanatory calculations from this live allocation,
not fields to replay on another boot. Both complete output ranges fit the
coherent primary/secondary extents and do not overlap the CPU root child.

The owner stays alive through genuine retirement and the read operation;
teardown unmaps after stopping PCI bus mastering. The raw retired DMA observer
reports all 327680 output bytes unchanged and zero. That excludes a wrong CPU
pixel-coordinate view as the explanation for these particular bytes, but does
not prove the GPU ever attempted the output store.

The remaining precise observational gap is the lack of retained before/after
**hardware** snapshots of output's compact leaves, companion records and
containing-storage root entries. Native CPU publication checks establish the
pre-post grammar, not an immutable table through execution or the undocumented
GPU store consumer's selected bank. A future bounded observer can derive and
read those cells from the retained live node and binding, without altering any
mapping, introducing initialization or manufacturing completion. No such
target observation or native change was performed by this audit. FAA ownership
and follow-up now belong to the render agent; performance is a separate track.
