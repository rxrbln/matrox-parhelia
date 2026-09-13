# Parhelia 3D command submission — consolidated model and claim audit

> **Superseded in part — see `parhelia-3d-integrated.md` §8.** Its "opcodes only via a secondary buffer" reading is half right: the call's START low bits select the section framing (`|2` header-ful, `|0` header-less). `0x12b0 = 0x3ff0f800` and the `reg = 0x1000 + slot*4` registers are refuted.
Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Scope: how a 3D command reaches the Parhelia-512 (0x0528) engine — ring,
framing, register file, block file, WARP binding, the record stream and its
translator, draw packets, fences, doorbell. Every claim carries a tag:

| tag | meaning |
|---|---|
| **[BIN]** | read out of a vendor binary in this pass (`mtx_parhelia.o` = kernel core, `mtx_drv.so` = X HAL, `libGL.so.1.3.0` = ICD, `MTXPARD.dll` = Windows D3D). Addresses are `.text` offsets. Where a doc's binary reading was relied on without re-reading it here, the row says *[BIN, doc]* |
| **[HW]** | measured on a board (0x0528 or P690), or the hardware truth established 2026-09-05 |
| **[INF]** | inference, self-consistent but unproven |

Verdicts: **CONFIRMED** (binary and/or hardware agree), **CONTRADICTED** (with
the source that contradicts it), **UNVERIFIED** (no evidence either way).

Spot-checks performed for this audit, so the table can be re-derived:
`mtx1342` 0x90010, `mtx693` 0x436a0, `mtx1937` 0x32360, `mtx2196` 0x4acb0,
`mtx2393` 0x3c2d0, `mtx2147` 0x4b170, `mtx1450` 0x4c760, `mtx1132` 0x4c730,
`mtx3319` 0xb610, `mtx398` 0x32920, `mtx558` 0x45030–0x451e7 and 0x45c26–0x45cd1,
`mtx3400` 0x43a3b–0x43a81 / 0x44009–0x44021, caller sets via `readelf`-style
relocation scan, literal scans of all four binaries.

---

## A. The corrected model of register-level 3D submission

### A.1 Memory spaces and addresses

| item | statement | tag |
|---|---|---|
| Space tag | Every GPU address carries a 2-bit space selector in bits[1:0]: `0` = local framebuffer (VRAM byte offset), `2` = system heap type 1, `3` = system heap type 2. `mtx1132` @0x4c730: `addr = offset + obj[0x18]; if obj[0x20]==1 addr|=2; if ==2 addr|=3` | [BIN] |
| Ring memory | The command ring must be physically contiguous GFP_DMA32 system memory. A ring in the framebuffer BAR overruns and locks the engine | [HW] |
| Vertex data for address-mode draws | VRAM byte offset with tag 0 works; a raw CPU physical address of system memory took the machine down | [HW] |
| Where the tag lives for vertices | For the kernel's own draws the tag is written to register `0x1000` (see A.5) and the packet addresses are `buf[0x58] + offset` without the tag (`mtx2393` 0x3c6db, 0x3c73c–0x3c7b0) | [BIN] |
| Client chunk addresses | `mtx2196` passes the DRAW record's **region-5 byte offsets** (`0x2000`, `0x200c`…) OR'd with 2 straight to SECADDRESS/SECEND (0x4ada0–0x4adbb, 0x4b038–0x4b054). No base is added in `mtx2196`/`mtx1937`. What makes a 0x2000-relative offset resolvable to the engine is **not identified** (per-draw aperture-descriptor writes at `0x0030..0x003c` are observed in the gltri mmiotrace) | [BIN] + [HW]; resolution [INF], see D.1 |

### A.2 The primary ring and the doorbell

| register | role | tag |
|---|---|---|
| `0x404` PRIMADDRESS | ring fetch pointer. Written at ring (re)start with the low two bits **cleared** (`mtx2508`, kernel-abi §6.4); reads back the fetch position; `0x404 == end` means fetched, not executed | [BIN, doc] + [HW] |
| `0x408` PRIMEND | doorbell. `0x408 = ringBusBias + writePtr`; the bias carries the space tag, hence `end|3` for a DMA32 ring. Written once per submit (`mtx1937` 0x3243c–0x3244b) | [BIN] + [HW] |
| `0x400` | status write-back pointer (`mtx2508`), not a limit | [BIN, doc] |
| `0x384` DMAPAD | write-posting pad, written 0 when the previous register offset was within 128 bytes below; also written before `0x408` when `pStatus[0x88]` says so (`mtx1937` 0x3241d–0x32432, 0x32541) | [BIN] |
| `0x314 = 1` | front-end kick, written in bursts by the X driver; purpose unknown | [HW] |
| `0x8f0` | bit17 = bus-master idle; bits[25:24] = WARP busy; `0x03000000` steady = locked engine | [HW] |
| `0x450` | full-equality idle test against an init-time snapshot (`WaitBusMastering`); bit22 = indexed-write FIFO empty | [BIN, doc] |
| `0x41c` | tag of the last retired DWGSYNC | [HW] |
| `0x318`/`0x31c` | pre-submit bus gate, taken only when `endFlags == 0` (`mtx1937` 0x32473–0x324f0) | [BIN] |
| `0x300` | **16-bit**; high half is read-only status. Vendor holds `0x140`; arm = `(v & 0xffff) | 0x140` | [HW] |
| Doorbell granularity | `0x408` advances `0x20` per write in the capture; `mtx1937` appends `0x14`; per RENDER the primary receives ≥ 2×0x14 + 0xc | [HW]/[BIN]; reconciliation [INF] |

### A.3 Packet framing

| rule | statement | tag |
|---|---|---|
| Header | `hdr = (opcode << 24) | (groups << 17) | ((totalDwords − 2) & 0xffff)`; `groups` = (16-byte groups after the header) − 1, 6-bit, clamped to 63 (`mtx1342` 0x901f0–0x9022c); 0 in every opcode-0x00 register list observed | [BIN] + [HW] |
| Exception | opcode `0x07` (packed 16-bit index list): low16 = **indices − 1**, so `0x07000001` carries one payload dword (`libGL` 1c4c95; `p_lineloop` capture) | [BIN ICD] + [HW] |
| Opcode 0x00 = register list | `[hdr][tag][data…][tag][data…]`. A tag packs four register indices LSB-first, `reg = 0x1000 + idx*4`; index `0x21` ends the **tag** (no data), index `0x20` pads and consumes one data dword. Several tags may sit under one header: `mtx2393` 0x3c4de emits `[7][0x2121dd87][0][0][0x83828180][0x1000f][0][0][0]` | [BIN] |
| Header-less form | `[tag][data…]` with no count works for pure register writes (the 2D fill draws) and desyncs the parser as soon as an opcode appears in the same stream | [HW] |
| Opcodes seen | `0x00` register list; `0x03`/`0x07`/`0x8f` draws (ICD: `0x03000000 | ctx[0xa040] | ctx[0xa044] | count`; `0x8f` = sequential ranges, `0x07` = packed indices); `0x17` address-mode draw (kernel `mtx2393` 0x3c7aa, `mtx1017` 0x676d; Windows 0x2e0ce and `or reg,0x17000001` ×3); `0x41` block load / bulk data; `0x0f` control; `0xd7` (opcode-41 doc) | [BIN] |
| Opcode 0x41 | `0x41000000 | (n<<17) | (n*4+3)`, then `n`, `first`, `1`, `0`, then `n` vec4 — `mtx1342` 0x901d6–0x9023c. `mtx676`'s `0x4106000f` + 16 dwords is a **different** 0x41 packet (no descriptor: two test vec4 + six scatter bases) | [BIN] |
| Primary vs secondary content | In the vendor, the **primary ring only ever receives header-less tag packets**: the 5-dword secondary call (`mtx1937` 0x323ce), the kernel's 3-dword fence `[0x21212425][seq][addr]` (`mtx558` 0x451dd, `mtx2100` 0x32733), `mtx3144`'s 9-dword fence block. **Every header-ful / opcode packet is written into a pushbuffer object (write ptr `+0x60`, limit `+0x70`) and reached by a secondary call**: bind + preamble + block loads (`mtx693`), Setup3D (`mtx398`, `mtx2393`), the submit flush packet (`mtx558` 0x45062), the copied client fence (`mtx2196` 0x4b0e6 copies into the pushbuffer, not the primary) | [BIN] |
| Our card | The primary ring **also** executes a correctly header-framed opcode stream directly (verified byte-for-byte against the captured vendor region-5 content) | [HW] |
| Reconciliation | The vendor's primary/secondary split is therefore a software convention, not a parser requirement; how the engine distinguishes a header-less tag from a header at a packet boundary is not established (see D.2) | [INF] |

### A.4 Two-level submission (the vendor's model) versus our result

```
mtx1937(pushbuf, start, end, startFlags, endFlags)      .text+0x32360
    if start >= end: return                               (nothing queued)
    if endFlags == 0: spin on 0x318/0x31c (mask 0x57 == 0x50, or 1 == 1)
    primary = pushbuf[0x80]; reserve 0x14 bytes (SwitchToNewBuffer on wrap)
    write [0x20292820][0][start|startFlags][end|endFlags][0]
         = tag {0x20 pad, 0x28 SECADDRESS, 0x29 SECEND, 0x20 pad}
    if primary[0x98]: mtx3144(primary)  (fence path)  else  0x408 = primary[0x9c] + writePtr
```

