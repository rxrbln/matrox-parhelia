# Parhelia-512 — engine and 3D-context bring-up, register level

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Scope: everything between "PCI device found, BARs mapped, nothing else done"
and "a Gouraud triangle lands in a VRAM surface we chose", **without** the
vendor kernel module and **without** an X server.

Everything here was read out of binaries only:

| tag | binary | how read |
|---|---|---|
| `[KERN]` | `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | `objdump -dr -M intel`; `.text` offsets, symbols `mtxNNNN` plus the un-obfuscated globals |
| `[XDRV]` | `matroxdriver-x86_64-cvs-20090511/xserver/7_0_0/mtx_drv.so` | same; real `HAL*`/`Client*` names |
| `[XDRV4]` | `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` | **`nm -D`, not `nm`.** The `.symtab` is obfuscated `mtxNNNN`, but the `.dynsym` carries **1039 real C++ mangled names** at the same addresses (`MmSurface`, `MmSurfDesc`, `MmHostAperture`, `Surface2d`, `SurfDesc2d`, `DeviceBitmap`, `HSLPAR*`). This is by far the best source for memory and surface semantics and we had not been using it. |
| `[HW]` | our own measurements on a 0x0528 board | |
| `[INF]` | inferred, self-consistent, not proven | |

Companion document: `docs/parhelia-surface-address-translation.md` carries the
full surface-addressing derivation summarised in §2.4.

The two register accessors that make `[KERN]` readable:

```
memWriteDword(handle /*rdi*/, reg /*esi*/, val /*edx*/)
memReadDword (handle /*rdi*/, reg /*esi*/, out /*rdx*/)
```

`handle = dev->[0x2030]`. Every MMIO access in the kernel object goes through
them, so the register map below is complete for that binary: it touches exactly
`0x000, 0x300, 0x308, 0x318/0x31c, 0x384, 0x400, 0x404, 0x408, 0x41c, 0x450,
0x8f0, 0x8f4, 0x8f8, 0x8fc, 0x908, 0x910, 0x914, 0x918, 0x91c, 0x920, 0x924,
0x938, 0xbc0, 0x10b0, 0x12b8, 0x12bc, 0x1800, 0x19a8, 0x1a00, 0x1ba8, 0x1dc4`
plus `0x318`/`0x31c`, which are reached through a computed operand
(`mtx1937+0x133`, `esi = 0x31c - ((flags & 0x70000) ? 0 : 4)`) rather than an
immediate — and nothing else. **Everything else in `0x1000-0x13ff` reaches the chip only as
DMA packet payload, never as a direct MMIO write.** That is a load-bearing
result for a from-scratch driver: the engine is programmed through the ring,
not through MMIO.

---

## 0. The DMAPAD rule (applies to every MMIO write below) — PROVEN

`[KERN]` every call site of `memWriteDword`, e.g. `mtx1391+0x2f9`
(`0x9f9a6`..`0x9f9c1`), `mtx2508+0x11d` (`0x3199d`), `mtx1937+0xb7`
(`0x32417`):

```
prev = shadow->lastReg;          /* shared->[0x88], init 0xffffffff */
if (shadow->padEnable) {         /* shared->[0x84], init 1          */
    if (prev < reg && prev + 0x80 >= reg)
        mmio[0x384] = 0;         /* DMAPAD                          */
    shadow->lastReg = reg;
}
mmio[reg] = val;
```

A *read* always sets `lastReg = 0xffffffff` (i.e. disables the pad for the next
write). The hazard window is `reg ∈ (prev, prev+0x80]` — a write to a register
**above** the previous one by at most 128 bytes. Every `0x384 = 0` in an
mmiotrace is a side effect of the *following* write; a re-implementation must
apply the rule generically instead of copying pad writes out of a trace.

---

## 1. Engine init

### 1.1 What the kernel module does NOT do

The 22-step device-start table at `[KERN]` `.rodata+0x1860`
(`{init_fn, cleanup_fn, name}`, 0x18 bytes/entry; the name slot points at an
empty string for all 22 entries — the "names" the earlier note mentions do not
exist) contains almost **no register-level engine init**. Walking every
`memWriteDword` call site and matching it against the 22 init functions leaves
exactly two:

* step 19, `mtx3810`/`mtx3191` — the WARP unit; writes `0x920` (§3.1b).
* step 13, `mtx1625`/`mtx1584` — the interrupt sources; `mtx1625` calls
  `mtx1769` (set a bit in `0x8fc`) and `mtx1584` calls `mtx2689` (clear one).

Everything else is C++ object construction. The register bring-up proper is in
the X driver's HAL.

### 1.2 The HAL init sequence — PROVEN, `[XDRV] mtx1373` (`0xddbb0`)

Order as emitted. `r12` = device, register/value pairs verbatim:

| # | offset in `mtx1373` | write | value |
|---|---|---|---|
| 1 | `0xddc4b` | — | `HSLPARApplyVidReset(dev, -1)` |
| 2 | `0xddc94` | `0x1c00` | `read \| 0x00808020` |
| 3 | `0xddca5` | `0x1c04` | `read \| 0x00000020` |
| 4 | `0xddcb2` | — | `ClientWait(10 µs)` |
| 5 | `0xddcca` | `0x1c08` | `2` |
| 6 | `0xddcdc` | `0x1c24` | `7` |
| 7 | `0xddceb` | `0x1c50` | from PInS |
| 8 | **`0xddcfa`** | **`0x8fc`** | **`0`** |
| 9 | `0xddd24` | `0x1c58` | `0x40` (0x527/0x528 only) |
| 10 | `0xddd4d` | `0x920` | `read & 0x7fffffff` |
| 11 | `0xddd5f` | `0x10b0` | `0x40` |
| … | | display/PLL/DAC block | not engine-relevant |
| 12 | `0xde26a` | — | `HSLS90InitMemClk()` |
| 13 | `0xde297` | `0x1c0c` | `read \| 0x0808` |
| 14 | `0xde2c4` | `0x1c10` | `read \| 0x0808` |
| 15 | `0xde327` | `0x12b8` | `0x00070601` |
| 16 | `0xde33b` | `0x12b0` | `0` |
| 17 | `0xde340`..`0xde371` | `0x010,0x020, … ,0x130` | `read & 0x0fffffff` (19 iterations, `i = 1..0x13`, `reg = i<<4`) |
| 18 | `0xde37d` | `0x000` | `0x10000000` |
| 19 | `0xde38c` | `0x004` | `0x00000000` |
| 20 | `0xde39e` | `0x008` | `0x10000020` |
| 21 | `0xde3b0` | `0x00c` | `0x00010001` |

**REPLICATED AND VERIFIED ON HARDWARE (2026-09-07).** `engine_init()` in
`tools/parhelia-3dtri.c` now emits steps 1-6, 8-11, 13-16 in this exact order,
plus 17-21 behind `--initaperture`, plus the §1.4 bus pair. Step 1 is a full
`apply_vid_reset()` for both heads (timing-block reset, `0x1c00`/`0x1c04`
bit 4 route, `0x1c00` bit 24 strobe), ported from the open driver's
`parhelia_apply_vid_reset`.

Measured from a cold boot with **`mtx` never loaded and X down**: the 2D
instrument draws 120000/120000 px before the init, 120000 after it, and
120000 again from a fresh process. So the sequence is faithful and
non-destructive with no vendor code anywhere in the path.

**Two steps are NOT replicated**, and neither is a literal register write:
* step 7, `0x1c50` = from PInS — needs the VBIOS PInS block, which this tool
  does not parse.
* step 12, `HSLS90InitMemClk()` — **now decoded and partially implemented**
  (`--initmemclk`), see §1.2b. Incomplete, and the partial form breaks the
  engine, so it is off by default.

Observation worth chasing: `0x10b0` reads back `0` after step 11 writes `0x40`.

**A trap this exposed:** `./3dtri --engineinit` alone does *not* stop after the
init - main falls through to the built-in WARP triangle draw, which renders
nothing and then hangs, killing the engine until reboot. An earlier reading of
that as "engine_init breaks the engine" was wrong. Always pair `--engineinit`
with the action you actually want (e.g. `--dmafill`), and never diagnose from
a `timeout`-killed run: stdout is block-buffered into a pipe and the whole
transcript is lost on SIGKILL, which is why the run looked silent.

Corrections to `docs/parhelia-engine-init.md`:

* **`0x8fc = 0` is not "hardware DMA, not emulated".** `0x8fc` is the
  **interrupt enable mask** — see §1.3. Writing 0 masks all sources. It has
  nothing to do with DMA mode.
* **`0x3a8 |= 0x20` is conditional**, not part of the common path:
  `[XDRV] mtx1373+0x8de` (`0xde48e`) only reaches it when the device id is
  `0x2539` *and* `dev->[0x2a0] == 0`.
* `0x10b0` is `0x40` here `[XDRV]`, but `0x10` in `[KERN] mtx3376+0x99`
  (`0xb19`). Both are init values for different owners; neither is the
  per-primitive value.

### 1.3 Interrupts — PROVEN

| reg | role | evidence |
|---|---|---|
| `0x8f4` | interrupt **status** (pending, read) | `[KERN] mtx1415+0x42` (`0x3adf2`) |
| `0x8f8` | interrupt **acknowledge** (write back the pending bits) | `[KERN] mtx1415+0x76` (`0x3ae26`) |
| `0x8fc` | interrupt **enable mask** | `[KERN] mtx1769` (set: read/OR/write, `0x3a4a1`+`0x3a504`), `mtx2689` (clear: read/AND-NOT/write, `0x3a312`+`0x3a36b`), `mtx3091` (`0x8fc = 0`, `0x3ac88`) |

`mtx1415` is the ISR: read `0x8f4`; if non-zero write the same value to `0x8f8`
and then walk up to 5 source records at `dev + i*0x78`, each with its mask at
`+0x68` and an enabled flag at `+0x70`. The enable shadow is kept at
`shared->[0x98]` (`mtx1769+0xf9`, `0x3a514`).

**A from-scratch driver that polls does not need any of this: write
`0x8fc = 0` and never touch `0x8f4`/`0x8f8`.**

### 1.4 The bus-interface / chipset block — PROVEN, `[KERN] mtx3376` (`0xa80`)

```
0x10b0 = 0x10
0x12b8 = 0x00070601 & 0xc00fffff          /* = 0x00070601 */
v = read(0x300);
v &= 0xe0e0ff3f;
v |= (agpMode & 0xff000000) | ((agpMode<<16) & 0x00100000) | ((agpMode & 7)<<16);
v |= 0x0100;                              /* byte1 bit0 */
   + per-chipset fixups keyed on the *host bridge* id:
     0x0527102b + VIA 0x3051106  -> v |= 0x40
     != 0x0527102b               -> v |= 0x080000
     Intel 0x25008086 / 0x25018086 -> further path at 0xe00
