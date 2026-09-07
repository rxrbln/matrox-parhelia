# PCIe P690 (0x2539) versus PCI-X Parhelia-512 (0x0528)

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Measured on box 56 after swapping the PCI-X 0x0528 for a PCIe x16 P690.  This
records what actually differs, because the caveat raised at the start of this
work -- that captures are bus-type specific -- turns out to be justified in at
least one functional respect.

## 1. Bus and resource layout **[HW]**

| | 0x0528 Parhelia-512 | 0x2539 P690 |
|---|---|---|
| slot | 0000:06:00.0 | 0000:03:00.0 |
| bus | PCI-X | PCIe x16, Express v2 Endpoint, 2.5GT/s |
| BAR0 (framebuffer) | 0xd0000000, **256 MB**, 32-bit prefetchable | 0xf0000000, **128 MB**, **64-bit** prefetchable |
| MMIO BAR | **index 1**, 8 KB | **index 2**, 8 KB (BAR0 being 64-bit consumes slots 0 and 1) |
| MSI | not presented | present, Count=1/4, Maskable |
| MaxPayload / MaxReadReq | n/a | 128 / 128 bytes |
| subsystem | 0x102b:0x14e1 | 0x102b:0x0040 |

Consequences for the tools:

- `p3dtri` resolves the MMIO BAR by scanning, and correctly reports
  `BAR indices: fb=0 mmio=2` on the P690.  Anything that assumes BAR1 is MMIO
  is wrong on this card.
- **The framebuffer aperture is half the size.**  `--rtoff 0x08000000` is
  exactly 128 MB and therefore lands one byte past the end of P690 VRAM; the
  old `--physbuf 0xd8000000` is a different card's address entirely.  Both must
  be re-chosen inside 0xf0000000..0xf7ffffff.

## 2. The vendor driver applies a different errata set **[HW]** **[RE]**

Same driver, same kernel, only the card swapped:

    0x0528:  [mtx] Parhelia patches applied: PowerM EOBTag Cap66Mhz CompBypass
    0x2539:  [mtx] Parhelia patches applied: PowerM Cap66Mhz CompBypass

**EOBTag is applied to the 0x0528 and not to the P690.**  This is not a
configuration accident -- the decision is structural.  In `mtx_parhelia.o`,
function `mtx3376` selects the patch set (strings at `.rodata.str1.1+0x00`
"PowerM ", `+0x08` "EOBTag ", `+0x10` "Cap66Mhz ", `+0x1a` "CompBypass "):

    c06:  cmp  r13d, 0x528102b        ; device+vendor
    c0d:  je   e20                    ; 0x0528 has its own path
    c13:  cmp  r13d, 0x527102b
    c1a:  sete dl
    c1d:  cmp  DWORD PTR [rsp], 0x3051106   ; host bridge = VIA 1106:0305
    c24:  sete al
    c27:  test al, dl
    c29:  je   c3f                    ; skip EOBTag unless BOTH
    c2b:  mov  rsi, <"EOBTag ">
    c35:  or   BYTE PTR [rsp+0x8], 0x40
    ...
    e20:  test BYTE PTR [rsp+0x4], 0x2 ; 0x0528: gated on a bus flag
    e25:  jne  c2b                    ; -> apply EOBTag
    e30:  jmp  c13                    ; else fall into the generic path

So EOBTag is reachable **only** for a 0x0527 sitting behind a specific VIA host
bridge, or for a 0x0528 when a bus flag bit is set.  A 0x2539 can never reach
it.  `Cap66Mhz` conversely is applied to anything that is not a 0x0527, which
is why it appears on both cards.

The surrounding code reads an MMIO register at `0x300`, masks it
(`and ecx, 0xe0e0ff3f`, then ORs in fields derived from the device id) and
writes `0x308`; the neighbouring path touches `0x384`, which is the DMAPAD
write-combining pad register already documented here.  That places this whole
block in the bus-master configuration group, and EOBTag sets bit 6 of the byte
at `+0x8` of the value being assembled.

**What EOBTag actually does to the hardware is NOT established.**  The name and
its neighbourhood suggest end-of-buffer tagging for DMA command buffers, which
would put it adjacent to the fence pair `0x12BC`/`0x41C`.  That is inference,
not a finding.

## 3. What this invalidates, and what it does not

Confirmed unchanged on the P690 **[HW]**: the engine register file responds
identically at `0x0000`, `0x450`, `0x8f0` (idle = `0x00020000`), `0x1300`, and
`0x400`/`0x404` are writable with the vendor X driver's own ring addresses
visible in them.

**Explicitly not carried over:** the `0x404 == end` block-load framing result
was obtained on the 0x0528, which runs with EOBTag applied.  Since EOBTag sits
in the bus-master configuration path and the P690 does not get it, that result
must be re-measured on this card before it is relied on here.  It is a result
about the 0x0528, not about "Parhelia".