| statement | tag |
|---|---|
| Callers of `mtx1937`: `mtx693` (after bind+block loads), `mtx2196` (client chunk ×2, kernel pushbuffer), `mtx2393`, `mtx2147`, `mtx398`, `mtx558` prologue/epilogue, `mtx1391` (drain before microcode), 20 sites in the ioctl/blit layer | [BIN] |
| Region 5 (the ICD's 672 KB "command ring") is a **secondary** buffer: `mtx2196` calls it with `start = slot`, `end = (recEnd & ~1) − 0x10` (hybrid) or `recEnd` (plain), both `|2` | [BIN] |
| Hybrid mode: DRAW record `end` bit 0 set → jump covers only the 3 draw dwords; the 16-byte fence packet `[2][0x21212425][v][a]` is `LocMemCpy`'d into the **kernel pushbuffer** (0x4b0ca–0x4b0f4) which is then secondary-called with `0x88 = 2` | [BIN] |
| Our secondary call `[0x20292820][0][bus|tag][bus|tag][0]` with tags 0/2/3 stops the engine on any content; MMIO-programmed `0x10a0`/`0x10a4` stop the primary too | [HW] |
| The single structural difference between our packet and the vendor's is the **address value**: the vendor passes region-relative offsets for the client chunk and `buf[0x58] + offset` for kernel buffers; we pass bus addresses. Nothing in `mtx1937` adds a base | [BIN]; consequence [INF] — D.1 |

### A.5 The byte-index register file — 3D registers and their vendor values

`reg = 0x1000 + idx*4`, indices 0x00–0xff. Registers marked *ring-only* are
never written by MMIO anywhere in the kernel object (constant-upload.md).

| reg (idx) | vendor value / role | source | tag |
|---|---|---|---|
| `0x1000` (00) | **vertex-stream base with space tag**: kernel index-mode draw writes `mtx1132(vertexRegion, offset)` here (`mtx2147` 0x4b5fe–0x4b622, tag `0x21212c00`); address-mode draw writes only `buf[0x34]` space flags (`mtx2393` 0x3c6db); `mtx693`/`mtx676` preambles write 0 | `mtx2147`, `mtx2393`, `mtx693` | [BIN] |
| `0x1040` (10) | `4` on every vendor 3D path | `mtx693` 0x4391f, `mtx2393` 0x3c6c9 | [BIN]; relation to stride [INF] |
| `0x10b0` (2c) | primitive word: ICD `0x00040000 | (topology<<12) | entry` (`0x0004305c` triangles); kernel HAL `mtx3319` builds `(v & ~0x7f) | A[p]`, `(v & ~0x7000) | (B[p]<<12)`, sets bit 17, sets bit 18 only when `[rdi+0x430] == 1` (`0x0002305c` triangles, `0x00020020` points); also `0x10` at bus-master arm and `0x40` at X-driver init | `mtx3319` 0xb610; ICD tables; mmiotrace | [BIN] + [HW] |
| `0x10b4`/`0x10b8` (2d/2e) | preamble pair: `0`, `0x83c00000` (`mtx693` 0x43941) or `0`, `0xa0000000` (`mtx2393` 0x3c44d, `mtx398`); constant-upload.md reads them as block-file write pointer / data port (`mtx3355`, `mtx676`) | `mtx693`, `mtx2393` | [BIN]; port reading [BIN, doc] |
| `0x10f0` (3c) | MODE, written twice back-to-back before every entry write; `0x20000028` on the bind path (`mtx693` 0x436ec–0x43707), `0x20000000` on Setup3D; a third write after the entry with bit 31 set only when `engine[0xa0628]` (software-WARP cache invalidate) is set (`mtx693` 0x4376e–0x437ac, `mtx2393` 0x3c3a8/0x3c854) | `mtx693`, `mtx2393` | [BIN] |
| `0x1100` (40) | program entry (`4 × instruction index`, `mtx1391` return); latched state, written by `mtx693`/`mtx1814`/`mtx3400`/`mtx2393` | `mtx693` 0x43768 | [BIN] |
| `0x1104` (41) | companion entry (`mtx676`, `mtx2393` 0x3c3f7) / ICD per-draw parity `(flag^1)<<2` on line strips | `mtx2393`, ICD | [BIN] |
| `0x1110` (44) | `3` on the Setup3D/address-mode path (`mtx2393` 0x3c3f0); `S[0x61c] | S[0x608]` from `mtx3400` — vertex-attribute format group with `0x1114`/`0x1118` | `mtx2393`, `mtx3400` | [BIN] |
| `0x1120`–`0x112c` (48–4b) | default attribute vector `0,0,0,1.0f` (`mtx3400` 0x43c06) | doc | [BIN, doc] |
| `0x1130` (4c) | pipeline control: `0x2000` on Setup3D (`mtx2393` 0x3c381 sets byte 1 bit 5); `S[0x5f0]` on the per-draw path (`mtx3400` 0x43a77 with `r12d = 0`); **bit 9 is not a WARP disable** | `mtx2393`, `mtx3400` | [BIN] + [HW] |
| `0x1200`–`0x120c` (80–83) | texture pipes; Setup3D `0x1000f, 0, 0, 0`; `0x121c = 0` | `mtx2393` 0x3c4fa–0x3c516, `mtx398` | [BIN] |
| `0x1220`/`0x1224`, `0x1228`/`0x122c`, `0x1230`/`0x1234` | surface slots 0/1/2 base+cfg, emitted by the kernel from bound handles (`mtx2954`) | doc | [BIN, doc] |
| `0x1260`/`0x1264`/`0x1268`/`0x126c` | combiner alpha/RGB instruction pairs; Setup3D pass-through: 5 slots `0x200,0x202,0x204,0x208,0x20a` each `{0x1264=slot, 0x126c=0x400400, 0x1260=slot, 0x1268=0x400400}` (`mtx2393` 0x3c56b–0x3c5cf, tag `0x9a989b99`) | `mtx2393` | [BIN] |
| `0x12b0` (ac) | `0x3fff8000` (Windows literal 0x2c53a); `mtx398` writes a computed value at 0x32d90 (`0x212121ac`). **`0x3ff0f800` appears in no binary** | `MTXPARD`, `mtx398` | [BIN] + [HW] |
| `0x12b8` (ae) | `0x00070601` ("EOBTag" patch), MMIO at arm and in Setup3D | mmiotrace, `mtx398` | [HW] + [BIN] |
| `0x12c0`/`0x12c4` | `0xffffffff`, `0x7fffffff` clip limits; `0x12c8..0x12d4 = 0,0,0xffff,0x1ff`; `0x12e4/0x12e8 = 0`; `0x12fc = 0`; `0x1314/0x1318 = 0`; `0x1324 = 0xffffffff`; `0x11e8 = 0x100000` (`mtx398` 0x32a91–0x32c62, tags `0xbab1b0b9`, `0xb4b3b2b5`, `0xbfc5c6c9`, `0xc2c1cbca`) | `mtx398` | [BIN] |
| `0x12f4` (bd) | guard band `0x41f80000` (31.0) paired with DWGCTL `0x002010cc`; `0x41700000` above two texture stages | `mtx2393` 0x3c55c, `mtx398` | [BIN] + [HW] |
| `0x1300` (c0) DWGCTL | 3D primitive: `0x002010cc` with `0x12f4 = 31.0` when caps `0x2440 & 0x7fc00`, else `0x000010cc` (`mtx2393` 0x3c531–0x3c55c, 0x3c8d1); the depth/clear rectangle path uses `0x008002cd` with `0x1328 = 0x0fff0fff`, `0x132c = 0` (`mtx2393` 0x3c644–0x3c675). Low nibble 6/7 (`0x307`, `0x707`, `0xc07`, `mode|7`) are one-shot **flush** opcodes, excluded from the shadow (`mtx2434` rule) | `mtx2393`, `mtx398`, `mtx558` | [BIN] + [HW] (`0xc07` draws nothing, `0x11` locks the engine) |
| `0x1328`/`0x132c` | scissor min/max, DWGCTL bit 23 enables; `0x1280`/`0x1284` are a packed 10-bit colour, not a scissor | `mtx2393`, `MTXPARD` 0x42c86 | [BIN] |
| `0x1374` (dd) | strobe: named in the tag, **data dword deliberately never written** (`mtx693` leaves `-0x10(%rdx)` untouched; `mtx2393` leaves `-0x28`) | `mtx693` 0x4374f–0x4376b | [BIN] |
| `0x1090`/`0x1094` (24/25) | completion write-back address/value. Vendor: in the client fence packet and the kernel's own 3-dword fence. **On our card the in-ring form never fires; MMIO before the doorbell with `addr|2` fires** | ICD capture, `mtx558`; [HW] | [BIN] + [HW] |
| `0x10a0`/`0x10a4` (28/29) | SECADDRESS/SECEND, written only inside the 5-dword call packet | `mtx1937` | [BIN] |
| `0x12bc` (af) | DWGSYNC tag; bit 0 = block until complete. Vendor prologue writes `fence|1` then `fence` (`mtx558` 0x45069–0x4507a). **On our card bit 0 set never retires; bit 0 clear retires and `0x41c` returns the tag** | `mtx558`; [HW] | [BIN] + [HW] |

### A.6 The block file and opcode 0x41

| statement | tag |
|---|---|
| 384 × vec4 image at `constobj+0x230`, dirty bitmap at `+0x04`, six 64-byte redirect records at `+0xb0` | [BIN] (`mtx1342` 0x9004e–0x9005c, 0x902da–0x902f0) |
| Emitter `mtx1342` has exactly one caller, `mtx693` 0x43999, reached only when `bank && *bank != 0` **after** the bind packet and before `mtx1937`. Chain: `mtx2196 → mtx606 → mtx1331 → mtx1651 → mtx3400 → mtx693 → mtx1342` — i.e. per-draw, driven by the client's DRAW record | [BIN] (single-caller relocations verified) |
| The block file has no MMIO address; it is reached only by opcode 0x41 (and possibly the `0x10b4`/`0x10b8` port) | [BIN] |
| Framing on hardware: fetch pointer stops exactly at `end`; a count-1 packet executes; multi-vec4 packets **stall unless preceded by the `mtx693` preamble** (`[2][0x21211000][0][4]`, `[3][0x212c1000][0][4][0x2305c]`, `[4][0x403c3cdd][x][mode][mode][entry]`); with the preamble a DWGSYNC behind them retires (constant-upload.md, later than session-summary.md) | [HW] |
| Bank A/B are host-side shadows; the packet carries no bank selector | [BIN, doc] |
| Which constants matter for a triangle (26 entries per block-state-map §1 vs 10 per draw-sequence Phase 3) is unresolved and has produced no pixel either way | [HW] |
| The readback port `0x910`/`0x938`/`0x914` (kind 3, 4 banks × 384) has never reflected any provably executed operation | [HW] |

### A.7 WARP program load and bind

| statement | tag |
|---|---|
| Loader `mtx1391` (two callers: `mtx676`, `mtx2316`): `mtx1937` drain, `WaitBusMastering`, `0x908` == `0x63a78001` masked, `0x924 & 0xfff == 0x100`, `0x8f0 & 0x03000000 == 0`, then `0x918 = byteCursor/12`, then 3 dwords per instruction to `0x91c`; store = 0x1800 bytes = 512 instructions; returns `4 × (instrIndex + entryOffset[idx])` | [BIN, doc] + [HW] (gates pass, dwords accepted) |
| `0x920` carries a 0/1 WARP-enable byte only; 0 and 1 make no measurable difference on our card | [BIN, doc] + [HW] |
| Bind packet: `[4][0x403c3cdd][x][0x20000028][0x20000028][entry]` (`mtx693` 0x43748–0x4376b) | [BIN] |
| `mtx3400` per-flush order: kinds 3, 4[n], 0, 1 through `mtx1814`/`mtx693` (each bind + constant block), vertex format `0x1120–0x112c`, `0x1110–0x1118`, `0x1130`, canned re-upload (kind 6 with reset), shader-object entries `0x1100`/`0x10f8`, stage entries `0x10c0`–`0x10d8` | [BIN, doc] |
| **New:** the kernel's own address-mode draw `mtx2393` binds **kind 16** (`mtx2475(engine, 0x10)` = `mtx223`, 10 instructions) with `mtx2316(engine, prog, bReset=1, entryIdx=8)` and writes the returned entry to both `0x1100` and `0x1104` (0x3c329–0x3c3f7). warp-binding §3 lists kind 16 as "not referenced by any binder" — wrong | [BIN] |
| There is no go register; the draw packet is the trigger; a 3D draw is one `0x408` write | [HW] + [BIN] |
| WARP outputs: setup reads position from port-B register 0 (`B0`), `B1`/`B2` colours, `B3+` texcoords; opcode 0x60 writes port A (the temp file), never an output. The `a259…a262` position reading in warp-fragments/warp-codegen is superseded | [HW] (established 2026-09-05) |

### A.8 The record stream and who translates it

| statement | tag |
|---|---|
| Doorbell `ioctl(fd, 0x4621)`; control page `+0x00` = 1 (RENDER), `+0x08` target, `+0x10` start offset, `+0x230` byte length, `+0x234` status | [BIN, doc] + [HW] |
| Region 2 = `{u32 key; u32 count; payload}` records, itemsize per key, 82-entry table at `.rodata+0x1b08`, unknown key aborts; consumer `mtx558` @0x44ef0 | [BIN] + [HW] (exact consumption on 6 streams) |
| Key `0x0c` ids are **GL state slots** (`mtx2617`, 172 arms) into software shadows; **no** `reg = 0x1000 + id*4` mapping exists. Refuted registers produced by that rule: `0x12a0`, `0x1058`, `0x1020`, `0x103c`, `0x101c`, `0x105c`, `0x1230`, `0x1224`, `0x1024`, `0x10d8` … | [BIN] + [HW] |
| Validators (`mtx3712`: `mtx360`, `mtx2480`, `mtx2215`, `mtx1906`, `mtx3024`, `mtx297`, `mtx3654`) produce register shadows and block-file entries; `mtx606 → mtx1331 → mtx1651 → mtx3400` emit them as header-ful packets into the kernel pushbuffer | [BIN] |
| Key `0x08` binds a buffer handle → `mtx2530 → mtx1450`: slot descriptor `{1, mtx1132(obj,0), item[+0x10] (stride), obj[+0x10], 0}` at `hwstate+0x2986+slot*20`, dirty `+0x2978 |= 1<<slot`, `+0x2979 |= 4` | [BIN] |
| Key `0x1b` DRAW `{prim, flags, u64 ringObj, u64 start, u64 end|1, u64 vertexBase}`; consumer `mtx2196` per record: `mtx2455` (prim → shadows), `mtx3712`, `mtx606`, `mtx1937` (kernel pushbuffer), `mtx2372` (surfaces), `mtx1937` (client chunk), hybrid copy | [BIN] |
| Texture environment / enables / env colour have **no register path**; they are compiled into the WARP program (slot-translation mechanism C) | [BIN, doc] |
| Per RENDER the primary ring receives, in order: (secondary call for the flush packet `[4][0xc0afafc0][mode|7][fence|1][fence][saved]` built in the pushbuffer, `mtx558` 0x45062), per DRAW record the two secondary calls above, then the kernel fence `[0x21212425][seq][addr]` (0x451dd), then `0x408` | [BIN] |

### A.9 Draw packets

| packet | layout | tag |
|---|---|---|
| `0x8f` index mode | `[0x8f000000 | (2N−1)]` then N × `(first, last)` with `last` = one-past-end (ICD 1c4c95: `first = r13d`, `last = r14d`; `p_lineloop`: first=1, last=4 for 3 vertices). Indices are slot-relative to the array bound at `0x1000` (kernel) with the stride from the bind record | [BIN ICD] + [HW]; base register for the ICD path [INF] |
| `0x07` packed indices | `[0x07000000 | (nIdx−1)]` then `ceil(n/2)` dwords, element 0 in the low half | [BIN ICD] + [HW] |
| `0x17` address mode | `[0x17000001][start][end]` — kernel `mtx2393` writes **exactly** `0x17000001` with bits[22:17] = 0 and 0x20 bytes of vertex data reserved *before* the packet and excluded from the submitted range (0x3c775–0x3c7b4); Windows ORs flags into `0x17000001`. "Stride in bits[22:17]" is **not** what `mtx2393` does | [BIN] |
| Per-draw cost (ICD) | 3 dwords draw + `[2][0x21212425][seq][fenceBusAddr]` in region 5; 3 vertices × 20 bytes in region 4; three u64 offsets in the region-2 DRAW record; one doorbell | [HW] |
| Fence value | `0x1094` = submission serial; `0x1090` = bus address of the 4-byte fence page (PCI_LOCKED cookie) | [HW] |

### A.10 Fences and completion

| mechanism | vendor | our card | tag |
|---|---|---|---|
| DWGSYNC `0x12bc` / `0x41c` | tag += 8, skipping 0; bit 0 = block; emitted as `mode|7` flush + `tag|1` + `tag` + restore in every submit prologue | bit 0 set hangs; bit 0 clear works with positive and negative controls | [BIN] + [HW] |
| Memory write-back `0x1090`/`0x1094` | in-ring register packet, page = PCI_LOCKED cookie | in-ring never fires (tags 0/2/3); MMIO `0x1090 = addr|2`, `0x1094 = v` before the doorbell fires | [HW] |
| `0x450` | init snapshot equality = idle | — | [BIN, doc] |
| Engine health | — | only the 2D fill (120000 px) is a valid health check; `0x8f0` idle is not sufficient | [HW] |

### A.11 Engine init and per-draw arm

| statement | tag |
|---|---|
| Kernel `modprobe` does no MMIO; init is deferred to device open | [HW] |
| X-driver first-start (`mtx_drv.so` `mtx1373`): `0x8fc=0`?, `0x920 &= 0x7fffffff`, `0x10b0=0x40`, `0x12b8=0x70601`, `0x12b0=0`, disarm 19 descriptors `0x0010..0x0130`, `0x0000..0x000c = 0x10000000, 0, 0x10000020, 0x10001`, `0x3a8 |= 0x20`. The aperture block resets the machine under a live console | [BIN, doc] + [HW] |
| Per-draw arm from the mmiotrace: `0x10b0=0x10`, `0x12b8=0x70601`, `0x300=(v&0xffff)|0x140`, `0x384=0`, `0x308=0x83`, `0xbc0=0x4087`, `0x314=1` | [HW] |
| The kernel core itself writes by literal offset only `0x300`, `0x308`, `0x384`, `0x10b0`, `0x12b8` (`mtx3376` patches) plus the DMA/microcode registers; the descriptor bank is not written by literal offset in the core | [BIN] |
| `0x8fc` is the IRQ mask register (ISR `mtx1415` reads `0x8f4`, writes `0x8f8`; teardown writes `0x8fc = 0`; arm trace `0x8fc = 0x10000`). `pStatus[0x8fc]` in kernel-abi is a *struct field* selecting ring/emulated/direct submission — not the register | [BIN, doc] |

---

## B. Claim-by-claim audit

### B.1 parhelia-3d-driving.md (self-marked STALE)

| claim | verdict | evidence |
|---|---|---|
| `0x400` PRIMADDRESS is the trigger, `0x404` PRIMEND, `0x408` PRIMPTR | CONTRADICTED | [HW] `0x404` = start, `0x408 = end|3` triggers; [BIN] `mtx2508`/`mtx1937` |
| Ring path = append `{0x20292820,0,start,end,0}` then bump `0x408` | CONFIRMED as vendor mechanism [BIN `mtx1937`]; CONTRADICTED as a working path here [HW] | A.4 |
| Direct-MMIO `ExecuteEmulDma` lets a userspace tool skip DMA for 3D | UNVERIFIED for 3D; `mtx2434` is dead in the kernel build [BIN]; `0x1000/0x1040/0x1100/0x1104/0x1110/0x1130` are ring-only [BIN] | A.5 |
| `0x1160`/`0x1180` attribute planes are the WARP vertex path | CONTRADICTED | [HW] draws go through the ring as data; planes are the legacy DDA path [BIN Windows, doc] |
| DWGCTL low nibble 6/7 = WARP triangle, kernel uses `0xc07`; fire = `0x12bc` | CONTRADICTED | [BIN] `0xc07`/`0x707`/`0x307` are flush opcodes excluded from the shadow; 3D = `0x10cc` family; `0x12bc` = DWGSYNC. [HW] `0xc07` draws nothing |
| Minimal shading state `0x12a0 = 0xf`, `0x101c = 0`, `0x103c = 0` | CONTRADICTED | refuted slot rule; `0x12a0` is named in no vendor binary [BIN] |
| `0x1200..0x120c = 0x10000` idle, `0x1264 = 0`, `0x126c = 0x400400` | CONTRADICTED in value | [BIN] `mtx2393`/`mtx398`: `0x1200 = 0x1000f`, `0x1204..0x120c = 0`, combiner `0x1264 = slot`, `0x126c = 0x400400` ×5 |
| Solid fill by direct MMIO (`0x1228/0x122c`, `0x1300=0x5c0`, `0x1314`, `0x1304`, `0x1308`, `0x138c`) | CONFIRMED [HW] for the 2D path (`0xc0` variant in the tool, EXA) | — |
| "Submission, geometry, worked sequences are proven" | CONTRADICTED | no 3D pixel has ever been produced [HW] |

### B.2 parhelia-3d-state-complete.md

| claim | verdict | evidence |
|---|---|---|
| Arm `0x10b0=0x10, 0x12b8, 0x300=0x1f0b0140, 0x384, 0x308=0x83`; submit `0x404`, `0x408=end|3`; `0x314` | CONFIRMED [HW], except `0x300` value: 16-bit, write `(v&0xffff)|0x140` [HW] | — |
| Command buffer = `(register-index, value)` pairs, `index = (reg−0x1000)/4` | CONTRADICTED | region 2 = key/count records of GL slots [BIN `mtx558`/`mtx2617`]; region 5 = opcode-framed [HW] |
| `0x1100–0x117c = 0xffffffff` attribute init; `0x1298 = 2048.0` | CONTRADICTED | slot ids 0x40–0x5f / 0xa6, not registers [BIN]; the doc's own header |
| Scissor = `0x1280`/`0x1284` via `0x2121a1a0` | CONTRADICTED | packed 10-bit colour (`MTXPARD` 0x42c86); scissor = `0x1328`/`0x132c` [BIN `mtx2393`] |
| Triangle list is the only HW primitive; DWGCTL low byte 0x07 | CONTRADICTED | `0x10b0` topology field with 9 codes [BIN ICD + `mtx3319`]; low nibble 7 = flush |
| `0x12a0` colour mask, `0x101c`, `0x103c`/`0x1064`, `0x12ac` | CONTRADICTED | refuted slot rule |
| `0x91c` = microcode only, not per-draw geometry | CONFIRMED | [BIN] `mtx1391` |

### B.3 parhelia-gpu-access-map.md

| claim | verdict | evidence |
|---|---|---|
| No MMIO at modprobe; BAR1 8 KB; descriptor bank `0x0000–0x0130` | CONFIRMED [HW] | — |
| `0x920 = 1` arms the descriptors | UNVERIFIED | `0x920` = WARP-enable byte [BIN, doc] |
| No SUBMIT ioctl; the doorbell is the submission | CONFIRMED [BIN] `mtx903` | — |
| §4 arm + ring submit; `0x91c` init-only; `0x924` low12 → `0x100` | CONFIRMED [HW]/[BIN] | — |
| §5 "7-dword descriptor: header, vertex count, sequence, vertex-buffer bus address" | CONTRADICTED | `+0x04` first, `+0x08` last, `+0x14`/`+0x18` fence value/address [BIN ICD] + [HW] |
| §5 "`reg = 0x1000 + index*4` is the DMA-ring namespace in region 5" | CONFIRMED | [BIN] |
| §5a pair-decoded state list (`0x1034=1 … 0x1298`) | CONTRADICTED | superseded in-doc; mis-sliced records |

### B.4 parhelia-command-construction.md

| claim | verdict | evidence |
|---|---|---|
| Seven mappings, sizes, roles; slot `n*0x2000`; vertices at `+0x40` | CONFIRMED [HW] | — |
| `0x8f000001` count = payload dwords − 1; `+0x04` first; `+0x08` one-past-end | CONFIRMED | [BIN ICD 1c4c95] + [HW `p_lineloop`] |
| `[2][0x21212425][fenceVal][fenceAddr]` | CONFIRMED [HW] | — |
| "Index 0x21 terminates the packet; the next dword is a new header" | CONTRADICTED | [BIN] `mtx2393` 0x3c4de: `0x21` ends the tag, the header's count spans further tags |
| "Register-list counts are not decremented — count = number of registers" | CONTRADICTED | [BIN] count = payload dwords − 1 (tags + data); `[7]` for 2 tags + 6 data |
| Draw header `0x03000000 | ctx[0xa040] | ctx[0xa044] | count`; bit27 ranges vs packed | CONFIRMED [BIN ICD, doc] | — |
| `0x10b0 = 0x00040000 | (B<<12) | A` table | CONFIRMED [HW] (`0x0004305c`); kernel HAL variant sets bit 17 instead [BIN `mtx3319`] | — |
| Region 2 = record stream; `+0x230` = length | CONFIRMED [BIN] | — |
| Vertex-format prefix sum; `[x,y,r,g,b]` stride 20 raw GL coordinates; format dynamic | CONFIRMED [BIN ICD] + [HW] | — |
| Scissor `[2][0x2121a1a0]` → `0x1280`/`0x1284` | CONTRADICTED | see B.2 |
| §"The state stream (region 2) — (index, value) pairs, `register = 0x1000 + index*4`" (lines 284–306) | CONTRADICTED | stale section; refuted by the same file's correction and [BIN] |
| §"What this means" step 5 "publish the slot by updating head/tail dwords" | CONTRADICTED | they are DRAW-record payload [BIN `mtx2196`] |
| §"Unresolved conflicts" #3 (`0x1230`, `0x1224`, `0x1024`, `0x10d8` are not what the slot rule says) | CONFIRMED | [BIN] |
| "Where the vertex-ring base reaches the hardware — open" | RESOLVED for the kernel path: key 0x08 → `mtx1450` → `mtx1132`; the kernel's own index-mode draw writes it to **`0x1000`** (`mtx2147`) [BIN]. For the ICD path the register is [INF] (`0x1000`, or the `0x1110` group) | A.5 |
| Replay: cookie publication at control `+0x28`/`+0x48`; in-band buffer creation | CONFIRMED [HW] | — |

### B.5 parhelia-minimal-triangle.md

| claim | verdict | evidence |
|---|---|---|
| 28000 px positive control; 19 doorbells; command table | CONFIRMED [HW] | — |
| Draw packet: `+0x08 = 3 vertices` (count) | CONFIRMED in value, but the field is `last` (one-past-end); equal when `first = 0` [BIN ICD] | — |
| `+0x18` = bus address of the `control+0x48` fence page | CONFIRMED [HW] | — |
| "A draw is the primitive plus its fence, nothing else" | CONFIRMED for region 5 [HW]; state travels in region 2 and the kernel pushbuffer [BIN] | — |
| Record 5 `(0x13,1)(0x14,2)` = WARP bind | CONFIRMED as record content [HW]; hardware effect [INF] | — |
| Command 0x05 carries a pointer table; byte-exact replay impossible | CONTRADICTED | control-commands.md retraction: inline allocation request [BIN `mtx3112`/`mtx938`] |
| No separate 3D launch register; every submission is `0x408 = end|3`; `0x0030..0x003c` programmed per transfer | CONFIRMED [HW]; the ring content is a secondary call [BIN] | — |
| DWGSYNC quad `[4][0xc0afafc0][0xc07][tag|1][tag][restore]` retires (tags 8/16/64) | CONTRADICTED (retracted in-doc) | [HW] bit 0 set never retires from a clean boot |
| "The engine has never been initialised" | UNVERIFIED | `engine_init` ran; `0x8fc = 0` eliminated [HW] |
| Write-back pair appended to the 2D fill changes nothing | CONFIRMED [HW]; solved by MMIO + tag 2 | — |

### B.6 parhelia-engine-init.md

| claim | verdict | evidence |
|---|---|---|
| `mtx1373` sequence (`0x8fc`, `0x920`, `0x10b0=0x40`, `0x12b8`, `0x12b0=0`, descriptors, `0x0000..0x000c`, `0x3a8`) | [BIN, doc]; `0x0000` reads `0x10000000` after X [HW]; aperture block resets the box under a live console [HW] | — |
| "`0x08fc` is the one to look at first; `mtx2434` keys on it; wrong value = no bus-master DMA" | CONTRADICTED | `0x8fc` is the IRQ mask [BIN, doc]; `pStatus[0x8fc]` is a struct field; `mtx2434` has no caller in the kernel object [BIN]; `0x8fc = 0` eliminated [HW] |
| `0x10b0 = 0x40` at init and a per-draw primitive word later; both right | CONFIRMED [BIN `mtx3319`] + [HW] | — |
| `0x12b0 = 0` at init | UNVERIFIED (`mtx_drv`); per-draw setup uses `0x3fff8000` [BIN `MTXPARD`] + [HW] | — |
| Only step 19 of the 22-step start table touches registers | [BIN, doc], UNVERIFIED here | — |
| `mtx2069`/`mtx1875` write `0x300 = 3`, `0x308 = 3` | UNVERIFIED; `0x308 = 3` is the reset path's value [BIN, doc]; arm uses `0x83` [HW] | — |

### B.7 parhelia-control-commands.md, parhelia-buffer-binding.md

| claim | verdict | evidence |
|---|---|---|
| Dispatcher `mtx3683`, 29-entry table; command 0x05 = inline allocation request; `control+0x10` size, `+0x18` type | CONFIRMED [BIN, doc] | — |
| Key 0x08 = 32-byte bind record → `mtx2530` → handle lookup / direct pointer → `mtx1450` | CONFIRMED [BIN] (`mtx1450` stores the 20-byte slot descriptor and dirty bits) | A.8 |
| `mtx1132`: `addr = offset + obj[0x18]`, type 1 → `|2`, type 2 → `|3` | CONFIRMED [BIN] | — |
| Surface packets `[4][0x8d8c8988]…` (`0x1220/0x1224/0x1230/0x1234`), `[2][0x21218b8a]` (`0x1228/0x122c`); texture `[0x72717000|…]` | [BIN, doc] | — |
| "`0x408 |3` because 3 is the ring's space tag" | [INF]; consistent with `mtx1937` (`0x9c` bias carries the bits) | — |
| `+0x10` latched into `hwstate[0x118]` | UNVERIFIED (not seen in `mtx1450`; it lands in the slot descriptor) | — |

### B.8 parhelia-block-state-map.md

| claim | verdict | evidence |
|---|---|---|
| Block-load packet §5.2 (`hdr`, `count`, `start`, `1`, `0`, payload; ≤ 63) | CONFIRMED [BIN `mtx1342` 0x901d6–0x9023c] + [HW] framing | — |
| Absolute MMIO address unknown; the DMA engine resolves the index | CONFIRMED [BIN] (no MMIO path in the object) | — |
| 1.a "viewport pair is the one you cannot omit" | UNVERIFIED [HW: no effect observed] | — |
| Minimum set = 26 entries (§1) | UNVERIFIED; draw-sequence Phase 3 says 10 in bank A and `0x179–0x17c` bank-B only [BIN, doc]; neither has produced a pixel [HW] | — |
| Bank A/B host-side; keep one shadow | CONFIRMED [BIN, doc] / [INF] | — |
| `mtx1342` "called only from `mtx693`" (constant-upload) | CONFIRMED [BIN] | — |
| §5.3 client payload redirect via `+0xb0` records; `mtx4027`/`mtx558` write the file at a runtime index | [BIN, doc]; redirect path read here (0x902da–0x902f0) CONFIRMED | — |

### B.9 parhelia-warp-binding.md

| claim | verdict | evidence |
|---|---|---|
| Entry goes to `0x1100` via `[4][0x403c3cdd][x][MODE][MODE][entry]`, `MODE = 0x20000028`, `0xdd` data not written | CONFIRMED [BIN `mtx693` 0x43748–0x4376b, 0x436ec–0x43707] | — |
| `mtx676` variant `[6][0x41403c3c][MODE][MODE][entry][entry][0x2121213c][MODE]` | [BIN, doc] | — |
| `0x920` carries only the enable bit; `mtx3806` returns nothing useful | CONFIRMED [BIN, doc]; `0x920` 0/1 no difference [HW] | — |
| Register table §1: `0x1000 = 0` "pipe preamble" | CONTRADICTED in role | [BIN] `0x1000` = vertex-stream base with space tag (`mtx2147`), space flags on the address-mode path (`mtx2393`); it is 0 in `mtx693` because the base is bound elsewhere on that path |
| `0x1040 = 4`, `0x10b0 = 0x2305c`, `0x10b4 = 0`, `0x10b8 = 0x83c00000` preamble | CONFIRMED [BIN `mtx693` 0x43907–0x4394f] (note `0x10b8 = 0xa0000000` on `mtx2393`/`mtx398`) | — |
| Stage entries `0x10c0`–`0x10d8`, `0x10f8`; "disabled = 2"; six always written | [BIN, doc], UNVERIFIED here | — |
| Entry = `4 × (0x918 index + entryOffset)`; built-in blobs have zero offset tables | [BIN, doc] | — |
| No go register; vertices/draw packet trigger | CONFIRMED [HW] | — |
| §3 "kinds 2, 15, 16 have no binder" | CONTRADICTED | [BIN] `mtx2393` binds kind 16 (`mtx223`) with entry idx 8 on the kernel's address-mode draw |
| §5 step 6 "submit `[0x8f000001][first][last]` plus the vertex data" | CONTRADICTED as complete | index mode needs the array bound at `0x1000` (kernel) — never done in the tool [BIN] |
| `0x10c0–0x10d8` are WARP stage entries, not stencil ops | CONFIRMED [BIN] (draw-recipe's register column is void) | — |
| Attempt 1: reboot caused by an all-zero constant store | CONTRADICTED | draw-sequence: the vendor binds with `0x000..0x14c` zero [BIN, doc]; cause UNVERIFIED |
| Attempt 5: "engine accepted the work, fence moved" | CONTRADICTED as evidence | X owned the engine; fence reads were stale [HW] |
| `a259…a262` = transformed position output | CONTRADICTED | position is read from port-B 0; op 0x60 writes port A (temp file), never an output [HW] |
| Reset between attempts requires a reboot | CONFIRMED [HW] | — |

### B.10 parhelia-vertex-isa.md

| claim | verdict | evidence |
|---|---|---|
| PART 1: programs uploaded via key `0x45`/`0x46` "DMA packets" | CONTRADICTED in wording | they are record-stream keys consumed by the kernel [BIN `mtx558`]; hardware delivery is `0x918`/`0x91c` [BIN `mtx1391`] |
| "Delivery is a DMA descriptor / dedicated ring-packet type" | CONTRADICTED | [BIN] MMIO stream via `mtx1391` |
| Vertex layout: viewport transform CPU-applied; z/w never streamed | CONTRADICTED | [HW] raw object-space coordinates; stride grows with z/alpha/texcoord |
| Command buffer = `(register-index, value)` pairs; `--warpring` with captured state is the right shape | CONTRADICTED | [BIN]/[HW] |
| PART 2 §1 loader, 12-byte instruction, gates | CONFIRMED [BIN, doc] + [HW gates] | — |
| §11 readback port `0x910/0x938/0x914`, kind 3, 4 banks | CONFIRMED mechanism [BIN, doc]; UNUSABLE as an execution detector [HW] | — |
| §12 slot table (`+0x40 = mtx1824` …) | CONTRADICTED | vertex-pipeline §1.3: shifted by one [BIN, doc] |
| "`mtx3806` returns the entry-point value written to `0x920`" | CONTRADICTED [BIN, doc] | — |
| `mtx676` = self-test that runs at init; `mtx2785` is a RAM sweep | CONFIRMED [BIN, doc] | — |

### B.11 parhelia-session-2026-09-05.md

| claim | verdict | evidence |
|---|---|---|
| "The kernel never emits a block-load packet; `0x4106000f` is the only 0x41 tag" | CONTRADICTED | [BIN] `mtx1342` builds the header at 0x9022c (`or $0x41000000`); retracted by constant-upload.md |
| Constants DMA'd from `constobj+0x230` (hypothesis) | CONTRADICTED | [BIN, doc] no `MtxRegion_GetPhys` on that path; `mtx1342` copies into the ring |
| `0x300` is 16-bit | CONFIRMED [HW] | — |
| Write-back via MMIO `addr|2` | CONFIRMED [HW] | — |
| Header-less `[hdr][4 data]` register write, `0x17000001` draw, non-blocking DWGSYNC, count-1 header-less 0x41 execute | CONFIRMED [HW] | — |
| "Multi-vec4 0x41 in any framing stops the engine" | CONTRADICTED by the later measurement | constant-upload.md: executes with the `mtx693` preamble [HW] |
| Secondary call stops the engine, any tag | CONFIRMED [HW]; vendor uses it [BIN] — D.1 | — |
| Eliminated list (`0x8fc=0`, bind precondition, `0x1000`/`0x10b0` readings, secondary draw form, identity at `0xE0`, X as initialiser) | [HW] | — |
| "No host-side code pushes the block file" | CONTRADICTED | [BIN] `mtx693 → mtx1342` on the per-draw path |

### B.12 parhelia-record-stream.md, parhelia-slot-translation.md, parhelia-opcode-41.md, parhelia-constant-upload.md

| claim | verdict | evidence |
|---|---|---|
| `{key, count}` framing, itemsize table, 82-entry table, abort on unknown key; DRAW record layout | CONFIRMED [BIN] (`mtx2196` reads `+0x8`, `+0x10`, `+0x18` bit 0, `+0x20`) + [HW] | — |
| Three delivery mechanisms; texture env compiled into the program; register maps `0x12c8/0x12cc/0x12d0`, `0x12e4/0x12e8/0x12ec/0x12f0/0x1324/0x1328/0x132c/0x12fc` | CONFIRMED [BIN] (`mtx398` writes exactly that register set with defaults) | A.5 |
| Header rule `[31:24] opcode / [22:17] groups−1 / [15:0] dwords−1`; `0x41` = destroy verb in the token table; `0x41` = index `0x1104` | CONFIRMED [BIN] | — |
| `mtx3400` emits four 0x41 uploads per flush before the vertex-format writes | [BIN, doc] (three `mtx1814` sites 0x44218/0x44255/0x4429b + `mtx693`) CONFIRMED | — |
| Destination-select preamble is required; `0x1000/0x1040/0x1100/0x1104/0x1110/0x1130` are ring-only | CONFIRMED [BIN] + [HW] | — |
| `0x10b4`/`0x10b8` = block-file write pointer / data port (`mtx3355`, `mtx676`) | UNVERIFIED; `mtx693`/`mtx2393` write the pair as a mode word (`0`, `0x83c00000` / `0xa0000000`) [BIN] | — |

### B.13 parhelia-draw-recipe.md

| claim | verdict | evidence |
|---|---|---|
| §0 record grammar; `0x0c` pairs are state ids; `(0x0c,N)`/`(0x05,N)` are headers | CONFIRMED [BIN] | — |
| §1.1 arm step 4 `0x300 = (v & 0xe0e0ff3f) | agpbits | 0x100` | CONTRADICTED | [HW] 16-bit register, vendor holds `0x140` |
| §1.1 `0x0000..0x0130` = 20 descriptors `{base, size, 1, 0x10001}`; `0x408 = ringBase|0x23` first; `0x8fc = 0x10000` | CONFIRMED [HW] | — |
| GL client never maps BAR1 | CONFIRMED [HW] | — |
| §1.2 seventeen pre-draw submissions, control-op vocabulary | [BIN ICD + HW, doc] | — |
| §1.3 "published reg" column | CONTRADICTED (void) — stated in-doc | — |
| §2.2 DRAW record; kernel translates the primitive enum through shadows (`mtx2455`) | CONFIRMED [BIN `mtx2196` 0x4ae07]; the ICD also emits `0x10b0` directly [BIN ICD] — both exist | — |
| §2.4 `mtx2434` decode; `0x20` pad / `0x21` end; "the next dword is a new header" | CONFIRMED for header-less streams; CONTRADICTED inside header-ful packets [BIN `mtx2393`] | — |
| `mtx2434` is dead code in the kernel build | CONFIRMED [BIN] (zero callers) | — |
| §2.4a hybrid mode: jump + inline copy of the last 16 bytes "straight into the primary ring" | CONFIRMED mechanism; CONTRADICTED destination — the copy goes into the kernel pushbuffer and is secondary-called (`mtx2196` 0x4b0ca–0x4b0f4, `0x88 = 2`) [BIN] | — |
| Ring sequence (a) flush 6 dwords, (b) calls, (c) 3-dword fence, (d) `0x408` | CONFIRMED [BIN] with the correction that (a) lives in the pushbuffer (`mtx558` 0x45040–0x450cc) | — |
| "Count prefix on some elements, not others; `mtx2434` reads no count" | CONFIRMED observation; resolution: header-less only in the primary, header-ful only in secondaries [BIN]; the engine accepts header-ful in the primary [HW] | A.3 |
| §4 wait primitives, no timeouts, `0x450` snapshot, FIFO bit 22, tag rule | [BIN, doc]; `mtx1937` gate read here CONFIRMED | — |
| §4.4 serial write-back `0x1090`/`0x1094` | CONFIRMED [HW]; in-ring form never fires on our card [HW] | — |
| §5.8 reset = PCI config `0xE0` strobe, no caller | [BIN, doc] | — |
| HW attempt: seven real programs stream; draw rasterises nothing; binding + block path missing | CONFIRMED [HW] then; still no pixel | — |

### B.14 parhelia-draw-sequence.md ("Follow this file")

| claim | verdict | evidence |
|---|---|---|
| Tag-byte rule valid, slot-id rule refuted | CONFIRMED | — |
| Phase 0: sample `0x8f0` more than once; GFP_DMA32 ring; never the FB BAR | CONFIRMED [HW] | — |
| Phase 1: `0x300 = (read & 0xe0e0ff3f) | agpbits | 0x100` | CONTRADICTED [HW] | — |
| Phase 1: `0x920 = 1` "WARP enable — this bit and nothing else" | CONFIRMED [BIN, doc]; no effect either way [HW] | — |
| Phase 2 gates; `0x918 = cursor/12`; do not stream `warp_ucode.h` | CONFIRMED [BIN] + [HW] | — |
| Phase 3: bind → block loads → submit; `mtx1342` single caller; `program == NULL` emits nothing | CONFIRMED [BIN `mtx693` 0x43748 → 0x43999 → 0x43829] | — |
| Phase 3: bank A/B, init set 10 indices, `0x179–0x17c` bank-B only | [BIN, doc]; conflicts with block-state-map §1.c; UNVERIFIED which matters [HW] | — |
| Phase 4: `0x1328`/`0x132c`, DWGCTL bit 23, `0x12f4`; `0x1280/0x1284` not scissor | CONFIRMED [BIN `mtx2393`] | — |
| Phase 5 preamble `0x1000 = 0` | CONTRADICTED in role (vertex base) [BIN `mtx2147`] | A.5 |
| Phase 6 short bind form; `0x10f0` double write | CONFIRMED [BIN]; latch semantics UNVERIFIED | — |
| Phase 7 `DWGCTL = (dst<<10) | (src<<8) | 0x07` | CONTRADICTED | [BIN] 3D = `0x10cc` family; nibble 7 = flush; the file contradicts itself later |
| Phase 7 draw `[0x8f000001] first count` | CONTRADICTED in field name (`last`) [BIN ICD] | — |
| "Three packet count rules" | CONTRADICTED | one rule (payload dwords − 1) plus the `0x07` index exception [BIN] |
| Readback four banks | mechanism [BIN]; port UNUSABLE [HW] | — |
| P690 `mtx676` replication "0 registers changed" as isolation | CONTRADICTED as evidence | readback port never observes anything [HW] |
| DWGCTL `0x11` locks; `0xc0` draws; `0x07/0xc07/0x30200c` nothing; X owns the engine | CONFIRMED [HW] | — |
| Two-source: DWGCTL `0x10cc` family; `0xc07/0x307/0x707` flush | CONFIRMED [BIN `mtx2393`, `mtx398`, `mtx2434`] | — |
| "The `0x8f` packet belongs in a SEPARATE buffer entered via `0x10a0 = start|2`, `0x10a4 = end|2`" | CONFIRMED as the vendor model [BIN `mtx2196`/`mtx1937`]; on our card the primary accepts it inline [HW] | A.4 |
| Later "RETRACTION: it is inline in the level-1 ring" | CONTRADICTED | region 5 is the level-2 buffer; the level was misidentified [BIN] |
| Later "payload is first, count, not first, last" | CONTRADICTED | `last` [BIN ICD]; indistinguishable with `first = 0` |
| "`0x109c = start` alternative reading" | UNVERIFIED | — |
| "WARP launch is the `0x1100` write" | UNVERIFIED wording; `0x1100` is latched state [BIN, doc] | — |
| "Bulk inline-data word `((n/4−1)&0x3f | 0x2080)<<17 | (n−1)`" | CONFIRMED — it is the ordinary 0x41 header (`0x2080<<17 = 0x41000000`) [BIN] | — |
| `0x1104` = page toggle; `0x1374` data-less strobe | CONFIRMED [BIN] | — |
| Merged Setup3D list | CONFIRMED [BIN `mtx2393`/`mtx398`] except `0x12b0 = 0x3ff0f800` → CONTRADICTED (`0x3fff8000`, `MTXPARD` 0x2c53a; `mtx398` computes it) [BIN] + [HW]; third `0x10f0` write is `0` unless the cache-invalidate flag is set | A.5 |
| `0x1088/0x108c` tag-repeat pair vs kernel-abi `0x10a8/0x10ac` | UNRESOLVED (two [BIN] readings) | — |
| `0x0940` bit 16, `0x1148` | UNVERIFIED [BIN Windows] | — |
| "3D state does not go through the ring" | CONTRADICTED in wording | the translated state does go through the ring, as header-ful packets in the kernel pushbuffer reached by a secondary call [BIN] |
| `[count][a][key][0]` grammar | CONTRADICTED (retracted in-doc) [BIN] | — |
| Address-mode draw `[0x17000001 | flags | ((vertexDwords−1)<<17)][start][end]` | CONFIRMED opcode/shape [BIN]; "stride in [22:17]" CONTRADICTED by `mtx2393` (writes exactly `0x17000001`) | A.9 |
| CPU physical address took the machine down; VRAM offset + tag 0 safe | CONFIRMED [HW] | — |
| "The failure is inside the 3D pipeline; everything around is proven" | CONTRADICTED by the same day's later finding (packets were fetched, not executed; instruments broken) [HW] | — |
| `mtx2434` lists `0x24/0x25/0x28/0x29` as consumed-not-written | CONFIRMED [BIN, doc]; it is an emulator's special-casing, not hardware semantics [INF] | — |
| SECADDRESS/SECEND by MMIO stop the primary; in-ring `0x1090` never fires; MMIO `|2` fires | CONFIRMED [HW] | — |

### B.15 Other files touching submission

| file | claim | verdict |
|---|---|---|
| parhelia-kernel-abi.md §6.1–6.4, §7 | `mtx1937` packet and `0x408` write; `mtx2434` decode; `0x12bc`/`0x41c`; `mtx3144` 9-dword block; `0x404 = addr & ~3` | CONFIRMED [BIN] (`mtx1937` read here; rest [BIN, doc]) |
| parhelia-kernel-abi.md §5.5 | "region 2 offsets `0x008–0x028` = a 0x28-byte submit record `{prim, handle, START, END|1, vtx}`" | CONTRADICTED — it is the DRAW record payload inside a record stream [BIN]; the file's own "superseded" note |
| parhelia-drm-design.md §3.1–3.5 | uAPI = raw packet stream; kernel ring = secondary calls `{0x20292820,…}`; `0x404 = addr & ~3`; whitelist rejecting `0x24/0x25/0x28/0x29/0xaf`; per-submission sequence (a)–(d) | design [DECIDE]; the vendor facts CONFIRMED [BIN]; note (a) lives in a secondary, and the in-ring `0x1090` form does not fire on our card [HW] |
| parhelia-accel.md, parhelia-2d-accel-complete.md | vendor pushbuffer doorbell `MMIO 0x3050 = 1` | CONTRADICTED as a BAR1 register: `0x3050` is outside the 8 KB BAR1 (drm-design §10 item 5) [HW]; the 13 `[rax+0x3050] = 1` stores in `mtx_drv.so` therefore address a struct/shared-page field, not MMIO [INF]; the doorbell the kernel writes is `0x408` [BIN] |
| parhelia-accel.md, 2d-accel-complete.md, registers.md | engine register file `0x1000 + idx*4`, tag packets, direct MMIO equivalent for 2D | CONFIRMED [HW] (EXA/XAA) |
| parhelia-gl-driver.md "Command submission — DONE: direct MMIO, fire with `0x12bc`" | CONTRADICTED (see B.1) | — |
| parhelia-gl-driver.md, gl-feature-map.md, fragment-ops.md, accel.md §"Render state" | register column from the slot rule (`0x101c`, `0x1020`, `0x103c`, `0x1058`, `0x12a0`, `0x1064`…) | CONTRADICTED (each file now carries a correction banner; the tables remain) |
| parhelia-fragment-isa.md §1/§10 | combiner regs reached "as (register-index, value) dword pairs in the DMA command stream" | CONTRADICTED in wording — they are emitted by the kernel as tag packets (`mtx2393`/`mtx398`), the client emits key-0x46 records [BIN] |
| parhelia-mesa-driver.md | "Submission is pure userspace MMIO; the kernel char device does not submit work" | CONTRADICTED [BIN] (`mtx903` doorbell → `mtx558` → ring); `mtx2153` relocation claim withdrawn in-doc |
| parhelia-rendertarget.md §0.2 | kernel DMA blocks are length-prefixed `[n−1][tag][data…]`, 6 witnesses | CONFIRMED [BIN] |
| parhelia-vertex-submission.md | `0x07` packet, count rules, `0x1104` parity, batch capacity, no cross-submission vertex reuse | CONFIRMED [BIN ICD] + [HW] |
| parhelia-m9-3d-submission.md / parhelia-m9-dma.md | M9 `0x400`/`0x404` read-only; ring base is a DMA-adapter descriptor; `0x314`/`0x31c` doorbells | M9-only; out of scope for 0x0528; recorded for C.2 |
| parhelia-warp-readback.md | port kind 3 only; VRAM-ring hazard; port never observes anything | CONFIRMED [HW] |
| parhelia-3d-coverage.md | "Block-load packet framing SOLVED [HW]"; "0x8f needs a bound array, 0x17 self-contained" | CONFIRMED framing [HW]; execution needs the preamble [HW]; the `0x8f` base register = `0x1000` on the kernel path [BIN] |

---

## C. Code versus docs and binaries

### C.1 tools/parhelia-3dtri.c

Rows describe the committed HEAD version. An uncommitted edit dated 2026-09-05 23:18 (not part of this audit) rewrites `tri_secondary`'s static state toward the `mtx398`/`MTXPARD` block (texture-unit registers `0x11c0..0x11d8`, combiner slots `0x80/0x82/0x84/0x88`, `0x12e4/0x12e8/0x12d4/0x12c8` defaults) and adds `g_prog2`/`g_entry2`/`g_matbase2`; re-check those rows against it.

| function / line | what the code does | disagreement |
|---|---|---|
| `dma_build` | header-less `[tag][4 data]`, pads with idx `0x20` + data 0 | matches the vendor's *primary-ring* form [BIN]; fine for the 2D fill [HW]; must not be mixed with opcodes [HW]. Comment at 945–950 calls it "the hardware packet format reversed from `CDPSHwManager::WriteBuffer`", which is the header-less X-driver form only |
| `dma_build_hdr` | `[cnt][tag][cnt data]` per group of ≤ 4 | header = payload dwords − 1 ✓ [BIN]; one tag per header (the vendor also packs several tags under one header — equivalent) |
| `emit_reglist` | one header, many tags, `buf[0] = w − 2` | matches `mtx2393`'s multi-tag packets ✓ [BIN] + [HW] |
| `ring_submit` | `0x404 = start | g_startflags`, read-back, `0x408 = end | g_endflags`, per submission | vendor writes `0x404` only at ring restart and masks the low bits off (`& ~3`); per-submit only `0x408` [BIN, doc]. Re-pointing `0x404` every time is a deviation (harmless so far) |
| `append_dwgsync` | `[4][0xc0afafc0][0x0c07][tag(|1)][tag][restore]` | vendor prologue writes `mode|7` (`mtx558` 0x4505e: `r12d|7`), `mtx508` `0x707`, `mtx3144` `0x307,4,0x707`; the literal `0xc07` is one instance. Vendor sets bit 0 on the first tag every submit [BIN]; on our card bit 0 hangs [HW] — unresolved (D.4) |
| `append_writeback` | in-ring `[2][0x21212425][val][addr|tag]` (+ DWGSYNC) | byte-identical to the client packet [HW capture]; never fires here [HW]; MMIO path (`dma_fill_via_ring` `g_wbmmio`) is the one that works |
| `append_secondary_call` / `tri_secondary` | `[0x20292820][0][bus|tag][bus|tag][0]` padded to 8 dwords + 3-dword fence | shape identical to `mtx1937` + `mtx558` [BIN]; **address value differs**: vendor passes region offsets (client) or `buf[0x58]+offset` (kernel) [BIN]; stalls here [HW] — D.1 |
| `tri_secondary` state list | `0x1374, 0x10b4=0, 0x10b8=0xa0000000, 0x121c, 0x1200=0x1000f…, 5 combiner slots, 0x12c0/0x12c4/0x1324, 0x12b8, 0x12b0=0x3fff8000, 0x1000=0, 0x1040=4, 0x10b0=0x0004305c, surfaces, DWGCTL, 0x1328/0x132c, 0x12f4` then the `mtx2393` preamble verbatim, then `[0x17000001][vtx][vtx+60]` | matches `mtx2393`/`mtx398` [BIN] and hardware truth [HW] except: (a) `0x1100`/`0x1104` written as 0 where `mtx2393` writes the entry of **kind-16 `mtx223`** streamed at index 0 with `bReset = 1` — the tool streams no program on this path, so entry 0 points at whatever the store holds; (b) `mtx398`'s `0x11e8 = 0x100000`, `0x12c8..0x12d4`, `0x12e4/0x12e8`, `0x12fc/0x1314/0x1318`, `0x1304/0x1308`, `0x132c = 0x10001000` defaults are not emitted; (c) `0x1000 = 0` is right only because the vertices sit at a VRAM offset (tag 0) |
| `warp_draw_triangle` 2181–2254 | Setup3D + bind in one stream | `0x10f0` third write = `0x80000000` unconditionally — `mtx2393` writes 0 unless the software-WARP invalidate flag is set [BIN]; `0x1130 = 0x2200` with the comment "bit9 = WARP bypass" — CONTRADICTED [HW]; `0x10b0 = 0x0002305c` here vs `0x0004305c` in `tri_secondary` — inconsistent within the tool; `0x1000 = 0` with the comment calling it the vertex base — correct reading, but then vertices are addressed by `0x17` and `0x1000` only needs the space tag [BIN] |
| `warp_draw_triangle` 2424/2454/2476 | `0x17000001 | 0x80000000 | ((vdw−1)<<17)` | `mtx2393` writes exactly `0x17000001` [BIN]; bit 31 and the `(vdw−1)<<17` group count are unsupported by the kernel's own emitter; `tri_secondary` (1837) uses the plain form — the tool is inconsistent |
| `warp_block_preamble` | `[3][0x212c1000][0][4][0x2305c]`, `[4][0x403c3cdd][0][mode][mode][desc]` | header-ful despite the comment saying header-less; omits `mtx693`'s `[2][0x21212e2d][0][0x83c00000]`; writes `0x1000 = 0` (vertex base) [BIN] |
| `block_load` (`g_blockfmt`) | `0x41000000 | ((n−1)<<17) | (n*4−1)` + bare payload | this is `mtx676`'s **vertex/test** packet shape, not a block load (no descriptor) [BIN opcode-41]; default form matches `mtx1342` ✓ |
| `warp_init_constants` / `warp_min_constants` | 10-index bank-A set / 26-entry set | both [BIN, doc]; neither produced a pixel [HW]; the 26-set includes bank-B-only `0x179–0x17c` |
| `engine_init` | `0x8fc = 0`, `0x920 &= 0x7fffffff`, `0x10b0 = 0x40`, `0x12b8`, `0x12b0 = 0`, `0x3a8 |= 0x20`, `0x300 = 3`, `0x308 = 3` | `0x8fc` is the IRQ mask, writing 0 masks interrupts [BIN, doc]; `0x300 = 3` clobbers the `0x140` arm value [HW]; `0x308 = 3` is the reset-path value, arm uses `0x83` [HW] |
| `dma_fill_test` (VRAM pushbuffer via `0x400/0x404/0x408`, `0x384` kick) | legacy | contradicted by [HW] (VRAM ring overruns; `0x384` is DMAPAD) — dead path |
| `warp_selftest676` | replicates `mtx676` with `0x10b4 = 0` as "group 1" | `mtx676`'s first tag is `0x2121212d` → `0x10b4` ✓ [BIN, doc]; the readback verification is void [HW] |
| `warp_fifo_const` | constants via `0x10b4` pointer + `0x10b8` data | rests on the constant-upload.md port reading; `mtx693`/`mtx2393` write the pair as `{0, 0x83c00000/0xa0000000}` [BIN] — UNVERIFIED |
| Comment 1272–1286 | "everything the ICD builds is a secondary buffer; the primary carries only the call and the fence" | CONFIRMED [BIN]; but the [HW] result (primary executes opcodes; secondary stalls) is the standing contradiction |
| Comment 3088–3096 | "the primary ring is register-replay only, does not parse opcodes" | CONTRADICTED [HW] (opcode-framed streams execute in the primary) |
| Comment 2243–2248 | `mtx3319` table POINTS `0x00020020`, LINES `0x0002103d`, TRIANGLES `0x0002305c`, STRIP `0x0002405c`, FAN `0x0002505c` | consistent with `mtx3319` (bit 17 form) [BIN]; the ICD form has bit 18 [HW] |

### C.2 src/parhelia_m9_ring.c, src/parhelia_exa.c

| file | code | disagreement |
|---|---|---|
| `parhelia_m9_ring.c` | `0x400` = requester base/control, `0x404` = final-address doorbell; `0x109c`/`0x0448` context fields; `0x1068/0x106c` fence addresses; stop code `0x45`; VRAM ring; busy poll `0x8f0 & 0x30000` | M9-only. Static recovery from `KernelLibraries.o_shipped` proves privileged requester setup at `0x400`, submission at `0x404`, descriptor save/restore at `0x500–0x524`, and completion through memory tags. Earlier userspace writes did not latch because requester control belongs in the kernel. The packet format (`idx0|idx1<<8|…`, stop `0x45`) is M9's, not the Parhelia `0x21`-terminated tag. The prototype still lacks the exact requester ownership, descriptor, tag, and bounded recovery model |
| `parhelia_exa.c` | direct-MMIO 2D: surfaces `0x1220/0x1224` + `0x1228/0x122c`, `PLNWT`, `DWGCTL` idle then op, `DSTXY`, `LEN` fires; comment: "the DMA doorbell (0x3050) lies past the 8 KB register BAR" | CONFIRMED [HW] for 2D; the `0x3050` note is right (see B.15). Sync by `0x8f0` idle only — adequate for 2D, and the file says so |
| `parhelia_exa.c` header | "Registers are poked by direct MMIO — the simplest of the two proven paths (the other is the DMA pushbuffer)" | both proven for 2D [HW]; neither has produced a 3D pixel |

### C.3 Documented but never implemented / implemented but undocumented

| item | status |
|---|---|
| Binding a vertex array for `0x8f` index mode (`0x1000 = base|tag`, stride) | documented now (A.5), never implemented; every index-mode attempt in the repo ran unbound |
| `mtx2393`'s kind-16 program bind before the address-mode draw | newly found [BIN]; not implemented; no doc mentions it |
| Kernel's own address-mode draw state (`mtx2393` full list incl. `0x1110 = 3`, `0x10b8 = 0xa0000000`, `0x8002cd` rect path) | implemented in `tri_secondary` only |
| `mtx398` default register set (`0x12c8..0x12d4`, `0x12e4/0x12e8`, `0x12fc`, `0x1314/0x1318`, `0x132c = 0x10001000`, `0x11e8 = 0x100000`) | documented in draw-sequence as "Linux adds" only partially; not implemented |
| Six stage-entry registers `0x10c0–0x10d8`, `0x10f8` | documented; deliberately not implemented after attempt 1 |
| `0x8f` + `0x07` + `0x1104` parity packets | documented [BIN ICD]; the tool never emits them |
| Per-draw aperture-descriptor writes `0x0030..0x003c` | observed [HW]; writer and purpose undocumented; not implemented |
| In-ring `0x1090/0x1094` | implemented and documented; does not fire here [HW] |
| `--secdraw`/`--secondary`/`--secmmio` variants | implemented; every variant stalls [HW]; the docs' "secondary buffer" sections predate the measurements |

---

## D. Open questions that block rasterisation

1. **GPU address space for secondary buffers (and for tag-2/3 data).** The vendor passes `slot|2` region-relative offsets for the client chunk and `buf[0x58]+offset` for kernel buffers [BIN `mtx2196`, `mtx1937`, `mtx2393`]; we pass bus addresses and every secondary call stalls [HW]. Either `buf[0x58]` is an aperture-relative base programmed through the `0x0000–0x0130` descriptor bank (the per-draw `0x0030..0x003c` writes in the gltri trace are the candidate) or the region-2 DRAW offsets are rebased somewhere not read here. Experiment: dump `0x0000–0x0130` while the vendor stack draws, and try a secondary call whose SECADDRESS is `(bus − descriptorBase)|2` against a descriptor programmed the way the trace shows. Until this is settled, the vendor's per-draw ring sequence cannot be reproduced literally.
2. **Header/tag discrimination at packet boundaries.** The vendor primary mixes header-less tags with nothing else; our primary executes header-ful opcode streams. What the parser does with a header-less tag whose top byte equals an opcode (`0x00`, `0x03`, `0x07`, `0x0f`, `0x17`, `0x41`, `0x8f`, `0xd7`) is unknown; this is the mechanism behind "header-less streams desync when opcodes appear" [HW]. Experiment: header-ful-only streams bracketed by non-blocking DWGSYNC, adding one header-less tag at a time.
3. **The bound program for a pre-transformed draw.** `mtx2393` binds kind 16 (`mtx223`, 10 instructions, entry idx 8, streamed with reset) before its `0x17` draw [BIN]. Our address-mode draws bind entry 0 with either nothing or our own transform program. A 10-instruction pass-through/setup program has to be written from the ISA documents (the vendor blob cannot ship), then bound exactly as `mtx2393` does (`0x1100` and `0x1104` = entry, `0x1110 = 3`, `0x1130 = 0x2000`, `0x10b8 = 0xa0000000`).
4. **DWGSYNC bit 0.** The vendor sets it on the first tag of every submit; on our card it never retires. Something in the vendor's init makes blocking syncs complete (IRQ mask `0x8fc = 0x10000`? the `0x400` write-back page? `0x8f0`-visible WARP state?). Experiment: replicate the arm trace's `0x400` write-back page and `0x8fc` mask, then a blocking DWGSYNC behind a 2D fill.
5. **In-ring write-back.** `[2][0x21212425][v][a|2]` fires for the vendor and not for us; the MMIO form fires. Related to D.1 (address interpretation of `0x1090` when supplied from the stream) or to a register-file mode set by the vendor init.
6. **What `0x1040 = 4` and the `0x1110–0x1118` group mean.** Every vendor preamble writes `0x1040 = 4` regardless of vertex layout; the stride for index mode must come from the bind record via `mtx3400`'s vertex-format packets [INF]. Without it an `0x8f` draw cannot be issued correctly; with address mode the engine must still know the vertex layout, and `mtx2393` gives it nothing but `0x1040 = 4` and `0x1110 = 3`.
7. **Which block-file entries a draw reads** (10 vs 26; bank A vs B; the `0x0e0..0x15f` = `r − 0xE0` window vs the collisions below r12) — no experiment has produced a pixel to discriminate, and the readback port is blind.
8. **`0x10b0` bit 17 vs bit 18** (kernel HAL vs ICD) and the meaning of `[rdi+0x430] == 1` in `mtx3319`.
9. **An execution witness inside the 3D pipeline.** Non-blocking DWGSYNC proves the packet retired; VRAM proves a fragment landed; nothing observes the WARP itself. The readback port is blind [HW]; the `0x1090` MMIO write-back only says the ring retired.

---

## E. Summary of the most consequential contradictions

1. **Level confusion on region 5.** draw-sequence.md's "GROUND TRUTH" retraction ("the `0x8f` packet is inline in the level-1 ring, the second-level model is wrong") misidentifies the level: `mtx2196` submits region 5 through `mtx1937`'s secondary call, so the captured ring is the level-2 buffer [BIN]. The hardware nonetheless executes that content inline in our primary ring [HW], which makes the *vendor's* split a convention — but it does not make the retraction correct, and every doc that reasons from "the vendor puts opcodes inline" inherits the error.
2. **Our secondary calls carry the wrong kind of address.** The vendor never passes a bus address to SECADDRESS/SECEND: region offsets `|2` for the client chunk, `buf[0x58]+offset` for kernel buffers [BIN]. This is the strongest candidate for "secondary calls stall on any content" [HW] and for the in-ring `0x1090` never firing, and no doc records it.
3. **`0x1000` is the vertex-stream base, not a preamble constant.** `mtx2147` writes `mtx1132(vertexRegion, offset)` there; `mtx2393` writes the space flags [BIN]. warp-binding §1, draw-sequence Phase 5 and every tool path write 0. For index mode this is the missing array binding; for address mode it must carry the vertices' space tag.
4. **The kernel's own address-mode draw binds a program the docs say nobody binds.** `mtx2393` → kind 16 (`mtx223`) with reset and entry idx 8, written to `0x1100` and `0x1104` [BIN]; warp-binding §3 marks kind 16 unreferenced. Our `0x17` draws run with entry 0 and no matching program.
5. **The block-load emitter exists and is per-draw.** session-summary.md's "the kernel never emits a block-load packet" and "no host-side code pushes the block file" are wrong: `mtx1342` computes the header (0x9022c) and is reached per DRAW record through `mtx2196 → mtx606 → mtx1331 → mtx1651 → mtx3400 → mtx693` [BIN]. constant-upload.md already retracted it; session-summary.md still states it.
6. **Refuted registers survive in five files.** `0x12a0`, `0x1058`, `0x1020`, `0x103c`, `0x101c`, `0x105c`, `0x1064`, `0x1230`, `0x1224`, `0x1298`, and the `0x1100–0x117c = 0xffffffff` init remain in 3d-driving.md, 3d-state-complete.md, gl-driver.md, gl-feature-map.md, fragment-ops.md and accel.md (register columns). `0x12a0` is named in no vendor binary.
7. **`0x12b0 = 0x3ff0f800` is a transcription error** repeated in draw-sequence.md; the Windows literal is `0x3fff8000` and the Linux HAL computes the value [BIN]; hardware truth agrees with `0x3fff8000`.
8. **The `0x300` arm formula is wrong in three files** (draw-recipe §1.1, draw-sequence Phase 1, drm-design §5): the register is 16-bit and the vendor holds `0x140` [HW].
9. **DWGCTL for 3D.** 3d-driving.md and draw-sequence Phase 7 still specify the `0x07`/`0xc07` family; the binary says those are flush opcodes and 3D is `0x10cc`/`0x2010cc` (with `0x12f4 = 31.0`), the rect path `0x8002cd` [BIN]; `0xc07` draws nothing and `0x11` locks the engine [HW].
10. **Packet-count "three rules" are one rule plus one exception.** Every opcode's low 16 bits are payload dwords − 1 (header-ful register lists included); `0x07` counts 16-bit indices [BIN]. command-construction.md's "count = number of registers" and draw-sequence's three-rule table both mis-frame multi-tag register packets, and "0x21 = next dword is a header" is false inside a header-ful packet.
11. **Instruments retracted after the fact.** Every "0 registers changed", "packets consumed, engine idle, nothing drawn" and "the fence moved" conclusion before 2026-09-05 rests on the readback port, a mis-anchored VRAM scan, a stale `0x41c`, or a ring contended by X [HW]. warp-binding.md attempts 1–5, draw-sequence's P690 negative and "the failure is inside the 3D pipeline" section, and vertex-isa's "--warpring is the right shape" all remain in the tree without the retraction attached.
12. **`0x8fc` is the IRQ mask.** engine-init.md and `engine_init()` treat it as a DMA-mode selector (`pStatus[0x8fc]` is a struct field); writing 0 masks interrupts and has no bearing on bus-master DMA [BIN, doc] + [HW].