write(0x300, v);  write(0x308, …)
```

`0x300`/`0x308` are the AGP/PCI bus-interface control pair. For a PCI-X 0x0528
in a PCI slot the AGP fields are zero and this reduces to
`0x300 = (read & 0xe0e0ff3f) | 0x100`. **Not required** for bus-master DMA to
work (we have drawn without it) — it selects AGP transfer mode.

### 1.5 The command stream: PRIMADDRESS / PRIMEND / write-back — **the `0x400` question, RESOLVED**

There is **no ring** in the circular-buffer sense: no base register, no limit
register, no read/write pointers. The engine is given a *start* and an *end*
bus address per submission and fetches the span in between. Three registers:

| reg | role |
|---|---|
| `0x404` | **PRIMADDRESS** — bus address of the first dword to fetch, `& ~3` |
| `0x408` | **PRIMEND** — bus address one past the last dword, `\| class`; **writing it is the doorbell** |
| `0x400` | **status / write-back page base** — bus address of the driver's shared status structure |

`0x400` is *not* a ring limit. Proof chain:

1. `[KERN] mtx728` (`0x2610`) allocates two regions and builds a descriptor:
   * `0x26a3`: `MtxRegion_Alloc(pool, size=0x18,   type=3, flags=2)` → region A
   * `0x26e7`: `MtxRegion_Alloc(pool, size=0xae0, type=1, flags=2)` → region B
   * `0x2727`..`0x27dc`: collects `MtxRegion_GetLinear/GetPhys/GetSize` of both,
     plus `MtxDevice_GetRegisters` phys+size (the MMIO BAR),
     `MtxDevice_GetFrameBuffer` phys+size, `MtxDevice_GetBmFlags`, and
     `ClientGetNbCPUs() > 1`.
   * `0x2803`: calls `mtx3229(&descriptor, dev)`.
2. `[KERN] mtx3229` (`0x2460`) initialises region B — the **shared locked data
   buffer**, 0xae0 bytes — and this is the structure every other function reaches
   through `dev->[0x10]`:

   | field | set to | at |
   |---|---|---|
   | `+0x40` (u32) | **`phys(B)`** | `0x24ad` |
   | `+0x44` | 0 (sync-tag counter, later stepped by 8) | `0x24ea` |
   | `+0x4c` (u32) | **`phys(B) + 0x20`** | `0x24b9` |
   | `+0x58` | `size(B)` = 0xae0 | `0x2488` |
   | `+0x60` (u64) | `phys(B)` | `0x249e` |
   | `+0x6c` | `nCPUs > 1` | `0x24c8` |
   | `+0x70` | 0 — spinlock (`OsInterlockedExchange`) | `0x2490` |
   | `+0x74` | 0 — last submitted PRIMEND | `0x24cb` |
   | `+0x78` | 0 — busy count; every writer spins on it | `0x24d2` |
   | `+0x84` | **1 — DMAPAD enable** | `0x24e0` |
   | `+0x88` | **0xffffffff — last register written** | `0x250d` |
   | `+0x98` | 0 — interrupt-mask shadow | — |
   | `+0x9c` | **`memReadDword(0x450)`** — fence shadow, seeded from HW | `0x2517`, `0x2556` |
   | `+0xa8/+0xac` | framebuffer phys / size | `0x2581`, `0x2596` |
   | `+0xb8/+0xbc` | `fbsize >> 1` twice | `0x25a3`, `0x25c4` |
   | `+0xc8/+0xcc` | MMIO BAR phys / size | `0x2561`, `0x2576` |
   | `+0xe0` | `BmFlags` | `0x24a2` |
   | `+0x900…` | zeroed, then 15 entries of stride 0x18 set to `0xffffffff` | `0x25ca`..`0x25fc` |

3. `[KERN] mtx2508` (`0x31880`) — the only writer of `0x400`:
   ```
   0x318cc  poll  read(0x8f0) until bit 17 set
   0x31933  poll  read(0x450) until == shared->[0x9c]
   0x319d1  write 0x404 = (busaddr(writePtr)) & ~3
   0x31a52  write 0x400 = shared->[0x40]        /* &= ~0x1f if erratum bit 2 */
   ```
   `shared->[0x40]` is `phys(B)`.

4. The fence packet the driver emits into the stream, `[KERN] mtx1017+0x1ba`
   (`0x6299`) and `mtx2095+0x9f` (`0x3205f`):
   ```
   0x21212425      /* register indices 0x25, 0x24, then 0x21 = END */
   <tag>           /* -> index 0x25 = MMIO 0x1094 */
   shared->[0x4c]  /* -> index 0x24 = MMIO 0x1090 = phys(B) + 0x20 */
   ```

So the observed `0x400 == 0x1b466000` is `phys(shared status page)`, and the
fence records observed at `0x1b466020` are at `phys + 0x20` — exactly
`shared->[0x4c]`, the address the driver hands the engine in register `0x1090`.
The page is a page-aligned locked allocation, which is why `0x400` reads
page-aligned. **`0x400` is a write-back page base, `0x404`/`0x408` are the
command-span pointers. Both readings in the old notes were half right.**

### 1.6 Submission — PROVEN, `[KERN] mtx1937` (`0x32360`)

`mtx1937(chan, start, end, startTag, class)`:

```
if (start >= end) return;                    /* 0x32364 */
if (class == 0) {                            /* buffer lives in VRAM        */
    reg = (chanFlags & 0x70000) ? 0x31c : 0x318;
    mask = (chanFlags & 0x7e000) ? 0x1 : 0x57;
    want = (chanFlags & 0x7e000) ? 0x1 : 0x50;
    while ((read(reg) & mask) != want) ;      /* 0x324a6..0x324f0 */
}
/* append a 5-dword secondary-buffer CALL to the primary buffer */
*p++ = 0x20292820;
*p++ = 0;
*p++ = start | startTag;                      /* every caller passes 2      */
*p++ = end   | class;
*p++ = 0;
write(0x408, busaddr(p));                     /* 0x3244b — the doorbell     */
```

Two things a from-scratch driver must copy:

* **A command buffer in VRAM (class 0) requires a memory-controller flush
  poll on `0x31c`/`0x318` before the doorbell.** This is the mechanism behind
  the "VRAM ring hazard" we measured — a VRAM-resident buffer submitted
  without that poll can be fetched before the writes have landed. Using
  `GFP_DMA32` system memory (class 3) skips the poll entirely.
* The doorbell value is the *bus* address of the primary buffer's write
  pointer, i.e. `hostPtr + buf->[0x58]`, where `buf->[0x58]` is a
  `busAddress - hostVirtualAddress` **delta** kept per buffer
  (`mtx2508+0xf4` `0x31974`, `mtx1937` via `mtx2095+0xdd` `0x320a9`), OR'd with
  `buf->[0x34]`, the 2-bit memory class.

### 1.7 The fence — PROVEN

| reg | role |
|---|---|
| `0x1090` (index 0x24) | bus address of a 4-byte completion word |
| `0x1094` (index 0x25) | the value to store there |
| `0x450` | MMIO mirror of the last retired tag; the driver polls **this**, not host memory |

Everywhere the driver waits it does:

```
while (!(read(0x8f0) & (1<<17))) ;            /* DMA engine idle           */
while (read(0x450) != shared->[0x9c]) ;       /* last submitted tag retired*/
```

(`mtx2508+0x51`/`+0xb8`, `mtx479+0x57`/`+0xbb`, `mtx1391+0x108`/`+0x146`.)

`shared->[0x9c]` is *the expected value*, seeded from `read(0x450)` at init and
advanced by the driver. So `0x450` is readable and meaningful **from a cold
engine**, which makes it the ideal instrument for a from-scratch bring-up: read
it once, submit a packet that writes `0x1094 = read+1`, poll `0x450`.

### 1.7b `0x41c` — the block-sync tag readback, PROVEN

`0x41c` is **read-only in the entire kernel object**: 14 `memReadDword` sites
(`mtx508+0x30`, `mtx2434+0x312`, `mtx4025`, `mtx984`, `mtx3616`, `mtx3388`,
`mtx986`, `mtx1833+0x9b` `0x57a2b`, `mtx1425`, `mtx3450`, `mtx2260+0x90`
`0x66200`, `mtx2961` ×2, `mtx2894`) and **zero** `memWriteDword` sites. Every
site does the same thing:

```
cur = read(0x41c) & ~7;        /* low 3 bits are flags */
if (cur > wanted) …            /* the tag has passed   */
```

So `0x41c` is the retired-value of the tag armed by writing `0x12bc = tag|1`
(the only `0x12bc` writer is `mtx2434+0x2de`, `0x39bbe`), and it is a *different*
counter from the DMA fence at `0x450`. Two independent completion mechanisms:

| arm | poll | granularity |
|---|---|---|
| packet writes `0x1094 = tag`, `0x1090 = addr` | `0x450` | whole submission retired |
| packet writes `0x12bc = tag \| 1` | `0x41c` (mask `~7`) | drawing-engine block retired |

### 1.8 The DWGSYNC / interrupt-terminated submit packet — PROVEN, `[KERN] mtx3144` (`0x31c30`)

Emitted verbatim into the primary buffer (`0x31c67`..`0x31ceb`):

```
0x21c0c0c0      /* indices 0xc0, 0xc0, 0xc0, 0x21=END ; 0xc0 -> MMIO 0x1300 */
0x00000307      /*   0x1300 = 0x307 */
0x00000004      /*   0x1300 = 0x004 */
0x00000707      /*   0x1300 = 0x707 */
0xc0afafdd      /* indices 0xdd, 0xaf, 0xaf, 0xc0 ; 0xdd->0x1374, 0xaf->0x12bc */
0x00000000      /*   0x1374 = 0     */
tagA | 1        /*   0x12bc = tag|1 */
tagB | 1        /*   0x12bc = tag|1 */
shared->[0x8c]  /*   0x1300 = …     */
```

`tagA`, `tagB` come from `shared->[0x44]`, incremented by 8 each time and
skipping 0 (`0x31ca0`..`0x31ccf`). This is the exact packet behind the
"`0x1300` nibble 6/7 = sync opcode, `0x12bc` tag|1 arms block-until-retire"
note; it is now proven, with the operand values.

### 1.9 Engine-idle signature — PROVEN, `[KERN] mtx1391` (`0x9f6b0`)

The strictest idle wait in the whole binary, used before touching the WARP
program store:

```
while (!(read(0x8f0) & 0x00020000)) ;              /* 0x9f7b3 / 0x9f7e1 */
while (read(0x450) != shared->[0x9c]) ;            /* 0x9f81b           */
do { a = read(0x908); b = read(0x924); }
  while ((a & 0xfff7ffbf) != 0x63a78001 ||
         (b & 0x00000fff) != 0x00000100);          /* 0x9f86b / 0x9f8e7 */
