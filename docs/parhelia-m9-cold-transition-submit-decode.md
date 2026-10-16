# M9148 cold transitions, publication kick and primary submission

This note records binary decodes made on 2026-09-15 while reviewing the
clean-room kernel's init and command-submission paths (`BUGS.md` at the
workspace root).  Evidence is the shipped Linux core
`KernelLibraries.o_shipped`, SHA-256
`bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`, the
preserved `captures/m9x-linux-gltri-20260914` images, and
`captures/m9x-requester-live-20260914/ioctl-mmio.csv`.  All addresses are
`.text` offsets.  Several earlier notes are corrected by this one; each
carries a dated pointer here.  Recheck the file hashes, every instruction
anchor, the `m9x582` relocation order and the capture framing with:

```sh
python3 tools/m9-cold-transition-submit-evidence.py
```

## 1. The cold `m9x421` transitions emit no packets

`m9x421` (`0x16410`) has two halves: direct MMIO window writes and a packet
half (`e6=2` or `e6=0,2`; then `47=11a4,1308`, `e3`, `43`,
`e6=40000000`, `22=1088|20000000`).  The packet half is gated on the
primary requester object `B = [Y+0]` (`Y` = the object at `dev+0x740`):

```text
1652a  test %rbx,%rbx ; je 16632          ; no builder -> MMIO only
16ab0  mov 0xc8(%rbx),%r15d ; test ; je 16632
16632  xor %ebx,%ebx ; jmp 16a32          ; MMIO windows only
16908  test %rbx,%rbx ; je 16af8          ; no 47/e3/43/e6/22 tail, no 1088 read
```

`B+0xc8` is a construction refcount: zeroed by the constructor, incremented
only at the end of `m9x182` (`0x2e83f..0x2e842`, after the restore pool and
`m9x1192` succeed), decremented in `m9x812` (`0x2d4bf`).  The service-owner
`m9x582` (`0x2ed80`) calls, by relocation:

```text
2ee63  m9x595                    service allocation (rep stos zero)
2eebf..2ef91  eleven initializer-table entries (.rodata+0x5a20, stride 0x10)
              entry 8 = m9x1042 -> 0x121d0 -> m9x1172 -> m9x92 -> m9x53
                        -> m9x421 #1 (0x187aa, publish=1)
              entry 10 = m9x723 -> m9x950
2efd1  m9x1187 -> m9x421 #2 (0x121bc, publish=1)
2efd9  m9x865
2efe1  m9x949 -> m9x182 -> m9x109        ; B+0xc8 becomes 1 here
2efed  m9x926   2eff5 m9x301   2effe m9x1141   2f007 m9x115
```

Both cold transitions therefore run with `B+0xc8 == 0` and write only the
windows.  The 5+14 dword clean-room transition streams had no vendor
counterpart; kernel commit `b788cc4` removes them.

The warm callers that do emit the packet half are `m9x887` (`0x17ea5`,
publish=0), `m9x715` (`0x19fa3`, publish=0) and `m9x42` (`0x193cb`).  In
`m9x887` the packet path (`m9x73` before `m9x328`, `m9x421` after) is gated at
`0x174c5..0x174f8` on `MmFlags` bits 3, 24 and 25 together; `0x100e8` fails
bit 24 (`0x174d3`), so on the Linux M9148 profile neither runs.  The first
packet the vendor engine executes is sequence 0, which opens with
`4545e3e6 3 <target>` (`bm-00.bin` offset 8): `e6=3`.

## 2. Every mode-1 allocation ends with `0x314 = 1`

`m9x810`'s mode-1 branch (`0x38320..0x3839c`) runs after `m9x1030`
(allocation, `m9x887 -> m9x328` CPU mirror, `31c=f,1,0`) and calls the
unnamed function at `0x37a20` (`m9x862+0x20`; `m9x862` itself at `0x37a00`
only zeroes a 3-dword parameter block):

```text
37a3a  loop m9x880(res,1) until 0        ; wait for the last token stored at alloc+0x60
37a4b  MMIO[0x314] = 1                   ; unconditional
37a86  testb $0x4,(desc)  ; je 37d78     ; window-slot bind only for descriptor bit 2
```

`m9x1030` builds descriptor byte 0 from bit 4 (=1, `0x1d239`), bit 5
(`request+0x30 == 0`, always 0 because `0x1cfe9` sets it to 1), bit 6
(candidate type 3) and bit 7; never bit 2.  So system resources (BmBuffers,
restore pool, vertex/program arenas) are not bound into the window table
(section 4), but each one is followed by a `0x314` kick.  The order per
resource is: mirror, `31c=f,1,0`, `314=1`.  No `0x314` is written inside
`m9x328`; `linux-compact-pte-publication-re.md` is right about that routine
and wrong about the allocation as a whole.

## 3. The restore tag is 1