Any future capture or hardware claim in this repository should name the device
id it was taken on.

## 4. GART / AGP aperture **[HW]**

The test board carries **two** host bridges: an nVidia nForce3 250Gb (AGP) and
an ALi M1695 with three PCI Express root ports.  The P690 sits at `03:00.0`
behind `00:11.0`, an **ALi PCI Express Root Port**:

    +-0b.0-[04]--          nForce3 250Gb AGP Host to PCI Bridge
    +-11.0-[03]----00.0    ALi PCI Express Root Port  <- the P690

The kernel does bring up an AGP GART, but it belongs to the nForce3 bridge and
has nothing to do with this card:

    agpgart-amd64 0000:00:00.0: AGP aperture is 64M @ 0xdc000000

**The vendor driver's bus messages do not describe the card.**  Both cards
produce byte-identical text:

    [mtx]   Board type detected: PCI
    [mtx]   Chipset 0x____:0x102b was detected
    [mtx]   AGP chipset was detected
    [mtx]   PCI transfers available for read write

"AGP chipset was detected" is emitted for a PCIe endpoint on a board whose AGP
bridge is a different device entirely, so it is reporting the **system's**
bridge, not the card's bus.  Do not use these lines to decide card behaviour --
the `EOBTag` difference in §2, which comes from the device id and a bus flag, is
the discriminator that actually varies.

**No GART translation is involved in command submission, on either card.**  The
ring is programmed with raw CPU physical addresses obtained from `/proc/self/
pagemap` and the engine fetches from them correctly on both the 0x0528 and the
P690 -- confirmed by the fetch pointer stopping exactly at the programmed end
address.  There is no IOMMU remap on this box, so physical == bus address.

### CLOSED: no aperture in the memory ABI

`GET_MEMORY_INFO` (ioctl nr=0) on the P690 returns:

    +0x00 = f0000000    framebuffer base
    +0x08 = 08000000    framebuffer size (128 MB)
    +0x10 = febfe000    MMIO base
    +0x18 = 00002000    MMIO size (8 KB)
    everything else 0 or 0xffffffff

Structurally identical to the 0x0528 capture (`d0000000` / `10000000` = 256 MB /
`febfe000` / `2000`). **There are no aperture fields at all**, so the ABI does
not hand userspace a GART window on either card, and surfaces live in VRAM.

The earlier claim that `mtx` registers no character-device major was **wrong** —
a mis-read of a truncated `/proc/devices`. It registers **major 252** as `mtx`.
The node simply does not survive a reboot because `/dev` is tmpfs; recreate it
with:

    mkdir -p /dev/matrox && mknod /dev/matrox/mtx0 c 252 0

With that node present the vendor OpenGL stack runs on this card:

    direct rendering: Yes
    OpenGL renderer string: OpenGL Linux library for Parhelia
    OpenGL version string: 1.3 ICD v1.03

which is the positive control the 3D work had been missing, **and it works.**

**ioquake3 runs on this card.** Full GL init and a live process:

    ----- R_Init -----
    ...setting mode 3: 640 480
    GL_RENDERER: OpenGL Linux library for Parhelia
    GL_VENDOR:   Matrox Graphics Inc.
    GL_VERSION:  1.3 ICD v1.03
    PIXELFORMAT: color(24-bits) Z(24-bit) stencil(8-bits)
    MODE: 3, 640 x 480 windowed hz:N/A
    ----- finished R_Init -----
    --- Common Initialization Complete ---

It reaches the menu and stays up, so the vendor 3D path is functional here.

**`glxgears` specifically reboots the box** -- that was measured and is a real
fault -- but it is one application, not the stack. An earlier revision of this
file generalised it to "the vendor 3D stack reboots this P690" and concluded a
known-good capture could not be taken here. **Both were wrong**, and the wrong
conclusion would have sent the capture work to the 0x0528 for no reason.

The consequence is the opposite of what was recorded: a known-good draw **can**
be captured on this card, from ioquake3 under `mtxtrace`, and diffed against
what our own tool emits.


## 5. The per-device branch census **[RE]**

Counted mechanically over both binaries (`objdump -d`, no vendor source read).

| binary | device-id compare sites | functions |
|---|---|---|
| `kernel/mtx_parhelia.o` | 21 | 1 (`mtx3376`, the bus-interface init) |
| `xserver/7_4_0/mtx_drv.so` | 69 | 29 |

The ids that appear: `0x520`, `0x521`, `0x525`, `0x527`, `0x528`, `0x2527`,
`0x2537`, `0x2538`, `0x2539`. Every branch splits them into

* **original Parhelia-512** — `0x527`, `0x528` (our PCI-X card)
* **LX / P-series** — `0x2537`, `0x2538`, `0x2539` (our PCIe P690)
* older parts — `0x520`, `0x521`, `0x525`, `0x2527`

so a unified driver needs exactly one family predicate plus a handful of
per-id exceptions, not a per-id table.