while (read(0x8f0) & 0x03000000) ;                 /* 0x9f967           */
```

`0x908` and `0x924` are pipeline status words; `0x63a78001` is the idle
signature of a Parhelia-512. `[INF]` the exact bit meanings are not derivable —
only the mask/compare constants are.

### 1.10 Register-list packet decoding — PROVEN from the vendor's own interpreter

`[KERN] mtx2434` (`0x398e0`) is a *software* interpreter of the same packet
format the hardware consumes, which makes it an authoritative spec:

* `ctx->[0x2024]` = the current tag dword, consumed one byte at a time from the
  LSB; `ctx->[0x2020]` = how many indices are left in it (reset to 4 when a new
  tag dword is loaded, `0x39943`).
* `reg = 0x1000 + (tag & 0xff) * 4` (`0x3990d`).
* index `0x21` (`reg == 0x1084`) **terminates** the tag dword and consumes **no**
  data dword — the next dword is read as a *new tag* (`0x39917`, tested before
  the payload fetch). This is the byte the vendor uses to pad out a short tag:
  `0x21218988` is "index `0x88`, index `0x89`, done", a 3-dword group.
* index `0x20` (`reg == 0x1080`) **does consume a data dword** and then does
  nothing with it. The payload fetch at `0x39960` happens *before* the
  `cmp ebx, 0x1080` at `0x399aa`, so `0x20` is a true DMAPAD slot: index plus
  one dummy dword. (An earlier draft of this section had it consuming no data —
  that was wrong; the fetch is unconditional.)
* every other index consumes one payload dword and shadows it at
  `ctx[0x1020 + i*4]` (`0x3997e`, `ctx + (reg>>2)*4 + 0x20`).

Practical consequence: `0x21` is the cheap terminator, `0x20` costs a dword.
Our own recipe uses `0x20` as filler and works, but matching the vendor means
closing a tag with `0x21` bytes.
* `0x108c`, `0x109c` and `0x12bc` have extra side effects in the interpreter
  (`0x39992`, `0x39970`, `0x39aea`) — `0x12bc` is the sync-arm register and is
  followed by a `0x450` poll and a `0x41c` write (`0x39b4f`, `0x39bed`).

---

## 2. Memory and heaps

### 2.1 What the kernel object actually has — PROVEN

The un-obfuscated global symbols are the complete memory API:

```
MtxRegion_Alloc / _Free / _GetLinear / _GetPhys / _GetSize
MtxRegion_FindFromVirtual / _IncrementRefCount / _DecrementRefCount(AndTest)
OsAllocLockedPages / OsFreeLockedPages / OsMapPhysicalMemory / OsUnmapMemory
OsGetPageSize
MtxDevice_GetFrameBuffer / _GetRegisters / _GetPciHandle / _GetRegisterHandle
MtxDevice_GetBmFlags
```

**There is no GART, no page table and no address-translation call anywhere in
the kernel object.** No symbol contains Gart, Tlb, PageTable or Translate, and
no function converts a GPU address back to a host address. Buffers are handed
to the engine as raw bus addresses. (There *is* an aperture — the
`MmHostAperture` windows of §2.3 — but it is a host-side view through BAR0, not
a translation the drawing engine applies; the kernel object never touches it.)

`OsAllocLockedPages(proc, size, contiguous, below4G, out)` is wrapped three
ways (`[KERN]`):

| wrapper | contiguous | below 4 GiB |
|---|---|---|
| `mtx3583` (`0x56f80`) | 1 | 1 |
| `mtx586` (`0x570c0`) | 0 | 1 |
| `mtx3185` (`0x571f0`) | 0 | 0 |

So system-memory heaps are ordinary locked page allocations; the "below 4 GiB"
variants exist because the engine's address registers are 32-bit. This is
exactly what our `GFP_DMA32` `dma32buf` module already provides.

**Do not confuse `MtxRegion_Alloc`'s `type` argument with the bus class.** The
two allocations in `mtx728` use `type = 3` (0x18 bytes) and `type = 1` (0xae0
bytes) and *both* end up as host system memory reached by physical address; the
bus class that gets OR'd into an engine address is a separate per-buffer field
(`buf->[0x34]`, §2.2).

### 2.2 The memory class — PROVEN

Every buffer descriptor carries:

* `buf->[0x34]` — the 2-bit **class**, OR'd into the low bits of any address
  handed to the engine. `0` = local/VRAM, `2` = AGP, `3` = PCI/system.
  `[KERN] mtx2425+0x1c` (`0x31aac`) compares it against 3; `mtx1937+0x65`
  (`0x323c5`) ORs it into PRIMEND; `mtx2508+0xfb` (`0x3197b`) ORs it into the
  value cached for PRIMADDRESS.
* `buf->[0x58]` — a `busAddress - hostVirtualAddress` **delta**, valid only when
  `buf->[0x68] & 0xffffffff0000ff00` is non-zero (`mtx2508+0xee` `0x3196e`,
  `mtx2095+0xd7` `0x320a3`, `mtx1937` via caller). GPU address =
  `hostPtr + delta`, then `| class`.
* `buf->[0x30]` — a coarse memory type (0/2/3), used to pick alignment:
  `[KERN] mtx2425+0x147` (`0x31bd7`) sets the per-submission alignment to
  `0x10` or `0x08` for class 3.

That is the whole translation. **Bits [1:0] are a bus-cycle selector, and the
remaining 30 bits are a flat GPU offset (§2.3b).**

The definitive source is `MmSurface::AttachToBlock`, present in both binaries
with identical logic — `[XDRV4] 0x073950` and `[KERN] 0x05c1b0`:

```
addr = block->ulOffset | busClass(surface->memType);
surface->m_ulGpuAddress (+0x90) = addr;
MmSurfDesc dword0               = addr;
```

`memType → class`: `0, 1, 6, 7 → 0`; `2 → 2`, demoted to `3` at `0x73a05` when
caps bit 4 is clear or bit 10 set; `3 → 3`; `4, 5 → 0`. A lookup-table form of
the same map — `{0,0,2,3,3,3,0,0,2,0…}` — sits at `[XDRV4] 0x3340c0` and is used
by `BmBuffer::AssignMemory`; it disagrees with the switch for memType 4/5
(table says 3, switch says 0). `[OPEN]`, but memType 4 is a "no allocation"
pseudo-type so it may never matter.

An exhaustive scan of the memory-manager range for `shr`/`shl`/`sar` by 3, 4, 6,
7 or 8 across all four binaries found only struct strides and bits→bytes
conversions. **There is no shift on any address path.**

### 2.2b VRAM partitioning at init — PARTIAL

The only VRAM bookkeeping the kernel object performs is in the shared struct
(`[KERN] mtx3229`):

```
shared->[0xa8] = (u32)phys(framebuffer)      /* 0x2581 */
shared->[0xac] = size(framebuffer)           /* 0x2596 */
shared->[0xb0] = 0                           /* 0x258c */
shared->[0xb8] = size >> 1                   /* 0x25a3 */
shared->[0xbc] = size >> 1                   /* 0x25c4 */
```

`+0xb8` is the **local** VRAM size and `+0xbc` the **non-local** pool size — the
two inputs `mtx1876` uses to build the bank table (§2.3b); `mtx3229` seeds both
to `fbsize >> 1` and the layers above overwrite them.

There is **no** code in the kernel object that places the front buffer, back
buffers, Z buffer or textures — those decisions are made above it. The X
driver's policy is hard-coded in `[XDRV] mtx40` (`0x3dc40`): it offers only
`videoRam/2 - 8 MB` to the on-screen + off-screen pools, divided further by 2, 3
or 4 for a shared entity, TripleHead or QuadHead. There is no registry key for
heap sizes — the defaults that do exist are `UseTextiled = 1`, `MmFlags = 2`,
`MemLevels = 0x3ff00842`, `PhysicalMemory = 0x100`,
`UseSundogMemoryStrategies = 0`.

A from-scratch driver is free to lay VRAM out however it likes, subject only to:

* the flat offset map of §2.3b (local below `0x08000000`);
* the surface alignment implied by `0x1224`/`0x1234` — `0x2000` for a 32 bpp
  type-2/3 surface (`GetLocalAlignmentAndGranularityREVB`, `0x74170`);
* the **display start** alignment, if the surface is scanned out:
  `HSLPARGetAddressAlignmentMask` (`0xad1e0`) requires 8 KB at 32 bpp, 4 KB at
  16 bpp, 2 KB at 8 bpp on `0x0527`/`0x0528` (one bit less on
  `0x2537`-`0x2539`); the per-head start registers stride `0x200`
  (`0x1824 + head*0x200`).

Two kernel ioctls are worth knowing if we ever want to co-exist with the vendor
module rather than replace it: `OsGetLocalMemoryInfo` = `0xc060461a`,
`OsAllocSurfaceBlockMemory` = `0xc0804618` (the latter is also what the host
aperture path uses).

### 2.3 The `0x0000-0x013f` block: the **host aperture descriptor table** — PROVEN

`[XDRV] HSLPARInitializeHardware` (`0xc035a`; the same code is `mtx1373+0x790`
in 7_0_0 and `MTXPARM.sys 0x32ecc`) programs it:

```
for (i = 1; i <= 0x13; i++)  reg[i<<4] = read(reg[i<<4]) & 0x0fffffff;
reg[0x000] = 0x10000000;
reg[0x004] = 0x00000000;
reg[0x008] = 0x10000020;
reg[0x00c] = 0x00010001;
```

**20 slots of 16 bytes** at `0x000`, `0x010`, … `0x130`. The `MmHostAperture`
chain in `[XDRV4]` gives the field meanings:

| dword | field |
|---|---|
| `+0x00` | **valid** (bit 28) `\|` BAR0 aperture offset `\|` bus class in bits [1:0] |
| `+0x04` | the surface's GPU address (same encoding as `0x1220`) |
| `+0x08` | ctrl — `pitch[13:0] \| type[18:16] \| format[29:24]`, i.e. the same word as `0x1224` |
| `+0x0c` | dims — `height[13:0] \| depth[27:16]` |

So slot 0's boot values decode as: valid, aperture offset 0, class 0; GPU
address 0; ctrl `0x10000020` = type 1, pitch 0x20; dims `0x00010001` = height 1,
depth 1. The disable loop clears bit 28 (the top nibble) of every other slot's
dword 0.

This is a **host-visible aperture** — a set of windows through BAR0 that let the
CPU touch a surface with the hardware applying the surface's own tiling/pitch —
not a GPU-side page table. Nothing in the drawing path consults it, and
`[KERN]` never reads or writes it. `MmHostAperture::Lock(unsigned long*,
void**)` returns `*arg1 = ap[0x70] + block[0x10]` (a GPU offset) and
`*arg2 = ap[0x70] + block[0x18]` (a CPU pointer).

~~An open driver that maps VRAM linearly through BAR0 does not need the table at
all beyond the boot programming above.~~

**CORRECTED ON HARDWARE (2026-09-07): BAR0 is NOT linearly writable.** Measured
on the 0x0528 with X and the GL client frozen, writing through the `resource0`
`MAP_SHARED` mapping and reading back **in the same process** immediately after
a write barrier:

| VRAM offset | write sticks? |
|---|---|
| `0x0400000`, `0x0800000`, `0x0a00000`, `0x0c00000`, `0x0e00000`, up to `0x0f40000` | **yes** |
| `0x0fb0000`, `0x1800000`, `0x2f60000`, `0x3800000`, `0x4000000` and above | **no — silently dropped** |

Binary search puts the boundary at exactly **16 MB (`0x1000000`)**. It is not
aliasing: a pattern written at `0x3800000` appears at no 16 MB alias, and one
written at `0x0400000` appears only at `0x0400000`. **Reads above 16 MB are
genuine** — the render target at `0x2f60000` reads back real engine output — so
this is a write-path limit, not a mapping error.

The practical consequence, and the reason CPU-poked textures never worked: a
texture cannot be placed in VRAM by writing through BAR0 unless it lives in the
first 16 MB. The vendor's own textures sit at `0x09006000` (150 MB), which the
CPU cannot write either, so **the vendor must upload through the engine** — a
2D blit or DMA from a system-memory staging buffer — which is consistent with
the earlier correction that the "linear texture memory" measurement had
actually measured the CPU staging heap, not the VRAM surface.

Whether the aperture table of this section is what gates the 16 MB window, or
whether it is a separate aperture-size control, is **not yet established**.

### 2.3b The GPU offset space is flat, split at `0x08000000` — PROVEN

`[KERN] mtx1876` (`0x56290`) builds the memory-bank table from **two**
independent inputs, the local VRAM size (`ctx->[0x10]->[0xb8]`) and the
non-local pool size (`->[0xbc]`), and always places local banks below
`0x08000000` and non-local banks at `0x08000000` and up. The decisive case is
`local = 64 MB, nonlocal = 0` (`0x56397`): it emits local banks
`(0, 0x2000000)` and `(0x2000000, 0x2000000 - resB)` and **no non-local bank at
all**.

```
[0x00000000, 0x08000000)   local VRAM
[0x08000000, …        )    non-local (AGP / PCI system memory)
bits[1:0] of any address   bus class (0 local, 2 AGP, 3 PCI)
```

Reserved head/tail constants: 2 MB (`resA`), 640 KB / 1 MB (`resB`), 512 KB
(`resC`). When two memory levels are in use the *local* half is itself split in
two banks (128 MB → `(0, 0x6000000)` + `(0x6000000, 0x2000000)`) — the plausible
referent of the `…ToABSundog` / `…ToCDSundog` registry switches. `MTXPARD`'s
`cmp ebx, 0x8000000` heap cap and `MTXPARM`'s 128 MB framebuffer-mapping clamp
for chip family ≤ 0x500 agree with 128 MB of local offset space.

**OPEN, and worth settling before an open driver allocates high VRAM:** how a
256 MB `0x0528` addresses VRAM above `0x08000000`. The miniport sizes the board
correctly (PInS code 3) but this offset map has no room for it.

### 2.4 Verdict on `0x1220 = 0x02000000` — RESOLVED

**There is no address translation. The bug was the stride.**

Full evidence in `docs/parhelia-surface-address-translation.md`; the load-bearing
points, all `[PROVEN]`:

1. **The base is a plain byte address plus a 2-bit memory-class tag.** Four
   independent witnesses, two binaries. `xserver/7_4_0/mtx_drv.so` still carries
   **C++ mangled names** for the memory manager — a source we had not used:
   * `MmSurface::AttachToBlock` (`.text+0x73710`): `surf->[0x90] =
     block->offset | tag`, `tag` = 0 for memory types 0/1/6/7, 2 for type 2,
     3 for type 3. No shift.
   * `Surface2d::Enable` (`.text+0x7bf00`): `bitmap->[0xe0] = surf->[0x90]`,
     then `|= 3` for non-local surfaces.
   * `DeviceBitmap::ProgSurfDesc` (`.text+0x7c350`): emits
     `{header, bitmap->[0xe0], bitmap->[0xc4]}` → `{base, cfg}`. No shift, no
     table lookup.
   * `[KERN] mtx1132` (`0x4c730`): `addr = offset + region->[0x18]`, then
     `| 2` for AGP, `| 3` for PCI/system, nothing for local — the same rule.
   * `HSLPAR_Register_PriSurfAddr_Set` (`.text+0xc5810`) masks the *scan-out*
     start to `0x07fff800` (2 KB-aligned byte address) — the display controller
     addresses VRAM the same way.
2. **The row stride is not `pitch * bpp/8`.** `MmSurfDesc::GetHostAperturePitch`
   (`.text+0x745c0`) reads `MmSurfDesc+0x04`, which *is* the cfg register word:

   | cfg type `[18:16]` | byte stride between scanlines |
   |---|---|
   | 0, 1 | `roundup(pitch, 32) * bpp/8` |
   | **2, 3, 4, 5** | **`next_pow2(max(pitch, 32)) * bpp/8`** |
   | any, pitch > 8191 | `8192 * bpp/8` |

   The captured colour target has `cfg = 0x32020780` — type **2**, width 1920,
   32 bpp — so its scanlines are **8192 bytes apart, not 7680**. Every VRAM
   sweep we ran was keyed to 7680 and therefore walked past the data, drifting
   512 bytes further off with every row. This is independently corroborated by
   our own hardware result in `randr-native-displaywidth-pow2-stride.patch`:
   the native DC only scans out when `displayWidth` is rounded up to a power of
   two. Supporting datum: `GetLocalAlignmentAndGranularityREVB`
   (`.text+0x74170`) aligns a 32 bpp type-2/3 surface to `0x2000` = 8192 bytes,
   exactly one power-of-two scanline.
3. **The cfg word decodes fully:**

   | bits | field |
   |---|---|
   | `[13:0]` | pitch in pixels — the surface **width**, not the stride |
   | `[18:16]` | surface type 0..5 (0/1 linear, 2 render target, 3 displayable, 4/5 texture) |
   | `23` | byte/word swap |
   | `[29:24]` | 6-bit pixel format (`MmSurfDesc::GetBitsPerPixel`, `.text+0x73e70`) |
   | `[31:30]` | tiling; 0 = linear in every captured descriptor |

   Format codes: 8 bpp → `0x10`, 16 bpp → `0x20`, 30 bpp (GigaColor) → `0x34`,
   24/32 bpp → `0x30`, and `[KERN] mtx1192` (`0x3dcb0`) force-rewrites a
   driver-owned colour target to `0x32`
   (`cfg = (cfg & 0xc0ffffff) | 0x32000000`).
4. **`0x1220` was not the render target in that capture.**
   `SurfDesc2d::Init` (`.text+0x7bc80`) stores, per slot,
   `desc->[0x10] = slot << 10` alongside the matching packet header, so
   **DWGCTL bits `[11:10]` are the destination surface slot** and `[9:8]` the
   source. The captured draw's `DWGCTL = 0x00a60ec4` selects destination slot
   **3** and source slot **2**. So the value we read out of the slot-0 base
   register was simply a different (or stale) surface.

   The two readings only reconcile if our own decoder has the slot order
   reversed — the packet header packs register indices **LSB first**, so data
   dword *i* takes header byte *i* counting from the low byte, and
   `0x21218f8e` is slot 3, not slot 0. `[INF]` but directly checkable:
   **re-check `tools/annotate-stream.py` and `tools/decode-span.py`.**

**Two corrections this forces on our tooling**, both cheap and both worth doing
before any further hardware run:

* every VRAM search must use `next_pow2(width) * bpp/8` as the stride;
* the packet-header index order in our decoders must be LSB-first.

The one open point is `MmSurfDesc::GetPhysicalMemSizeREVB` (`.text+0x74490`),
which sizes an allocation with `roundup(pitch, 32)` — 6.7 % *less* memory than
an 8192-byte stride needs for a 1920-wide surface. Either the driver stores an
already-power-of-two pitch in the descriptors it sizes, or the tail padding is
deliberately never touched. `docs/parhelia-surface-address-translation.md` §6
gives a four-outcome hardware test (one 4x4 solid fill into a surface we
program ourselves, then read 33 KB of VRAM) that settles it in a single run
without any GL or lock-up risk.

## 3. 3D context creation

### 3.1 WARP program upload — **SOLVED**, `[KERN] mtx1391` (`0x9f6b0`)

This closes the biggest gap: **WARP programs are not DMA'd from system memory.
They are written through a two-register window.**

```
0x918  WARP instruction-store write pointer, in units of 12-byte instructions
0x91c  WARP instruction-store data port, auto-incrementing, 3 dwords per instr
```

`mtx1391(dev, desc, reset, entryIdx)`:

```
if (reset) { dev->[0xa3118] = 0; dev->[0xa3121] = 0; }   /* 0x9f6ce */
size = desc->[0x18];                                     /* bytes    */
if (dev->[0xa3118] + size > 0x17ff) {                    /* 0x9f6f2 */
    dev->[0xa3121] = 1;  return 0;                       /* overflow */
}
flush the current command buffer;                        /* 0x9f769 */
<the full idle wait of §1.9>                             /* 0x9f7b3..0x9f974 */
write(0x918, dev->[0xa3118] / 12);                       /* 0x9f9dd */
for (p = desc->[0x20], n = 0; n < size; n += 12) {       /* 0x9f9f0..0x9fb05 */
    write(0x91c, *p++);  write(0x91c, *p++);  write(0x91c, *p++);
}
dev->[0xa3118] += size;
return 4 * (oldOffset / 12) + 4 * desc->[0x2c + entryIdx*4];
```

Consequences, all PROVEN:

* The instruction store is **`0x1800` bytes = 512 instructions of 12 bytes**
  (`cmp eax, 0x17ff` at `0x9f6f2`; the `/12` at `0x9f97d` is the magic-multiply
  `0xaaaaaaab`, `>>32`, `>>3`).
* A WARP instruction is exactly **3 dwords**; the upload loop is unrolled ×3 and
  advances the byte counter by `0xc` per iteration (`0x9faf7`).
* `0x918` takes an **instruction index**, not a byte offset and not a bus
  address.
* The return value — the thing that later goes into register `0x1100` — is
  `4 * instructionIndex` (`(oldOffset/3)` at `0x9fb32`, i.e. `4*(off/12)`) plus
  `4 *` a per-entry-point instruction offset from the descriptor. The
  `| 3` we see on `0x1100` in GL captures is contributed only by the *heap*
  upload path (§3.1b), where it is the memory class of the program's bus
  address. On the register-window path the entry has **zero** low bits.
* Programs are **not** in a class-3 sysmem heap. The note
  "context programs are in a class-3 sysmem heap; a captured draw image is only
  valid on its own boot" is wrong about the mechanism — the entry value is a
  store-relative index and is boot-independent. What *is* boot-dependent is any
  captured value that names a system-memory buffer (vertex data, the fence
  address), not the WARP entries.
* Every `0x91c` write is subject to the DMAPAD rule (`0x9fb58`, `0x9fb71`,
  `0x9fb8a` are the three pad branches) — with `prev == 0x91c` the rule never
  fires inside the loop, but the first `0x918` write after a `0x8f0` read does
  pad. Upload with padding enabled and it is safe either way.

### 3.1b The *second* upload path, and what `0x920` bit 0 selects — PROVEN

There are **two** program sources, and `mtx2316` (`0x9fc70`) picks between them:

```
9fc7b:  edx = dev->[0xa3114];
9fc83:  if (edx != 0) goto mtx1650;      /* system-memory heap, by bus address */
9fc88:  call mtx1391;                    /* on-chip store, via 0x918/0x91c     */
```

and `mtx3810` (`0xa3d36`) writes `0x920 = (dev->[0xa3114] != 0) ? 1 : 0`.

**So `0x920` bit 0 is the WARP program-source selector:**

| `0x920` bit 0 | program lives in | entry value written to `0x1100` |
|---|---|---|
| `0` | the on-chip instruction store, loaded through `0x918`/`0x91c` | `(instrIndex << 2) + 4*entryOff`, **low bits 0** |
| `1` | a 1 MiB system-memory heap, fetched by bus address | `4*(busAddr/12) + 4*entryOff`, then `&= ~3`, then `\|= class` |

That reconciles the two readings we have carried side by side. The heap path
(`mtx1650` `0x9fbc0`, allocator `mtx2565` `0x9fe10`): heap = `0x100` pages =
1 MiB, carved into **682 slots of `0x600` bytes = 128 instructions**, base
aligned up to a **384-byte (32-instruction) boundary**
(`aligned = ((phys + 0x17f) / 0x180) * 0x180`, `0x9ff97`); `dev->[0xa0648]` is
the bus address of slot 0, `dev->[0xa0640]` the matching CPU address, and the
class in `dev->[0xa0650]` is `3` when `chip[0x10][0xe0] & 0x400` is set,
otherwise `2` (`0x9fee3`), with both fallback allocators hard-coding `3`.

For a from-scratch driver the **register-window path is the one to use**: it
needs no heap, no bus addresses, and its entry values are boot-independent.
Write `0x920 = 0`.

**The upload is kernel-only.** `objdump -d` on `xserver/7_0_0/mtx_drv.so` and
on `opengl6/lib64/libGL.so.1.3.0` contains **no** immediate `0x918` or `0x91c`
loaded into a register-write argument — zero hits in either. Only
`mtx_parhelia.o` writes them, from `mtx1391`, whose two callers are `mtx2316`
(the ioctl path) and `mtx676` (the self-test). So a GL client uploads a vertex
program by ioctl, and an mmiotrace that starts after module load will never see
the upload. That is why no capture we have covers it, and why replaying a
captured stream on a cold card can never work: the instruction store is empty.

`mtx3810` (`0xa3c20`, step 19 of the start table) is the only caller chain that
runs at init: it constructs the WARP object, runs the self-test `mtx676`
(`0xa0230`), then writes

```
0x920 = (dev->[0xa3114] != 0) ? 1 : 0        /* bit 31 always clear */
```

so `0x920` bit 0 = "WARP instruction store is loaded / WARP enabled".
`[XDRV] mtx1373` writes `0x920 = read & 0x7fffffff`, i.e. it only clears bit 31.
`mtx656` (`0xa0e40`) does the same thing for the runtime path.

### 3.1c The block file at context create: **all zero** — PROVEN, `[KERN] mtx3107` (`0x8fef0`)

The constant-file constructor zeroes the whole thing:

```
8fef6:  ebp = 0x17f                      /* 384 iterations */
8fefc:  rbx = obj + 0x230                /* slot 0         */
8ff03:  mtx2321(rbx); rbx += 0x10        /* store 4 zero dwords */
8ff18:  LocMemSet(obj+0x04, 0, 0x34)     /* dirty bitmap  */
8ff29:  LocMemSet(obj+0x3c, 0, 0x30)
8ff3a:  LocMemSet(obj+0x70, 0, 0x30)
8ff4b:  LocMemSet(obj+0xb0, 0, 0x180)    /* six 0x40-byte descriptors */
```

**There is no static default image of the block file anywhere in the binary** —
no `.data`/`.rodata` object of 6144 bytes exists, and `mtx3107` is its only
whole-array writer. The 352 slots our capture never touches are simply **zero**.

The only slots dirtied at context create (`mtx2209` `0x8e000` bank A and its
twin `mtx867` `0x8eab0` bank B, through `mtx2702(obj, first, last)`) are:

```
0x14d ; 0x160..0x161 ; 0x172 ; 0x173 ; 0x174 ; 0x175 ;
0x178 ; 0x179..0x17c ; 0x17d ; 0x17e
```

`0x176`/`0x177` are never dirtied, so they stay zero and are never uploaded.

**The trap for a from-scratch driver:** `c[0..3]`, the MVP, is *not* in that
list. `mtx2209` copies the `.rodata` identity matrix (`mtx828`, `.rodata+0xc4840`,
exactly 0x40 bytes) at `0x8e8a3`/`0x8e8b4`/`0x8e8c5` into the **context shadow**
fields `ctx+0x174`/`+0x1b4`/`+0x1f4` — the modelview / projection / texture
stacks — not into the block file. The MVP reaches the hardware only when a draw
flushes the shadow. Start from a cold context and `c[0..3]` are zero, so the
WARP transforms every vertex to the origin and nothing rasterises. **Write
`c[0..3]` yourself before the first draw.**

### 3.2 The block/constant readback window — PROVEN, `[KERN] mtx514` (`0x8f7c0`)

```
write(0x910, ((kind & 0xf) << 24) | (((sel << 9) | index) & 0xfff));
write(0x938, 0xfffffffc);
for (i = 0; i < 4; i++) out[i] = read(0x914);
```

Four dwords come back per address, i.e. the file is addressed as vec4. `kind`
selects the file (our measurements say only `kind == 3` returns anything
stable). `0x938 = 0xfffffffc` arms the readback. `[INF]` `0x938` is a
component/byte-enable mask.

This is a read-only path — it does **not** write the block file. The block file
is written through the DMA stream, so its defaults come from whatever the
context-create code streams, not from MMIO.

### 3.3 A complete X-free, GL-free WARP run lives in the kernel — PROVEN, `[KERN] mtx676` (`0xa0230`)

`mtx676` is the WARP self-test that step 19 of the start table runs at module
load, **with no X server and no GL client alive**. It is the closest thing in
the vendor stack to the minimal bring-up we want, and it is worth copying
verbatim as a first instrument. Reconstructed, in order:

```
1.  0x920 = 0                                     /* WARP off, bit31 clear */
       [rsp+0x54] built as: &= ~1, bit31 cleared          (0xa02f3, 0xa02f8)
