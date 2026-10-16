# M9148 kernel service initialization capture

For the current old kernel, which lacks KGDB, use the
[current initialization tracing harness](parhelia-m9-cold-init-tracing.md):
a private instrumented vendor module plus X MMIO/PCI and port-I/O observers.
The KGDB tool below remains an alternative for a suitable debug kernel.

The missing runtime evidence is the interval from initial service construction
through the first primary doorbell. Existing client `ExecuteDma` captures
begin after the X server has constructed global vendor services. They do not
establish the initial register-write chronology.

`tools/m9x-cold-service-kgdb.py` collects this interval on the preserved vendor
Linux installation. It needs a KGDB connection and a **host GDB with Python**.
The currently installed host `/usr/bin/gdb` was checked on 2026-09-15 and is
built `--without-python`; it cannot run this script. The observer has offline
tests, but has not been exercised through KGDB on the target.

A private GDB 17.2 with Python was subsequently built from the cached
`/srv/t2/download/mirror/g/gdb-17.2.tar.xz` into `/tmp/m9-gdb` on the host.
`/tmp/m9-gdb/bin/gdb -nx -batch -ex 'python import gdb; print(gdb.VERSION)'`
passes. This resolves the host debugger dependency. The preserved target's
2.6.30 and 2.6.34.1 configurations both disable KGDB, kprobes and mmiotrace,
so this KGDB observer still needs a suitable target kernel/connection.

This is a kernel service trace. It does not capture the X driver's earlier
board POST, PCI bus-master enable, or direct userspace MMIO accesses. Those
still require a separate X-side observer if full cold-boot parity is needed.

## Exact register writer

The canonical `KernelLibraries.o_shipped` SHA-256 is
`bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`.
The hardware accessor constructor `m9x798` installs `m9x748+0x10`, a vtable
pointer at `.rodata+0x70`. Its effective `+0x78` method is the relocation at
`.rodata+0xe8`, `m9x1019` at `.text+0x55f0`:

```text
48 8b 47 28 55 89 f6 48 89 e5 89 14 30 c9 c3
MMIO base = *(u64 *)(rdi + 0x28)
offset = (u32)esi
value  = (u32)edx
```

The observer stops at function entry, before the write, and records the caller
return address from the stack. Thus a zero register readback cannot hide an
actual write. `m9x1147` is a different vtable slot and must not be substituted.

## Setup

Boot the preserved vendor OS. Load the vendor module and its relocated symbols
into the host debugger before starting X or any operation that creates the
render service. Use the actual final linked module's section addresses; the
section offsets in the proprietary object are not module load offsets.

Supply a new directory on the **debugger host**, and the canonical object:

```gdb
set $m9_cold_output = "/host/captures/m9-service-NEW"
set $m9_cold_image = "/host/parhelia/xf86-video-parhelia/m9xdriver-x86_64-1.4.3-80847-20140908-build_98/kernel/KernelLibraries.o_shipped"
set $m9_cold_vram = 0
set $m9_cold_stop_class1 = 1
source /host/parhelia/xf86-video-parhelia/tools/m9x-cold-service-kgdb.py
continue
```

`$m9_cold_stop_class1 = 1` records and continues past earlier doorbells,
then pauses before the first doorbell following construction of a native
class-1 CALL. This is the boundary needed for the current translated-fetch
failure. Set it to zero (the default when unset) to stop before the very
first doorbell instead. The selected boundary is recorded in the manifest.

For compact-table snapshots, replace zero with a **verified kernel virtual
mapping of BAR0 offset zero**, covering at least `[0,0x941000)`. A physical BAR
address, MMIO mapping, userspace mapping, or unverified direct-map offset is
not sufficient. If no such mapping is available, leave zero: event and
allocation collection still works, and every omitted snapshot is explicitly
marked unavailable. The observer does not create mappings or call target
functions.

The complete local object hash and relocation-free live instruction signatures
are checked before any breakpoint is installed. Code or symbol mismatches
abort setup. A runtime vtable check guards the MMIO accessor fields.

## Collected evidence

`events.jsonl` contains a monotonically numbered event stream, debugger host
timestamps, and target thread IDs:

- entry of the anonymous service initializer at `m9x1187+0x40`
  (`.text+0x121d0`), followed by entry/return of `m9x1172`, `m9x53`,
  `m9x109`, `m9x926`, and `m9x115`;
- every `m9x1019` 32-bit write after that initializer entry, including its
  `f04`, `344..354`, `334..340`, and `330` writes preceding `m9x1172`;
- each `m9x887` mapping input, with its 0x48-byte descriptor, 0x80-byte
  allocation resource, GPUVA and extent, plus a post-publication table image;
- each `m9x535` publication-gate input, including allocation mode, the raw
  `System\\MmFlags` wrapper image and its actual getter method address;