`m9x109` writes `10ec = ((R+0x200)&~0x1f) | (restore+0x34 & 3)`
(`0x2d24f..0x2d264`) and `m9x72` writes record `+0x4c = (W+0x440) |
restore+0x34` (`0x36a18`).  `restore-pool-template-closure.md` concluded
`T=0` because no instruction writes `request+0x1b4`.  The writer is
`m9x286`, called by `m9x810` with `rdi = request+0x68` and `ecx =
candidate type` (`0x38154..0x38192`); `0x10ac4: mov %ecx,0x14c(%rdi)`
stores the type at `request+0x1b4`.  The class-1 arm copies it to
`resource+0x30` (`0x38320..0x3832f`) and, on success, to `+0x34`
(`0x38411..0x3841b`).  For the restore pool (`m9x655`, `MmFlags=0x100e8`,
`BmDebugFlags=0xa0`) candidates 0 and 2 are disabled
(`0x36bfd..0x36c50`, `0x36cbb..0x36d08`) and candidate 1 is taken.  The GL
BmBuffer pool uses the same path and returns class 1 in every capture;
`m9x445`'s class-to-tag table at `.rodata+0x5ca0` is `{0,1,2,2,2,0}`.
Every constructor defaults `+0x30/+0x34` to 1.  Therefore:

```text
MMIO[0x10ec] = ((R + 0x200) & ~0x1f) | 1
MMIO[0x1094] =  (R + 0x200) & ~0x1f
MMIO[0x1098] = ((R + 0x17fc00) & ~0x1f) | 6
record+0x4c  = (W + 0x440) | 1
```

The compact PTE nibble stays `0xa` for both pools (bits assembled in
`m9x328` `0x12c3..0x1393` from descriptor bits 4/5/7).

## 4. `m9x115` and the post-arm calls

`m9x115` (`0x20620`) publishes a 20-slot buffer window table at MMIO
`0x000..0x13f`, 16 bytes per slot (`obj+0xc0 = 0` base, `obj+0xc4 = 0x14`).
An empty slot writes only `16n+0 = 0x0000ffff`; a live slot writes
`+4 = address|type`, `+8`, `+0xc`, then `+0 = start_page | end_page<<16`
last.  Slots are bound by `m9x1102 -> m9x716` for descriptor-bit-2
allocations only (section 2), so the table stays all-invalid on the
first-render path.  `m9x1172` (`0x222c0`; also `30c=0`,
`3a8=(old&fffcffff)|10000`) calls it once at construction; `m9x1141`
(`0x209b0`, refcount 0->1) and the explicit call at `0x2f006` repeat it
after the arm.

The `m9x565` low-VRAM root is also deliberately absent from this table.
`m9x565` stores `0x16` at request `+0x10` (`0x1da30`) before calling
`m9x1102`.  The set `0x10` bit makes `m9x1102` take `0x21c80 -> 0x22240`,
bypassing `m9x919`; `r15b` becomes zero and `0x21f38` selects the dynamic
list at `0x21f7f`, bypassing `m9x716`.  The root therefore has no fixed
slot for `m9x115` to republish.  Its GPU-visible activation is the compact
table CPU mirror followed by the `0x31c = 0xf,1,0` sequence in `m9x53`,
not a live `0x000..0x13f` window entry.

`m9x865` (`0x3e2a0`) has no MMIO: it allocates the two completion dwords
through `m9x479 -> m9x286(fmt 0x78, ecx = mode ? 0 : 2) -> m9x1030`, with
`mode = ((hwcaps & 0x200004) == 0x200004) ? (BmDebugFlags & 0x10 ? 1 : 0) : 1`
(`hwcaps` = `hwstate+0xe0`, read from hardware), stores the GPU address at
`hwstate+0x00/+0x28`, the space tag at `+0x18/+0x40`, and seeds both words
with `0x10`.  `m9x109` then writes `1068 = [hw+0x18]|[hw+0]`,
`106c = [hw+0x40]|[hw+0x28]`.  `m9x182` kicks `0x314` after each of its
three queue passes only when `hwstate+0x18 == 0` (`0x2e405`).  `m9x926`
allocates the four BM pools (`0x400000/0x100000/0x60000/0x500` bytes)
through `m9x1030`; `m9x301` clears a pointer.

The `m9x926` allocations are transitively GPU-visible. The publication gate
`m9x535` reads applied `System\\MmFlags=0x100e8` from device `+0x640`,
not the separate `0x300004` hardware capability word. `m9x344` constructs
this wrapper from configuration `+0x210`; `m9x233 -> m9x1079 -> m9x1096`
applies it before the allocation path, and `m9x184` returns the applied
value at wrapper `+0x14`. Bit `0x8` enables mode-0 publication, and bit
`0x40` additionally enables mode 1.

Each pool is mode 0, with no external page list. `m9x887` selects GPUVA page
index and rounded byte extent; `m9x328` writes identity PTEs to the primary
compact child:

```text
slot[i] = 0x801000 + 4 * ((GPUVA >> 12) + i)
PTE[i]  = (GPUVA + i * 0x1000) | 0x8
```

The fresh `m9x565` local range begins at `0xd41000`. With no intervening
mode-0 allocation, first-fit placement gives these four pools:

| GPUVA | requested bytes | pages | first PTE slot |
|---:|---:|---:|---:|
| `0x00d41000` | `0x400000` | `1024` | `0x804504` |
| `0x01141000` | `0x100000` | `256` | `0x805504` |
| `0x01241000` | `0x60000` | `96` | `0x805904` |
| `0x012a1000` | `0x500` | `1` | `0x805a84` |

Every pool's CPU mirror ends with `31c=f,1,0`. There is no enclosing
`314=1`: `m9x926` calls `m9x1030` directly, bypassing the mode-1 `m9x810`
helper. These four publications follow `m9x109` and precede the final two
`m9x115` passes. The new DRM implementation reserves exactly those VRAM
ranges before KMS and client allocation, and publishes them at that point.
The bases above are a static fresh-service prediction; the saved client
captures do not contain the four internal allocation objects. The
[cold-service observer](parhelia-m9-cold-service-capture.md) can confirm their
runtime placement and the actual write order.

## 5. Primary submission

Builder class vtable `m9x43+0x10` (`.rodata 0x40fb0`), ctor `m9x654`
(`0x2c930`), factory `m9x1182` (`0x2ff10`).  Fields: `+0x8` head, `+0x10`
base, `+0x18` limit, `+0x5c` pad alignment, `+0x88` tail reserve (`0x80`
dwords), `+0x110` current BmBuffer.  Tracker `EC+0x10`: group A (`1a/42`
token -> MMIO `1068`, `TC+0x00/+0x18/+0x10`) and group B (`e3` fence ->
MMIO `106c`, `TC+0x40/+0x28/+0x38`); both step `0x10`, written `|1`, read
`& ~0xf`.

| slot | function | effect |
|---|---|---|
| `+0xd0` | `m9x708` (`0x2be0`) | appends `4545421a <TC[0]\|TC[0x18]> <token\|1>`, returns token |
| `+0xd8` | `m9x604` (`0x2d00`) | `4545451b <TC[0x40]\|TC[0x28]>` then the `e3` group |
| `+0xe0` | `m9x81` (`0x2ac0`) | `45454543 <token>` when not yet retired |
| `+0x100` | `m9x954` (`0x2d920`) | flush through `m9x1039` |
| `+0x150` | `m9x528` (`0x2db30`) | space / buffer switch |

The complete vendor primary for the first `ExecuteDma` (`execute-00.bin`,
`S = ctx+0x44`, class 1, `N-8 = 0xf98`) is twelve dwords:

```text
45452d2c (S+0x008)|1 (S+0x540)|2
45452d2c (S+0x540)|1 (S+0xf74)|2
45452d2c (S+0xf74)|1 (S+0xfa0)|2
4545421a  TC[0]|TC[0x18]  token|1
```

Captured `0x404` append deltas `{0:18, 24:9, 36:15, 48:58}` equal
`12 + 12*ranges`, which also proves `B+0x80` bits 1 and 2 are clear on the
tested board (no per-submission `1b/e3/43`, no drain).  No padding is emitted
for a class-2 primary (`builder+0x5c = 0`, `m9x182 0x2e84f..0x2e859`).

Buffer switch (`m9x528`, depth 0, `0x2e170..0x2e1ed`): rotate the BmBuffer
ring; after one fruitless cycle `MMIO[0x314]=1` only if `TC[0x18]==0`;
append at the old head `453e421a <TC[0]|tag> <token|1> <newbase|class>`;
rebase the head to the new buffer with limit `end - 4*0x88`; flush.  MMIO
`0x400` is written exactly once in the whole object, in `m9x109`
(`0x2d19c`); `0x404` exactly once, in `m9x1039` (`0x2d830`).  `m9x1039`
skips both the class-0 `31c=0` and the `0x404` kick while `TC[0x54] != 0`
(18 of the 100 captured calls).

Completion wait (`m9x721`, `0xee30`): read `0x8f0`, return on
`0xffffffff`; `x = *TC[0x30] & ~0xf`; latch `TC[0x48]` and bump `TC[0x4c]`
on regression; finish on `TC[0x48] >= target`; `MMIO[0x314]=1` only when
`TC[0x40] == 0`, i.e. when `0x106c` is unprogrammed.  There is no timeout,
and `0x8f0` busy bits are not a progress criterion.

## 6. Sequence-0 image

The kernel's `m9_seq0_framed_payload` (998 dwords) is byte-identical to
`bm-00.bin[8,0xfa0)`; `execute-00.bin` descriptors are `[0,0x538)`,
`[0x538,0xf6c)`, `[0xf6c,0xf98)`; the buffer is zero beyond `0xfa0`.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