2.  mtx1391(...)  -> upload the test program, entry index 8, reset=1  (0xa03d7)
       ebp = returned entry value
3.  packet: count 1, tag 0x2121212d
       0x10b4 = 0                                          (0xa03f3)
4.  packet: count 4, tag 0x2e2e2e2e, repeated
       0x10b8 = <n dwords of input>                        (0xa0427)
5.  submit  (mtx1937)                                      (0xa04cb)
6.  mode = 0x10f0 value assembled as
       (v & ~0xf) | 8 ; bit31 cleared ; |= 0x20 ; (v & 0x8fffffff) | 0x20000000
       -> 0x20000028 from a zero start          (0xa04d8..0xa04fe)
7.  packet: count 3, tag 0x212c1000
       0x1000 = 0                                          (0xa0528)
       0x1040 = 4                                          (0xa052f)
       0x10b0 = 0x0002305c                                 (0xa0536)
8.  packet: count 6, tag 0x41403c3c then 0x2121213c
       0x10f0 = mode                                       (0xa055e)
       0x10f0 = mode                                       (0xa056f)
       0x1100 = entry                                      (0xa0569)
       0x1104 = entry                                      (0xa056c)
       0x10f0 = mode                                       (0xa058d)
9.  submit  (mtx1937)                                      (0xa05da)
10. the run packet, 17 dwords, header 0x4106000f:          (0xa064b)
       0x4106000f          /* opcode 0x41, count 3, totalDwords-2 = 0x0f */
       4 dwords of 0xaaaaaaaa                               (0xa05f0)
       4 dwords of 0x55555555                               (0xa0610)
       0x00000000, 0x00000040, 0x00000080,
       0x000000c0, 0x00000100, 0x00000140                   (0xa0680..0xa06a3)
       0x00000000, 0x00000000                               (0xa06aa)