- the four `m9x926` allocation returns at offsets `+0xed`, `+0x1b2`,
  `+0x277`, `+0x33b`, including success, allocation mode, request mode,
  complete 0x2b0-byte request, resource, GPUVA and extent;
- optional `[0x801000,0x941000)` table images at phase boundaries;
- each native CALL at `m9x830+0x290` (`.text+0xcf40`), after its third
  dword was stored: the actual three primary words, their CPU address,
  BmBuffer pointer, `+0x34` class, `+0x40` extent, `+0x44` GPUVA,
  `+0x48` 64-bit raw cookie, and a complete 0xb8-byte BmBuffer image;
- every pre-`0x404` boundary, with the CALLs constructed since the previous
  doorbell for the same primary builder, a table snapshot, and explicit read-only register observations
  covering `30c/314/31c/360/364/3a8`, requester state, raw IRQ status and
  masks, `f04/f0c/f64/f68/f74/f78/f7c`, `1068/106c`, and `132c..1334`;
- a terminal marker before the selected doorbell write.

The CALL breakpoint's live instruction signature is checked along with the
entry signatures. At this point RAX is the primary record address and
`*(u64 *)(rbp-0x58)` is the actual BmBuffer; these are taken from the
canonical `m9x830` disassembly, not inferred from the ioctl's returned next
buffer. The packet address must also equal the cursor at `RBX+8`. Each call's
event sequence number joins it to the doorbell record. At the doorbell,
the caller must be `m9x1039+0xf2`; this proves that RBX still holds the
submitting builder. Only that builder's pending CALLs are attached and
consumed. Construction thread IDs are retained without assuming that all
operations on a shared builder execute in the same thread.
This join records CPU construction order; it does not establish that every
record was fetched by the GPU.

For class-1 CALLs, a supplied BAR mapping also permits explicit page-entry
reads at the doorbell boundary. Using the observed `f74` and `360` origins:

```text
leaf_slot = MMIO[f74] + 4 * (CALL_address >> 12)
root_slot = (MMIO[360] & 0xfffff000) + 4 * (leaf_slot >> 12)
```

All pages in the actual half-open CALL range are recorded, up to 256 pages.
The event labels this as the selector-zero compact-table derivation; it
preserves raw values and does not assume that the entries are valid. Root
control bits `360 & 0xc` and equality of the two leaf origins are recorded.
For local root entries with nibble 8, a second leaf observation follows the
root's encoded backing page; the direct logical-slot observation is retained
as well. This exposes a nonidentity root instead of silently assuming one.
Other root flag combinations remain raw evidence, marked unavailable for
this local-root interpretation. An
empty/wrapped range, missing BAR mapping, or a derived offset outside the
verified table interval is explicitly marked unavailable. Full table images
remain available for an independent decode. Register readbacks are kept
separate from the pending doorbell's value and ordered write events: a
write-only requester register reading zero is not evidence that its earlier
write was lost. The observer never acknowledges IRQs or writes MMIO itself.

The allocation data is recorded even if its mapping-publication branch is
skipped. Do not infer PTE writes or `0x31c` triplets merely from the four
allocation sizes: correlate the actual `m9x887` events and register writes.
In particular, the memory-manager property at device `+0x640` is distinct
from the hardware capability word `0x300004`. The usual `MmFlags=0x100e8`
has the `0x8`/`0x40` bits tested by `m9x535`; substituting the hardware word
would incorrectly predict that this publication branch is skipped.
Raw request/resource images preserve fields whose meaning is still uncertain.
The observer does not claim that a GPUVA is a physical backing address.

Every artifact and JSON event is flushed with `fsync`; binary artifacts carry
sizes and SHA-256 values in the event stream. Limits are 2048 events and 64
table images. Errors, bounds, a second service constructor, or an unexpected
accessor disable this observer's breakpoints and mark the capture incomplete.
The selected `0x404` also disables them and **leaves the target paused before
that write**. Inspect the artifacts before continuing the vendor operation.

Debugger pauses change timing. This is evidence of CPU ordering, allocation
results and table contents; it is not a performance measurement or proof of
GPU execution. The selected doorbell has not executed at the terminal marker.
With class-1 stopping enabled, earlier doorbells may already have executed.

## Offline checks

```sh
python3 tools/test_m9_cold_service_observer.py -v
```

The tests use the real vendor instruction bytes with a simulated GDB memory
interface. They check rejection of mismatched code and accessor vtables,
preservation of `0x31c` write order, stopping before the first doorbell, and
allocation field/artifact collection. They also check traversal of earlier
doorbells, the actual BmBuffer/packet join, malformed CALL rejection, and
page-entry decoding with deliberately different live origins and an exclusive
page-aligned range end. They do not test KGDB availability,
GDB stack unwinding, or the hardware.