## 6. `0x308` carries a different VALUE per device — NEW **[RE]**

This is the one engine-register difference found so far, and it is in the
bus-master configuration group that §2 already flagged as the risk area.

`[KERN] mtx3376`, the tail of the function:

```
cbc:  cmp $0x527102b  -> dc5
cc9:  cmp $0x528102b  -> e35
cd6:  cmp $0x2537102b -> dba
      otherwise       -> ce6

e35 (0x0528):   if [r15+0xc] <= 5 -> dc5 else -> ce6
dba (0x2537):   if [r15+0xc] >  1 -> cf4 (ebx = 1) else -> dc5
dc5:            if bus flag bit 1 clear -> ce6; else set a flag bit and -> ce6

ce6:  mov $0x83,%ebx            /* 0x0528 */
cf2:  je  cf9
cf4:  mov $0x1,%ebx             /* everything else, P690 included */
d28:  mov %ebx,%edx
d2a:  mov $0x308,%esi
d32:  call memWriteDword        /* write(0x308, ebx) */
```

Whatever path is taken, the **value** is decided at `ce6` by device id alone:

| device | `0x308` |
|---|---|
| `0x0528` Parhelia-512 | **`0x00000083`** |
| `0x2539` P690, and every other id | **`0x00000001`** |

`0x300` is read-modify-written just before it with the AGP-mode fields and the
host-bridge fixups of `parhelia-engine-context-bringup.md` §1.4; those fields
are zero for a PCIe part, so on the P690 the pair reduces to
`0x300 = (read & 0xe0e0ff3f) | 0x100` and `0x308 = 1`.

`0x384` writes of 0 around this block are DMAPAD inserts, not configuration.

## 7. Config-space save differs in SIZE — NEW **[RE]**

Both branches guard on the same `dev[+0x8] & 0xc0000000 == 0x40000000` test
(a power/state gate) and then diverge:

* `[XDRV] mtx2096` (`0xc5580`), reached for **`0x528`**: calls
  `HSLPARSaveCriticalData`, reads struct field `0x3a8` and config dword 4.
* `[XDRV] mtx1900` (`0xc53e0`), reached for **`0x2538`**: loops
  `ClientReadConfigSpaceDword` for `ebx = 0..0x3f` — **64 config dwords, the
  PCIe extended header** — before reading MMIO `0x300`.

So suspend/restore on the PCIe parts must save 256 bytes of config space where
the PCI-X part saves the legacy header. A driver that saves only 64 bytes will
restore a P690 incorrectly.

## 8. Where the two cards stand for an open driver

| area | 0x0528 PCI-X | 0x2539 P690 PCIe | shared? |
|---|---|---|---|
| BAR layout | fb=0 (256 MB, 32-bit), mmio=1 | fb=0 (128 MB, **64-bit**), mmio=**2** | must probe, never assume |
| VRAM vs the flat `0x08000000` offset split | 256 MB — **overflows it, open question** | 128 MB — fits exactly | P690 is the unambiguous case |
| `0x308` | `0x83` | `0x01` | **§6, differs** |
| `0x300` | AGP fields + host-bridge fixups | fields zero | same code, different inputs |
| errata set | PowerM EOBTag Cap66Mhz CompBypass | PowerM Cap66Mhz CompBypass | §2 |
| config-space save | legacy header | 64 dwords | **§7, differs** |
| interrupts `0x8fc` | common path | `[XDRV] mtx1681` has a `0x2537` branch | mostly shared, one exception |
| DVO / output regs | `0x1c20` | `0x1c54` / `0x1c58` | already handled in the open driver |
| HAL init `0x1c58 = 0x40` | applied | **skipped** (`0x527`/`0x528` only) | bringup §1.2 step 9 |
| `0x3a8 \|= 0x20` | not applied | applied when `dev[0x2a0] == 0` | bringup §1.2 correction |
| engine register file, ring, fence | identical | identical | §3 |
| depth surface S8Z24 | format `0x36`, slot 1 | ioquake3 reports `Z(24-bit) stencil(8-bits)` | confirmed both |

### Still open, and where to look

1. **What `EOBTag` does to the hardware.** Still inference (§2). It sets bit 6
   of the assembled configuration byte; the 0x0528 gets it, the P690 cannot.
2. **How a 256 MB `0x0528` addresses VRAM above `0x08000000`.** Concrete new
   pointer: `[XDRV] mtx1158` (`0xe7e6e`) gates a `SFATOOL_StoreFeatureID`
   registration on `r12d > 0x8000000`, i.e. on VRAM larger than 128 MB. That
   feature id is the thread to pull. The P690 never takes this path, so the
   open driver can support the P690 fully without settling it.
3. **`0x404 == end` block-load framing on the P690.** Measured only on the
   0x0528 (which runs with EOBTag); §3 still says re-measure. Unchanged.