11. submit  (mtx1937)                                      (0xa06ec)
12. poll   read(0x8f0) & mask == want    (mask/want from the descriptor)
                                                            (0xa072f, 0xa0771)
13. mtx612 -> mtx514: read the results back through
       0x910 / 0x938 / 0x914                                (0xa0798)
14. repeat with the 0xaaaaaaaa / 0x55555555 patterns swapped (0xa079d…)
```

The header arithmetic checks out against the known rule
`hdr = (opcode<<24) | (count<<17) | ((totalDwords-2) & 0xffff)`:
`0x4106000f` → opcode `0x41`, count `3`, `totalDwords = 0x0f + 2 = 17`, and
exactly 17 dwords are written. That is an independent confirmation of the
framing rule from a code path we had never looked at.

Two corrections this yields:

* `0x1100` here receives the entry value **without** an `| 3`
  (`0xa0569`, `ebp` is `mtx1391`'s return unchanged), and `0x1104` receives the
  same value. The `| 3` seen in GL captures is therefore contributed by the GL
  driver, not by the entry encoding itself.
* The minimum register set to make WARP execute is small: `0x1000`, `0x1040`,
  `0x10b0`, `0x10b4`, `0x10b8`, `0x10f0`, `0x1100`, `0x1104`. Everything else in
  a captured GL span is rasteriser and pixel state, not vertex-engine state.

`[INF]` The test program's own instruction bytes are not recoverable from this
function alone — the descriptor's data pointer (`desc->[0x20]`) is filled in by
`mtx376`/`mtx2051` from a table this pass did not chase.

### 3.4 Register shadowing / context save — PROVEN

`[KERN] mtx2434` maintains a shadow of the whole `0x1000-0x13ff` indexed file at
`ctx[0x1020 + i*4]`. A context switch therefore replays a shadow, which is why
a captured span writes only the registers that changed: everything else was
inherited from the context's shadow, established at context create.

`[INF]` The defaults are not in the kernel binary — it only shadows what the
client streams. They are in libGL / the Windows display driver.

---

### 3.5 The default pipeline state — PROVEN, `[KERN] mtx398` (`0x32920`)

`mtx398` is the *context-switch* hook, not a one-shot init: its only caller is
`mtx3257` (`0x32f20`, the secondary-command-list submitter) and only when
`mtx3333(dev, ctxId, &out)` reports the context has changed (`0x32f4d`). It
emits the complete default 3D pipeline state straight into the primary ring
(write pointer `ring->[0x60]`, limit `ring->[0x70]`). Some groups carry a
leading `totalDwords-2` count (e.g. `mtx1331`'s `0x0000000f`) and some are
header-less — the texture-pipe group at `0x32a6a` starts directly with its tag
dword, no count. Verify per group; do not assume one framing. About 89
writes, plus the 12 from the `mtx1331` it calls first — this is the "~109
registers". The same builder exists verbatim in `MTXPARD.dll` at
`0x2c1e5..0x2c461`, which two-sources every value below.

**1. surfaces + 2D shadow** — `mtx1331`, see §3.6.

**2. four texture pipes**, loop `i = 0..3`, first index `0x80 + i`, tags
`0x727170<idx>` and `0x76757473`, then `0x2121217a`:

| reg | value |
|---|---|
| `0x1200 + 4*i` | `0x00040100 \| (1<<i)` |
| `0x11c0` | `0` |
| `0x11c4` | `0x00000249` |
| `0x11c8` | `0` |
| `0x11cc` | `0xffffffff` |
| `0x11d0` | `0` |
| `0x11d4` | `0` |
| `0x11d8` | `0` |
| `0x11e8` | `0x00100000` — only if `dev->[0x2440] & 0x7fc00` (`0x32a77`) |

**3. strobe + setup**

| tag | reg | value |
|---|---|---|
| `0x212121dd` | `0x1374` | *(data dword deliberately left uninitialised — the strobe)* |
| `0x83828180` | `0x1200` | `0x0001000f` |
| | `0x1204`, `0x1208`, `0x120c` | `0` |
| `0x212121c0` | `0x1300` | `0x002010cc` |

**4. five fragment-combiner slots**, tag `0x9a989b99`, `slot` ∈
{`0x80`, `0x82`, `0x84`, `0x88`, `0x8a`} (`0x86` is skipped by the `i == 3`
branch at `0x32de3`):

| reg | value |
|---|---|
| `0x1264` | `slot` |
| `0x126c` | `0x00400400` |
| `0x1260` | **`0x00000080` — a literal, not `slot`** (`0x32b91`, `MTXPARD 0x2c34f`) |
| `0x1268` | `0x00400400` |

Read with `docs/parhelia-fragment-isa.md` this means five RGB instructions are
installed but only **one** ALPHA instruction, at slot `0x80`, rewritten five
times. This corrects `docs/parhelia-3d-submission-audit.md` A.5, which has
`0x1260 = slot`.

**5. the 22-dword raster/ROP block**, tags `0xbab1b0b9`, `0xb4b3b2b5`,
`0xbfc5c6c9`, `0xc2c1cbca`, `0x212121bd`:

| reg | value |
|---|---|
| `0x12e4` | `0` |
| `0x12c0` | `0xffffffff` |
| `0x12c4` | `0x7fffffff` |
| `0x12e8` | `0` |
| `0x12d4` | `0` |
| `0x12c8` | `0` |
| `0x12cc` | `0x0000ffff` |
| `0x12d0` | `0x000001ff` |
| `0x1324` | `0xffffffff` |
| `0x1318` | `0` |
| `0x1314` | `0` |
| `0x12fc` | `0` |
| `0x1328` | `0` |
| `0x132c` | `0x10001000` (Linux) / `0x0fff0fff` (`MTXPARD 0x2c3e3`) |
| `0x1304` | `0` |
| `0x1308` | `0` |
| `0x12f4` | `0x41f80000` (31.0f) |

The tag `0xb4b3b2b5` decodes LSB-first to `0xb5, 0xb2, 0xb3, 0xb4` =
`0x12d4, 0x12c8, 0x12cc, 0x12d0` and the four data dwords are `0, 0, 0xffff,
0x1ff`. That corrects `docs/parhelia-2d-accel-complete.md` §6, which has
`0x12c8` and `0x12d0` swapped; the corrected assignment is also the one that
agrees with `parhelia-pixel-pipeline-complete.md` §3.4 (`0x12c8 = 0` = depth
and stencil test off; `0x12d0 = 0x1ff` = stencil write mask `0xff` plus depth
write enable in bit 8). Two-sourced against `MTXPARD 0x2c3a8..0x2c3bc`.

**6. fence, then two capability-dependent registers**

```
tag 0xafafddc0 : 0x1300 = 0x707 ; 0x1374 = 0 ; 0x12bc = tag|1 ; 0x12bc = tag
tag 0x212121ae : 0x12b8 = (dev->[0x2440] & (0x38|0x3c0|0x7fc00))
                          ? 0x00070601 : 0x00000001
tag 0xafafddc0 : (fence again)
tag 0x212121ac : 0x12b0 = (dev->[0x2440] & 0x10) ? 0x3ff00000 : 0x3fff8000
```

The base-silicon values `0x12b8 = 0x00000001` and `0x12b0 = 0x3ff00000` are new;
the docs so far only record the feature-enabled branch. `MTXPARD` inserts one
extra step Linux omits: `tag 0x2121c0c0 : 0x1300 = 0x307 ; 0x1300 = 0x0c`
(`0x2c42f`) before the fence.

**7. the 3D context object's precomputed plane packets** — `mtx3212`
(`0x41fc0`) is the context constructor. Besides zeroing the object and setting
the `= 2` "unbound" shader sentinels at `ctx+0x598/0x59c/0x5a0` and
`ctx+0x5d8..0x5ec`, it precomputes **13 ready-made 25-dword register-write
records** at `ctx+0xc4 + n*0x64` (records 0-8) and `ctx+0x3ec + n*0x64`
(records 9-12). Every one targets only the setup/DDA plane file
`0x1160-0x119c`, in the form
`{0x1160=A, 0x1174=0, 0x1178=0, TARGET=v}, {0x1164=A, TARGET=v},
{0x1168=A, TARGET=v}`:

| record | target | `A` | `v` |
|---|---|---|---|
| 0-3 | `0x118c` / `0x1190` / `0x1194` / `0x1198` | 1.0 | 1.0 |
| 4-7 | same | 1.0 | 0.0 |
| 9 | `0x1188` | 0.0 | 0.0 |
| 10 | `0x1184` | 1.0 | 1.0 |
| 11 | `0x119c` | 0.0 | 1.0 |
| 12 | `0x1180` | 1.0 | 0.0 |

`[INF]` these are the constant (non-interpolated) defaults for the eight
interpolators `0x1180..0x119c`, in 1..4-texture-set and perspective /
non-perspective variants. Given our own measurement that the `0x1160`/`0x1180`
DDA plane engine faults a live WARP, treat this table as reference only — do
not replay it alongside a WARP program.

### 3.6 The clear path — PROVEN

It is **split**: colour is cleared by the 2D drawing engine, depth and stencil
by a dedicated 3D primitive.

**Colour — `mtx1331` (`0x3e3e0`).** Flushes the surface/2D shadow and fires a
rectangle. Main form, 17 dwords:

```
[0]  0x0000000f                    /* totalDwords-2 */
[1]  0x8d8c8988   0x1220 = dev[0xb28]   colour base  (slot 0)
                  0x1224 = dev[0xb38]   colour cfg
                  0x1230 = dev[0xb30]   depth  base  (slot 2)
                  0x1234 = dev[0xb40]   depth  cfg
[6]  0x21c5c6c9   0x1324 = dev[0xb0c]   PLNWT
                  0x1318 = dev[0xb14]   BCOL
                  0x1314 = dev[0xb18]   FCOL
[10] 0xe3c1c2c0   0x1300 = dev[0xb08]   DWGCTL
                  0x1308 = dev[0xb1c]   DSTXY
                  0x1304 = dev[0xb20]
                  0x138c = dev[0xb24]   LEN   <-- this write fires the rectangle
[15] 0x212121dd   0x1374 strobe (data dword unwritten)
```

A slot-1 variant lives at `0x3e4b0` (10 dwords, tags `0x21c58b8a` →
`0x1228`/`0x122c`/`0x1314`, then `0xe3c9c2c0` → `0x1300`, `0x1308`, `0x1324`,
`0x138c`), and an incremental form at `mtx1619` (`0x412c0`).
**`0x138c` (index `0xe3`) is the only fire register in the whole kernel
object** — exactly four tag dwords contain the byte `0xe3`, all inside
`mtx1331`/`mtx1619`.

**Depth + stencil — `mtx2187` (`0x3ccb0`)**, reached via `mtx237` → `mtx2954`.
Three ring blocks of 6, 9 and 36 dwords:

```
block 1 (6 dw)  0x00000004
                0xb4b320c9   0x1324 = 0xffffffff       PLNWT
                             PAD    = 0
                             0x12cc = arg5             stencil cmp mask | ref
                             0x12d0 = arg4             stencil write mask | Zwrite
block 2 (9 dw)  0x00000007
                0xb2cacbc0   0x1300 = 0x000000c4       DWGCTL, opcode 4
                             0x132c = 0x0fff0fff       scissor max
                             0x1328 = 0x00000000       scissor min
                             0x12c8 = arg3             depth/stencil test state
                0x2121bf20   PAD = 0 ; 0x12fc = arg2
block 3 (36 dw) 0x00000022 + seven tags that zero every interpolator plane
                (0x605e5d5b, 0x59605e5b, 0x605e5a60, 0x635e5d5b,
                 0x645e5d5b, 0x655e5d5b, 0x665f5d5b)
```

`0x1300 = 0xc4` is opcode 4, BOP `0xC` (GXcopy), src slot 0, dst slot 0 — an
opcode that does not exist in the 2D table; it is the depth/stencil rectangle
primitive. `mtx3059` (`0x3d040`) is the multi-texture / AA twin, identical
except `0x1300 = 0x002000c4` plus an extra `0x21bdbf20` → `0x12fc = arg2`,
`0x12f4 = 0x41f80000`. `MTXPARD 0x2e39d..0x2e4b0` is the same code and adds a
DWGSYNC bracket Linux omits.

**Not derivable:** none of the three blocks carries a depth *value* — the
arguments are all masks and mode words — so where `glClearDepth`'s value is
written was not located.

### 3.7 Depth / stencil binding — PROVEN

Read off `mtx1331`'s data-dword order, the device-side shadow → register map is:

| shadow | register | meaning |
|---|---|---|
| `dev+0xb08` | `0x1300` | DWGCTL |
| `dev+0xb0c` | `0x1324` | PLNWT |
| `dev+0xb14` | `0x1318` | BCOL |
| `dev+0xb18` | `0x1314` | FCOL |
| `dev+0xb1c` | `0x1308` | DSTXY |
| `dev+0xb20` | `0x1304` | |
| `dev+0xb24` | `0x138c` | LEN (fires) |
| `dev+0xb28 + 4i` | `0x1220/0x1228/0x1230/0x1238` | base, slot *i* |
| `dev+0xb38 + 4i` | `0x1224/0x122c/0x1234/0x123c` | config, slot *i* |

**Colour is surface slot 0, depth+stencil is surface slot 2** *in the kernel's
2D shadow*. Caveat: `[XDRV4]`'s `SurfDesc2dList` instantiates only **two**
`SurfDesc2d` objects and the X driver never programs `0x1230`-`0x123c` at all,
so the slot roles for the 3D pipe are not independently verified — and §2.4
shows the captured 3D draw used destination slot 3 and source slot 2. Treat the
slot assignment as programmable, not fixed, and read the slot out of DWGCTL
bits `[11:10]`.

There is no separate stencil base: one descriptor, two independent mask
registers (`0x12cc`, `0x12d0`).

**No scaling is applied to a surface base anywhere in the kernel.** `mtx1637`
(`0x23dae`..`0x23de3`) copies the surface object's `+0xd8` field straight into
the colour-base shadow `dev+0xb28` and the depth object's `+0xd8` into
`dev+0xb30`, with `+0xdc` going to the depth config — no shift, no mask, no
add. Whatever ends up in `0x1220` is exactly the number the layer above
computed. That is a direct negative answer to "is the surface base scaled or
translated": it is not.

**The surface *config* word is decodable** — `[KERN] mtx3388`
(`0x500e2`..`0x5011e`, with the two side branches at `0x505c2`/`0x505cd`):

```
cfg  = pitchInPixels;          /* low 16 bits */
cfg |= 0x00010000;             /* always set  */
switch (bitsPerPixel) {        /* surface->[0x198]->[0x14] */
  case 8:  cfg |= 0x10000000; break;
  case 16: cfg |= 0x20000000; break;
  case 30: cfg |= 0x34000000; break;   /* 10:10:10:2 "GigaColor" */
  default: cfg |= 0x30000000; break;   /* 32 bpp */
}
```

So `0x1224` / `0x122c` / `0x1234` / `0x123c` = `format<<24 | type<<16 | pitch`;
the `0x10000` this particular call site ORs in is `type = 1` (linear 2D
surface), which is right for a host bitmap. The full field map is in §2.4.
`[INF]` that the same encoder is used for the depth surface — this call site is
a blit path; the depth-specific format code (S8Z24) was not located.

`[INF]`/not derivable: the concrete S8Z24 format code that goes into `0x1234`.
No depth-format builder exists in the kernel object; the ICD computes it
(escape command `0x0c`, enums `0x4f`/`0x4b`) inside `MTXOGL.dll`.

## 4. The minimum sequence

Target: freshly reset card, no X, no vendor module, our own `dma32buf`
system-memory command buffer; render one Gouraud triangle to a VRAM surface we
choose and read it back.

| # | step | status |
|---|---|---|
| 1 | PCI: enable memory space and **bus master** in the command register | INFERRED — the kernel object only *reads* config space (`ClientPciConfigReadDword`, `mtx…+0x2414..0x2458`); bus-master enable is in the OS glue. Standard PCI. |
| 2 | Map BAR1 (MMIO) and BAR0 (framebuffer) | PROVEN — `MtxDevice_GetRegisters`, `MtxDevice_GetFrameBuffer`, `[KERN] mtx728+0x188/+0x1b4` |
| 3 | `0x8fc = 0` — mask all interrupts | PROVEN — `[XDRV] mtx1373+0x14a` (`0xddcfa`) |
| 4 | `0x920 = read & 0x7fffffff` | PROVEN — `[XDRV] mtx1373+0x19d` (`0xddd4d`) |
| 5 | `0x10b0 = 0x40` | PROVEN — `[XDRV] mtx1373+0x1af` (`0xddd5f`) |
| 6 | `0x12b8 = 0x00070601`; `0x12b0 = 0` | PROVEN — `[XDRV] mtx1373+0x777`, `+0x78b` |
| 7 | `for i = 1..0x13: r = i<<4; write(r, read(r) & 0x0fffffff)` | PROVEN — `[XDRV] mtx1373+0x790` |
| 8 | `0x000 = 0x10000000; 0x004 = 0; 0x008 = 0x10000020; 0x00c = 0x00010001` | PROVEN — `[XDRV] mtx1373+0x7cd..0x800` |
| 9 | Allocate one page of DMA32 system memory as the **status page**; write `0x400 = phys(page)` (32-byte aligned) | PROVEN — `[KERN] mtx3229+0x4d`, `mtx2508+0x1d7` |
| 10 | Record `fence0 = read(0x450)` | PROVEN — `[KERN] mtx3229+0xb7` (`0x2517`) |
| 11 | Allocate the primary command buffer in DMA32 system memory; class = 3 | PROVEN — memory classes, §2.2 |
| 12 | Idle wait: `0x8f0` bit 17 set; `read(0x450) == fence0`; `0x908`/`0x924` signature; `0x8f0 & 0x03000000 == 0` | PROVEN — `[KERN] mtx1391+0x108..0x2c4` |
| 13 | `0x920 = 0` — bit 31 clear, **bit 0 clear selects the on-chip instruction store** rather than the system-memory heap. Written *before* the upload | PROVEN — `[KERN] mtx676+0x11d` (`0xa0348`), `mtx3810+0x11b` (`0xa3d36`), dispatch in `mtx2316` (`0x9fc7b`) |
| 14 | Upload the WARP vertex program: `0x918 = 0`, then 3 dwords per instruction to `0x91c`. Entry = `(instrIndex << 2) + 4*entryOffset`, **no low-bit tag** | PROVEN — `[KERN] mtx1391`, §3.1 |
| 15a | Zero the block file, then write `c[0..3]` (the MVP) and the 14 slots of §3.1c yourself — the hardware default is **all zero**, including the MVP | PROVEN — `[KERN] mtx3107`, `mtx2209` |
| 15b | Emit the default pipeline state of §3.5 (four texture pipes, five combiner slots, the 22-dword raster/ROP block, `0x12b8`/`0x12b0`) | PROVEN — `[KERN] mtx398` (`0x32920`), two-sourced against `MTXPARD 0x2c1e5` |
| 15c | Bind the surfaces: `0x1220`/`0x1224` = colour base/config (slot 0), `0x1230`/`0x1234` = depth base/config (slot 2). `cfg = format<<24 \| type<<16 \| widthInPixels`; with type 2 the **row stride the hardware uses is `next_pow2(width) * bpp/8`**, so size and read back the surface accordingly | PROVEN — `[KERN] mtx1331` (`0x3e3e0`), `[XDRV4] MmSurfDesc::GetHostAperturePitch`, §2.4 |
| 15d | Clear the colour surface with the 2D rectangle of §3.6 (`0x1300` DWGCTL, `0x1314`/`0x1318` colours, `0x1308`/`0x1304` position, `0x138c` LEN fires it); clear depth/stencil with the opcode-4 primitive if you use them | PROVEN — `[KERN] mtx1331`, `mtx2187` (`0x3ccb0`) |
| 15e | Bind the WARP program: `0x10f0` MODE, `0x1100` entry, `0x1374` strobe | PROVEN — §3.3, `[KERN] mtx676` |
| 16 | Append the fence: tag `0x21212425`, `<tag>` → `0x1094`, `phys(statusPage)+0x20` → `0x1090` | PROVEN — `[KERN] mtx1017+0x1ba`, `mtx2095+0x9f` |
| 17 | If the buffer is in VRAM: poll `0x31c`/`0x318` until `(v & 0x57) == 0x50` | PROVEN — `[KERN] mtx1937+0x146` |
| 18 | `0x404 = busaddr(start) & ~3` | PROVEN — `[KERN] mtx2508+0x156` |
| 19 | `0x408 = busaddr(end) \| 3` — the doorbell | PROVEN — `[KERN] mtx1937+0xe6` |
| 20 | Poll `read(0x450) == tag` | PROVEN — `[KERN] mtx2508+0xb8` |
| 21 | Read the surface back out of VRAM | — |

**Do step 15 in two stages.** §3.3 gives a *complete, proven, X-free and
GL-free* sequence that exercises the vertex engine with eight registers
(`0x1000`, `0x1040`, `0x10b0`, `0x10b4`, `0x10b8`, `0x10f0`, `0x1100`,
`0x1104`) and reads the result back through `0x910`/`0x914`/`0x938` — no
rasteriser, no surface, no pixel pipeline. Get that working first: it isolates
"our ring executes and the WARP unit runs our program" from "the raster and
pixel state are right", which is the ambiguity that has cost us the most time.
Only then add the rasteriser state and the surface bases.

**Every step is now derivable.** The gap the brief named — "the initial contents
of all 384 block-file slots" and "the default values of the ~109 pipeline
registers" — is closed: the block file has no default image at all (it is
memset to zero, §3.1c) and the pipeline defaults are the explicit table in §3.5.
The two residual unknowns inside step 15 are the depth *format* code for
`0x1234` and the depth *value* for a depth clear (§3.6, §3.7); neither is needed
for a colour-only Gouraud triangle.

---

## 5. Corrections to earlier documents

| document | claim | correction | sources |
|---|---|---|---|
| `parhelia-engine-init.md` | `0x8fc = 0` means "hardware DMA, not emulated" | `0x8fc` is the interrupt **enable mask**; `0` masks everything (§1.3) | `[KERN] mtx1769/mtx2689/mtx3091` |
| `parhelia-engine-init.md` | `0x3a8 \|= 0x20` is part of the common init | conditional on device id `0x2539` and `dev->[0x2a0] == 0` | `[XDRV] mtx1373+0x8de` |
| `parhelia-engine-and-context-init-complete.md` | `0x400` is the ring limit | `0x400` is the status/write-back page base = `phys(shared buffer)` (§1.5) | `[KERN] mtx728/mtx3229/mtx2508` |
| `parhelia-2d-accel-complete.md` §6 | `0x12c8 = 0x1ff`, `0x12cc = 0xffff`, `0x12d0 = 0` | `0x12c8 = 0`, `0x12cc = 0xffff`, `0x12d0 = 0x1ff` | `[KERN] mtx398`, `MTXPARD 0x2c3a8` |
| `parhelia-3d-submission-audit.md` A.5 | combiner block has `0x1260 = slot` | `0x1260` is a literal `0x80`; only one ALPHA instruction is installed | `[KERN] mtx398+0x271`, `MTXPARD 0x2c34f` |
| `parhelia-vertex-fetch-and-memory-classes.md` §4.1, `parhelia-engine-and-context-init-complete.md` §2.2 | WARP program object fields at `prog[0x24]` / `prog[0x30+i*4]` | those are the **Windows** offsets; the Linux object uses `prog[0x18]` length, `prog[0x20]` data, `prog[0x2c+i*4]` entries | `[KERN] mtx1391` |
| memory note "WARP program heap" | programs are *always* in a class-3 sysmem heap | two paths; `0x920` bit 0 selects, and the on-chip store path uses `0x918`/`0x91c` with boot-independent entry values (§3.1, §3.1b) | `[KERN] mtx2316/mtx1391/mtx1650/mtx3810` |
| — | `0x132c` default | driver-dependent: `0x10001000` (Linux) vs `0x0fff0fff` (Windows) | `[KERN] mtx398`, `MTXPARD 0x2c3e3` |
| `parhelia-rendertarget.md` | cfg `[13:0]` is the row stride | it is the surface **width**; the stride is `next_pow2(max(width,32)) * bpp/8` for surface types 2..5 (§2.4) | `[XDRV4] MmSurfDesc::GetHostAperturePitch` |
| `tools/annotate-stream.py`, `tools/decode-span.py` | packet-header index order | indices are packed **LSB first**; a top-byte-down decoder reverses the surface slots (§2.4) | `[XDRV4] SurfDesc2d::Init` |
| this document, first draft | index `0x20` consumes no data dword | it **does** — the payload fetch at `0x39960` precedes the `0x1080` test at `0x399aa`. `0x21` is the zero-cost terminator (§1.10) | `[KERN] mtx2434` |
| `parhelia-dma-ring-working` | the vendor pads a short tag with index `0x20` | the vendor pads with `0x21` (terminator, no dword) — see the headers `0x21218988` / `0x21218b8a` / `0x21218d8c` / `0x21218f8e`. Our `0x20` filler works on hardware; match the vendor when replaying | `[XDRV4] SurfDesc2d::Init` (`0x7bda0`), `[KERN] mtx2434` |
| `randr-native-displaywidth-pow2-stride.patch` | empirical "the VBIOS pads to a power of two" | now **explained**: `MmSurfDesc::GetHostAperturePitch` forces a power-of-two pitch for every tiled/textiled surface type (§2.4) | `[XDRV4] 0x745c0` |
| — | `0x0000-0x013f` is an unknown "aperture block" | it is the host aperture descriptor table, 20 slots × 4 dwords, fields decoded in §2.3 | `[XDRV4] MmHostAperture` |

## 6. What remains underivable from these binaries

1. **Whether a type-2/3 surface allocation is really sized at the power-of-two
   stride.** `MmSurfDesc::GetPhysicalMemSizeREVB` sizes it with
   `roundup(pitch, 32)`; §2.4 lists the one hardware test that settles it.
2. ~~**The depth-surface format code for `0x1234` and the depth clear value.**~~
   The **format is SOLVED** (§7); only the clear *value* is still open, and it
   is open only because no capture has ever issued a depth clear.
3. **How a 256 MB `0x0528` addresses VRAM above `0x08000000`.** The offset map
   of §2.3b has no room for it, yet the miniport sizes such boards correctly
   (PInS code 3). Settle this before allocating high VRAM.
4. **Bit meanings of `0x8f0`, `0x908`, `0x924`, `0x31c`, `0x300`, `0x308`,
   `0xbc0`, `0x938`.** Only the masks and compare constants the driver uses are
   recoverable; the field layouts are not encoded in the code.
5. **The `memType 4`/`5` bus class.** The switch in `MmSurface::AttachToBlock`
   says 0, the table at `[XDRV4] 0x3340c0` says 3. `memType 4` looks like a
   "no allocation" pseudo-type, so this may never matter.
6. **PCI bus-master enable and the BAR layout** come from the OS glue, which is
   not in `mtx_parhelia.o`.


## 7. The depth surface — SOLVED (2026-09-07)

Settled from the wire plus `[XDRV4] xserver/7_4_0/mtx_drv.so`, not inferred.

### 7.1 It is bound in slot **1**, not slot 2

Snapshotting the surface registers as of every 3D draw in
`captures/gl-matrix/*-secs.txt` gives, in every variant:

| slot | base | cfg | decoded |
|---|---|---|---|
| 0 | `0x1220 = 0x02000000` | `0x1224 = 0x32020780` | format 0x32, type 2, width 1920 — **colour** |
| 1 | `0x1228 = 0x01000000` | `0x122c = 0x36020780` | format **0x36**, type 2, width 1920 — **depth** |
| 2 | `0x1230 = 0x00000000` | `0x1234 = 0x32020000` | width 0 — **unbound** |
| 3 | `0x1238 = 0x00000000` | `0x123c = 0x32020000` | width 0 — **unbound** |

This corrects §3.6 and `parhelia-3d-state-complete.md`, which put depth in slot
2 on the strength of the kernel's clear helper naming `0x1230`/`0x1234`
"depth". The kernel helper can address any slot; the GL path binds slot 1.

### 7.2 Format `0x36` is 32 bpp, i.e. S8Z24

`MmSurfDesc::GetBitsPerPixel` (`[XDRV4] 0x73e70`) is a plain switch on cfg bits
`[29:24]`. Extracted mechanically:

| format | bpp | | format | bpp |
|---|---|---|---|---|
| 0x02, 0x05 | 1 | | 0x2a, 0x2b, 0x2c, 0x2e | 16 |
| 0x09, 0x0b, 0x10, 0x12 | 8 | | **0x32** | **32** (ARGB8888, colour) |
| 0x21, 0x22, 0x25, 0x27 | 16 | | 0x34, **0x36**, 0x38, 0x3a | **32** |

`0x36` reaches the `mov $0x20` arm at `0x73f68`. So the depth surface is
**32 bits per pixel** — 24 depth + 8 stencil — even though `gltest` asks for
`GLX_DEPTH_SIZE 16`: the ICD exposes only the 32-bit depth/stencil surface and
GLX treats the request as a minimum.

`0x35` and `0x37` appear in the kernel's capability table
(`[KERN] mtx1848` `0x46ae0`, five `{format, componentCount}` pairs: `0x17`/3,
`0x38`/3, `0x35`/1, `0x36`/1, `0x37`/1 — the single-component group is the
depth family) but **not** in `GetBitsPerPixel`; the X driver never handles
them, so their widths are not recoverable from this binary.

### 7.3 Depth state registers, from the draw-time attribution

| reg | meaning | evidence |
|---|---|---|
| `0x11f4` | **depth-test enable**, 1 = on | the only state change of the `depth` variant vs `base` |
| `0x12d8` / `0x12dc` | depth range min / max, 24-bit fixed point — `0x00000000` / `0x00ffffff` = GL's default `[0,1]` in S8Z24 | constant in every capture |
| `0x12c8` `[7:0]` | depth/stencil test state; `0xe4` → `0xeb` under `glDepthMask(GL_FALSE)` | `depthmask` variant |
| `0x12d0` bit 8 | set by `glDepthMask(GL_FALSE)` (`0x0ff` → `0x1ff`) | `depthmask` variant |

`0x11f4 = 1` also explains an earlier result recorded as a texture-path
mystery: setting it during the texture bisection zeroed the draw. That is
correct hardware behaviour — depth test against an unbound depth surface fails
every fragment.

### 7.4 The clear value is untested, not underivable

`gltest.c` only ever issued `glClear(GL_COLOR_BUFFER_BIT)`, so **no capture in
the tree contains a depth clear at all** — filtering every `*-secs.txt` for the
clear primitive's `0x1300 = 0x000000c4` / `0x002000c4` returns zero packets.
`GLTEST_CLEARDEPTH` and `GLTEST_DEPTHVAL` have been added to `tools/gltest.c`;
one capture with two different clear values will locate the register by
diffing, exactly as `0x11f4` was found above.


## 1.2b HAL step 12, `HSLS90InitMemClk` — DECODED, partially implemented

`[XDRV] mtx1945` (`0xdcef0`). Every value is **board specific and comes from
the VBIOS PInS block**, so nothing here can be hardcoded; `tools/parhelia-3dtri.c`
reads the option ROM through sysfs (`--initmemclk`, with `--pinsfile` as a
fallback when the ROM window is closed).

```
read 0x1c00,0x1c04,0x1800,0x1a00
0x1c00 |= 0x20 ; 0x1c04 |= 0x20        hold both DC clocks
0x1800 &= ~1  ; 0x1a00 &= ~1           both display heads OFF
HWDelay(1000)
HWS90ResetMemoryController()           = mc_write(0xff4, ctrl 3, 0)
HWS90InitializeSystemClock()
HWS90InitializeMemoryController()
```

**`SelectS90MemoryController`** reduces to `write(0xffc, ctrl)` for ctrl 1/2/3.
**`WriteS90MemoryControllerDword(reg, ctrl, val)`** = save `0xffc`, select,
write `reg`, restore `0xffc` if it changed.
**`WaitForS90MemoryIdle`** spins on `ReadS90MemoryControllerDword(0xff8, ctrl 1)`
until `(v >> 8) & 6 == 6`.

**`HWS90InitializeSystemClock`** — PLL words come from `PInS+0x200/0x204/0x208`:

```
read 0xe00,0xe04,0xe08
write each |= 1 ; then write each = 1 ; then write each = 0
HWDelay(1000)
if (IsScUseFS_SUN90(...,0)) { v = PInS+0x28; 0xe30 = v|0x80000000; delay(100); 0xe30 = v; }
if (IsScUseFS_SUN90(...,1)) { ... 0xe34 ... }        <- predicate NOT decoded
```

**`HWS90InitializeMemoryController`** — 44 entries of `pinsDatasConvertor`
(`[XDRV] .data 0x34b580`, `{pinsOff, reg, ctrl}`), value = `PInS[0x210 + off]`:

| ctrl | registers |
|---|---|
| 3 | `0xf00 0xf04 0xf08 0xf10 0xf1c 0xf20 0xf24 0xf28 0xf2c 0xf44 0xf94 0xf98 0xf9c 0xfa0 0xfc0 0xfc4 0xfc8 0xfcc 0xff0 0xff8` (pins `0x00`..`0x4c`) |
| 1 | `0xf14 0xf18 0xf48 0xf4c 0xf58 0xf5c 0xf68 0xf6c 0xf70 0xf74 0xf8c 0xf90` (pins `0x50`..`0x7c`) |
| 2 | the same twelve registers (pins `0x80`..`0xac`) |

Measured on the test board: PInS at ROM offset `0x7800`, version `0200`, clocks
`01610097 000c028b 00000328`, first controller words `2000f400 5053ffff
2608c383 2403438a`.

**NOT yet implemented, and this is why the partial form is harmful:**
1. `WaitForS90MemoryIdle()` after the 44 writes.
2. The tail: `0xf00 |= 0x40000000` (ctrl 1 read, ctrl 3 write), wait idle,
   `HWS90ResetMemoryController()` **again**, then further writes sourced from
   `PInS+0x268`, `+0x26c`, …
3. The FS-PLL branches (`0xe30`/`0xe34`) — `IsScUseFS_SUN90` not decoded.
4. Restoring the display heads and the DC clock hold afterwards.

**Measured:** with only parts 1-3 of the sequence above, the 2D instrument goes
from 120000 px to 0. Do not enable `--initmemclk` until the tail is finished.

## 1.2c Status of from-scratch bring-up (2026-09-07)

| | result |
|---|---|
| 2D from cold, `mtx` never loaded, X down | **120000/120000 px, reproducible** |
| engine init steps 1-6, 8-11, 13-16 + §1.4 bus pair | verified harmless, engine still draws |
| step 12 | decoded, partial, **harmful** - off by default |
| 3D replay from cold | wedges the engine at a **varying** packet (14, 20, 21 across runs) |

The 3D stall point moving between identical runs is the open lead. The
per-packet stall detector is a fixed poll budget (`--polls`, default 200000),
so a spurious timeout has not been excluded - but the first 3D attempt wedges
the engine, so a second measurement in the same boot is worthless. Any test of
this needs one reboot per data point.


## 1.2d Cold 3D bring-up: two missing context steps found and implemented

Both are things the vendor's context create does that a captured stream silently
assumes, and neither was in our path.

**1. The WARP instruction store was empty.** §3.1b already said it -
"replaying a captured stream on a cold card can never work: the instruction
store is empty" - but our `--warpprog` used the *heap* path (entry =
`4*(bus/12) | 3`), which `0x920` bit 0 = 1 selects. Cold, `0x920` reads 0, so
the hardware was looking at the on-chip store. `--warpstore` now uploads
through the documented window (`0x918` = instruction index, `0x91c` = data
port, 3 dwords per instruction) and yields the boot-independent entry
`4*instrIndex + 4*entryOff`.

**2. The constant file was never zeroed.** `mtx3107` zeroes all **384 slots**
at context create; our recipe loads 34, leaving 350 holding undefined cold
state. `gen-stream.py --zeroblocks` prepends twelve 32-slot zero loads.

**Measured effect, cold, `mtx` never loaded:**

| configuration | cold 3D replay |
|---|---|
| neither | stalls at a **varying** packet - 14, 20, 21 across identical runs |
| `--zeroblocks` only | stalls at packet 2, i.e. on a zeroing load itself |
| `--warpstore` + `--zeroblocks` | **deterministic**, stalls at recipe packet 20; all 12 zeroing loads pass, `0x41c = 0x1a0` (syncs retiring) |

The remaining stall is **positional, not slot specific**: removing the block
load at slot `0x149` with `--skipblocks 0x149` moved nothing - the next load
takes its place at the same recipe packet. So it is not a bad constant address;
something becomes true after a fixed amount of the recipe.

Still unimplemented and still candidates: HAL step 12's tail (§1.2b), the
per-chipset `mtx3376` fixups, HAL step 7, and the aperture block. Also unproven:
whether the per-packet doorbell stepping (`--polls`, default 200000) is itself
declaring a false stall - the first cold 3D attempt wedges the engine, so each
data point costs a reboot.


## 1.2e Comparing our init against the vendor's by register state diff

**mmiotrace cannot see the vendor's engine init.** It hooks only kernel
`ioremap` regions, and the vendor X driver maps the BAR from userspace, so its
HAL writes never fault through the tracer. Two capture attempts across
`modprobe` and across X startup both returned **zero** writes. `mtx1373` is
reachable by static RE only.

What does work is a **state diff**: `3dtri --dumpregs` prints a curated set of
registers (the documented init and engine ranges - not a blind sweep), so cold,
post-our-init and post-vendor-X states can be compared. Measured on the 0x0528:

| | registers |
|---|---|
| we set, **matching the vendor** | `0x300`, `0x308`, `0x1c08` |
| we set to a **different** value | `0x1c00` - ours `0080e0e2`, vendor `0080f1e2` (bits 0 and 4) |
| the vendor sets, **we do not** | `0x004`, `0x008`, `0x00c`, `0x0304`, `0x0e10`, `0x0e14`, `0x0e1c`, `0x1a00`, `0x1c04` |

This confirms the §1.4 pair: `0x308 = 0x83` on the 0x0528 and the `0x300`
read-modify-write both land exactly where the vendor leaves them.

### The aperture block is not the hazard we recorded

The vendor programs `0x004/0x008/0x00c`, and our gated-off step 17-21 code
already writes **exactly those values**. Run cold with `mtx` unloaded and X
down, `--initaperture` leaves the register file matching the vendor
(`10000000 00000000 10000020 00010001`) and the 2D instrument still draws
120000/120000 px. The earlier "the aperture block RESETS THE MACHINE" note
holds only under a **live VBE console**, whose scanout is fed through the
apertures it disarms. Cold, it is safe and correct.

### Do not copy the remaining values

Forcing the other seven registers to the vendor's measured values after our
init takes the 2D instrument from 120000 px to **0**. The FS PLL words
(`0x0e10/0x0e14/0x0e1c`) need their programming protocol - hold, strobe,
divider fields, as `parhelia_program_dc_clock` does in the open driver - not a
raw write, and `0x1a00` enables a display head whose timing has not been
programmed. `[XDRV] mtx769` only *reads* these to compute frequencies; the
writer has not been located yet.

**Remaining gap for cold 3D, precisely:** `0x0304`, the FS PLL trio, `0x1a00`,
`0x1c04` bit 8, and `0x1c00` bits 0 and 4 - each to be implemented with its own
sequence, plus HAL step 12's tail (§1.2b).


## 1.2f The remaining init diff is ALL display — the engine init is complete

Following each of the six registers into the vendor binaries rather than
copying its value:

| reg | what it is | evidence |
|---|---|---|
| `0x0e10`, `0x0e14`, `0x0e1c` | **PLL0, PLL1, PLL3** - `PARHELIA_PLL(n) = 0x0e10 + n*4`, i.e. pixel clocks | `src/parhelia_reg.h`; `[XDRV] mtx1324` builds the FS descriptor `0xe10 + fs*4` with frequency limits from PInS |
| `0x1a00` | display head 1 enable (bit 18 = PLL locked) | our own modeset code |
| `0x1c00` bits 0/4, `0x1c04` bit 8 | DC clock controllers | §1.2 steps 2/3 and `parhelia_program_dc_clock` |
| `0x0304` | a **bit-flag** register with paired accessors in the display code: `[XDRV] mtx1250` clears bit 0, `mtx2734` sets bit 0, `mtx2140` clears bit 4 | `0xf5c70`-`0xf5d30`, adjacent to the clock-division helpers |

The PLL words are **computed, not stored** - none of the vendor's three appears
anywhere in the option ROM, and `mtx1324` reads only min/max limits from PInS.
They must be solved the way `parhelia_program_dc_clock` already does
(`Fout = 27MHz*N*P/X/B3`) and written with the `word | bit31` load strobe.

**RETRACTED (2026-09-07).** This section originally claimed that forcing the
seven remaining registers to the vendor's values, and `0x0304 = 0x00001010`
alone, each took the 2D instrument from 120000 px to 0. **Both results were a
harness artefact, not hardware.** Those scripts ran
`./3dtri --engineinit ... >/dev/null` with **no terminating action**, so main
fell through to the built-in WARP triangle draw, which hangs and kills the
engine until reboot - the trap already recorded in §1.2. Re-measured with
`--dumpregs` terminating every init command:

```
cold 2D                       120000 px
after engine + aperture       120000 px
after 0x0304 = 0x1010         120000 px
after all six display regs    120000 px
```

**None of them harms the engine.** The same mistake also produced a transient
claim that the ported PLL programming and `dc_route()` broke 2D; that was the
same fall-through. Any hardware measurement here must give every `3dtri`
invocation an explicit terminating action.

**Conclusion: the register-level ENGINE init is complete.** Everything the
vendor does that we do not is display bring-up, which an offscreen cold 3D draw
does not need. So the cold 3D stall at recipe packet 20 is **not** an
engine-init gap - it has to be in the command stream or the 3D context state,
and that is where to look next. Chasing more init registers is the wrong tree.


## 1.2g Full vendor register parity reached - and 3D still stalls

With `--engineinit --initaperture` plus the seven registers set to the values
the vendor X driver leaves on this board, our register state matches the
vendor's across the whole curated set, and the 2D instrument stays at
120000/120000 px. The cold 3D replay stalls at exactly the same recipe packet
as before.

That is the clean version of the §1.2f conclusion, on valid data: **the
register-level GPU init is complete and is not what is stopping 3D.** The
remaining defect is in the command stream or the 3D context state.

What is implemented and verified cold, `mtx` never loaded, X down:

| | |
|---|---|
| `mtx1373` steps 1-6, 8-11, 13-16, **17-21** | ✓ |
| §1.4 bus pair, `0x300` RMW and `0x308 = 0x83` | ✓ |
| WARP program in the on-chip store via `0x918`/`0x91c` | ✓ |
| constant file zeroed, 384 slots | ✓ |
| the seven remaining vendor registers | ✓ (values measured, not derived) |
| 2D engine after all of it | **120000/120000 px** |

Still not implemented: HAL step 7 (`0x1c50` from PInS), step 12's tail
(§1.2b), and the FS PLL *encoding* - the vendor's words for `0x0e10/0x0e14/
0x0e1c` do not decode under the DC-clock field layout (they give VCO1 =
1350 MHz, outside the documented window), so those values are currently copied
rather than solved.


## 1.2h COLD 3D COMMAND SUBMISSION WORKS (2026-09-07)

With `mtx` never loaded, X down, and a genuine power cycle, the whole 32-packet
3D stream is now consumed and the engine stays healthy:

```
cold 2D                              120000 px
writeback: 0x400 = 16c95000 (readback ok)
warpprog: on-chip store, 11 instructions, 9 placeholders patched
ringreplay: 43 packets kicked, all idle
engine: idle 1
```

Four defects had to be fixed, in this order. Each was found by decoding the
stalling packet, not by permuting flags.

**1. Only one of nine WARP entry registers was patched.** `0x10c0`-`0x10d8`
and two earlier `0x1100` writes carried `075579bf`-style bus addresses of the
vendor's programs *from the capture boot*. On any other boot they point at
memory holding no program. `gen-stream.py` now emits a placeholder for every
entry above `0x01000000` (values below that are counts/classes in the same
registers); the tool patches all nine to our on-chip entry, which is
boot-independent.

**2. The constant-file zeroing was emitted in the wrong place.** `--zeroblocks`
put the twelve zeroing loads at the *head* of the stream, before the register
writes that configure the WARP unit. The block file belongs to that unit, and
a block load submitted to an unconfigured one stalls a cold engine at the very
first packet - confirmed with a minimal 133-dword stream containing nothing but
one block load, which stalls under **every** init condition. The zeroing is now
inserted immediately before the recipe's own first block load.

**3. `0x400`, the write-back page, was never armed.** It is the physical
address of the driver's shared status structure. Cold it reads 0, and the
DWGSYNC drain and surface fences cannot retire without it. `--writeback400`
parks a page-aligned scratch page in our own DMA buffer and programs it.

**4. The stream armed BLOCK-UNTIL-RETIRE and then waited forever.** The
stalling packet was not a sync opcode - it was the vertex-fetch/surface group,
which *contains* `0x1300 = 0x407` followed by `0x12bc = tag|1` then `tag`. The
odd `0x12bc` write arms block-until-retire; `0x41c` sat at `0x1a0` waiting for
`0x1a8`, forever. The sync is ordering only and not needed to draw, so
`--nosync` drops the whole group (31 register writes).

**AND IT RASTERISES.** On a genuine cold boot, with the render target placed
**below 16 MB**:

```
after --vramzero        0 px
draw with px-red     8051 px
draw with px-white   8824 px, different colours
```

Different vertex data gives a different pixel count and different colours, so
these are ours. De-tiled it is a clean triangle, bbox x 1..271, y 0..191.

**A triangle now renders from our own command stream on a cold engine with no
vendor code in the path at all** - no `mtx`, no X, no ICD.

**The render target must be below 16 MB.** That is the same boundary as the CPU
write limit ([[parhelia-bar0-16mb-write-limit]]): HAL step 12's 44-entry PInS
table programs memory controllers 1, 2 and 3, and without it only the low bank
appears usable. With the RT at `0x02000000` the draw retires but nothing
appears.

**A trap that cost a wrong conclusion:** an earlier run concluded "the draw
rasterises nothing, proven by colour rotation". That test scanned
`0x02000000`, where `--vramzero` silently does nothing because CPU writes above
16 MB are dropped - so it compared two uncleared leftovers and found them
identical. Always clear and scan below 16 MB, or verify the clear took.

**Method note.** `reboot -f` is a WARM reboot: the VBIOS re-POSTs enough to
restore 2D, but the WARP instruction store and the constant file are not
touched, so consecutive runs are **not** independent cold samples. A result
that reached packet 41 was later shown to be residual state - on a genuine
power cycle the same stream stalled at packet 0. Any cold-3D claim needs a real
power cycle, or at least must be reproduced after one.
