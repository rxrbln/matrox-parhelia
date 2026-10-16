# M9148 vendor X cold initialization, traced

This note turns the live vendor-X register trace in
[captures/m9x-cold-init-20260915-v1](../captures/m9x-cold-init-20260915-v1/README.md)
into an ordered replication reference.  Target: `root@192.168.1.56`, Linux
`2.6.30-dist`, boot ID `aab643c8-e10f-49bb-848a-09c4846d34d1`, Xorg 1.5.2 with
the pinned vendor X 7.4 `m9x_drv.so` (SHA-256 `17b8d8c0…`).  Every value below
is an observed accessor argument recorded before the write executed; readback
of write-only registers was not used.

Scope: userspace vendor-X writes through the driver's register and PCI
accessors.  The kernel render service (`m9x.ko`) is captured separately by the
instrumented observer module and is described in its own section once dumped.
Firmware execution before Linux is outside this trace.

## Capture mechanics and coverage

* Part 1 (`xinit-mmio.jsonl`): seq 0..200012, 200000 writes, ended by the
  observer write limit inside the BAR0 self-test.
* Part 2 (`xinit-mmio-part2.jsonl`): resumes the same paused X process through
  gdbserver; see the capture README for the takeover.  The rest of the BAR0
  self-test is suppressed by pinned loop gates, not logged per dword.
* Register space 0 is the MMIO BAR (`febfe000`, 8 KiB); register space 1 is
  the vendor BAR0 mapping (`d0000000`, 256 MiB).
* Two bit-banged serial buses dominate the write count and are collapsed
  here: `0x304` (19994 writes, final `00011010`) with `0x318` strobes
  (942, final `30000003`) before the cold gate, and the DDC pins
  `0x1c48`/`0x1c4c` (84363 in part 1).

## Order observed in part 1

Before the cold gate the driver writes PCI `0x44 = 0x318`, then reads the
board serial ROM through `0x304/0x318`.  `enable-hardware` (`m9x25172`) is
entered at seq 104729 and `cold-initialize` (`m9x21146`) at seq 104732.  The
cold path then performs, in this order:

| seq | writes | recovered role |
|---:|---|---|
| 105310 | `358 = 00010001` | reset/configuration preamble (`m9x4358`) |
| 105311..105372 | PCI `00..fc` replayed from the saved header, ending `04 = 00100107` | restore after reset; bus master on |
| 105373..105375 | `e4c = 000fb370`, `e50 = 000f5b1e`, `358 = 00070000` | preamble completion |
| 105377..105402 | `1900/1b00/1e00/1f00 = 00030000`, `1c28 = 0f021084`, `1c24 = 0`, `1c10 = 0,80000000,0`, `1c24 = 1`, the four controllers `= 0`, `1c00/1c08 = 02000001`, `1c04/1c0c = 1`, `1c24 = 101`, `1c10 = 2`, `1c80/1c84 = 01001400`, `1c9c..1ca8 = 00040000` | display route reset and clock holds (`m9x7139`, `m9x15067`) |
| 105403..105406 | `8fc = 0`, `8f8 = 032f7f1f`, `8dc = 0`, `3a8 = 00020a05` | IRQ mask, W1C acknowledge, common defaults (`m9x2800`) |
| 105407..105414 | `1c00/1c08 = 02000001`, `1c04/1c0c = 1`, `1800/1a00/1980/1b80 = 00040000` | outputs held |
| 105415..105425 | `f3c = 0e0f0420`, `f40 = 081f060c`, `f44 = 0d080c18`, `f48 = 071f0408`, `f4c = 091f0904`, `f54 = 40`, `f58 = 42088000`, `f5c = 10601060`, `f60 = 0`, indirect `f18/f1c` select `1ffe = 0` | MC defaults with PInS patches (`f58`, `f5c` differ from the static fallbacks `32000000`, `0`) |
| 105426..105484 | `e24 = 70000000`; nine selectors held and set to `00020000`; `e30/e34/e38` each programmed `80208462 → 00008462 → 812093eb → 010093eb → 81209b38 → 01009b38` (twice for the last word); selectors released to `e00 = 00020006`, `e04 = 00020008`, `e08..e20 = 00020004` | system synthesizers (`m9x13609`) |
| 105485..105489 | `f2c = 02c8eca7`, `f30 = 33a63f74`, `f34 = 0268d605`, `f38 = 6d`, `f04 = 0448803e` | MC register image, patched (fallbacks `02c8e8a7 23a63474 026655a3 2b 0440803e`) |
| 105490..105537 | indirect MC image through `f18 = 0001_1fNN` / `f1c = value`: `ff=5`, `00=0`, `02=42`, `07=0`, `08..0b=4000`, `11=249`, `12=249`, `13=55`, `16=55`, `17=0`, `1a..1d=0`, `23=0`, `24=90000`, `25=dd22ee11`, `26=7788bb44`, `27=0`, `2a=1`, then select `1ffe` | indirect MC image (`m9x21748`), patched |
| 105538..105551 | `f0c = 00200000`; `f14 = 00000042 → 02000042`; `f10 = 1`; `f14 = 04000042`; `f10 = 00010000`; `f14 = 01000042 → 05000042 → 09000042 → 00000042`; `f10 = 000c7300`; `f14 = 10000042`; `f08 = 04f3c000`; `f0c = 00200000` | MC enable pulses and tail (`0x0f14` packed `42`, `f10` patch `000c7300`, `f08` patch `04f3c000`) |
| 105552..105555 | `4 = 0`, `8 = f0000000`, `c = 00010001`, `0 = a0000000` | BAR0 window slot 0 for the self-test |
| 105557.. | space 1: dword `i` at BAR0 `4*i` for `i < 0xffff`, then `i<<16` for `0x10000 <= i < 0x1ffff`; read back and compared | destructive VRAM self-test (`m9x24614`) |

The final part-1 value of every space-0 register written is:

```text
0000=a0000000 0004=00000000 0008=f0000000 000c=00010001 0304=00011010 0318=30000003
0358=00070000 03a8=00020a05 08dc=00000000 08f8=032f7f1f 08fc=00000000 0e00=00020006
0e04=00020008 0e08..0e20=00020004 0e24=70000000 0e30/0e34/0e38=01009b38
0e4c=000fb370 0e50=000f5b1e 0f04=0448803e 0f08=04f3c000 0f0c=00200000 0f10=000c7300
0f14=10000042 0f18=00011ffe 0f1c=00000001 0f2c=02c8eca7 0f30=33a63f74 0f34=0268d605
0f38=0000006d 0f3c=0e0f0420 0f40=081f060c 0f44=0d080c18 0f48=071f0408 0f4c=091f0904
0f54=00000040 0f58=42088000 0f5c=10601060 0f60=00000000 1800/1980/1a00/1b80=00040000
1900/1b00/1e00/1f00=00000000 1c00/1c08=02000001 1c04/1c0c=00000001 1c10=00000002
1c24=00000101 1c28=0f021084 1c80/1c84=01001400 1c9c..1ca8=00040000
```

## Relation to the clean-room driver

The pre-X baseline on the same boot already reads `f04 = 0448803e`, the value
the cold path writes, so firmware POST performs the same patched MC programming
before Linux starts.  The clean-room driver is warm-only and writes none of the
`358`, `e00..e50`, `f08..f60`, `304/318` or display-route values above.  Its
`3a8` update preserves the firmware value and sets bits 17:16 to `01`; the
vendor X cold path first writes the full word `00020a05`, and the kernel
service later applies the same 17:16 update.  Whether firmware leaves the
low `3a8` bits at `0a05` on a clean-room boot is not established by this
trace and must be read on such a boot before treating it as a difference.

The cold path programs BAR0 window slot 0 only for its self-test.  The kernel
service later invalidates all 20 slots (`m9x1172 → m9x115`), matching the
clean-room driver.

## Kernel service layout on the 1 GiB M9148

Source: the observer blob `m9cold-capture-123934.bin` and the BAR0 image
`vram-root-0-ec1000-124346.bin`.  The vendor module reports
`TotalMemorySize = 0x40000000` and sizes its tables for it.  The size comes
from PInS, not BAR0:

- **Parser:** `m9x320 → m9x798` follows the u16 pointer at ROM `0x7ffc`,
  checks the signature `0x412e` and reads PInS+0x44.
- **Nibble lookup:** each of the four low nibbles `n` in 1..6 adds
  `0x2000000 << (n - 1)`.
- **Default:** the sum replaces the default only if it is not smaller.
- **This board:** `M9148-E1024LAF`, PInS+0x44 = `00014444`, total
  `0x40000000`.

The tables are laid out as follows:

| region | range | contents |
|---|---|---|
| primary compact table | `[0x801000, 0x921000)` | local identity PTEs `|8` at `0x801000 + 4*(off>>12)`; class-1 PTEs `|0xa` from `0x901000` |
| root identity table | `[0x921000, 0x9a1000)` | entries `(i<<12)|8` |
| tail | `[0x9a1000, 0xac1000)` | zero |
| restore pool | `[0xac1000, 0xc42000)` | prefix, 1024 records, filler `45454545` |
| mode-0 pools | local `0x10ec1000` (4 MiB), `0x112c1000` (1 MiB), `0x113c1000` (`0x60000`), `0x11421000` (`0x500`) | published with `31c=f,1,0`, no `314` |

Service writes, in order:

- **`m9x1172`:** `30c=0`, `3a8=00010a05`, 20 slots `0000ffff`.
- **`m9x53`:** windows `132c/1330=801000`, `1334=920000`, `360=921000`,
  `364=9a0000`, `f64=9a1000`, `f68=ac0000`, `f74/f78=801000`,
  `f7c=920000`; then `31c=f,1,0`, `30c=1000`, `31c=c,1`, `f0c=00200040`.
  The second transition repeats the windows.
- **Restore pool:** published as mode 3, then `31c=f,1,0`, `314=1`.
- **`m9x109`:** `400=055e0002`, `109c=00080000`, `448=0`, `1068=08dea002`,
  `106c=1cbbc002`, `10ec=00ac1200`, `1094=00ac1200`, `1098=00c40c06`,
  `44c/450/454=00280800/1f3f2707/00070703`.
- **Tail:** `m9x926` pools, `m9x115` twice, then the first `404=055e001c`.

## Vendor triangle submission

Source: `gltri-flat-124914/` and `m9cold-gltri-124914-order.txt`.  One flat
triangle takes 17 ExecuteDma calls and 34 kernel CALLs, all class 1, and
renders correctly.  Per call the kernel:

1. publishes any new BmBuffers (32 KiB: `31c=f,1,0`, `314=1`; `0x21000`
   arenas: `31c=f,1,0`, with one `314=1` closing the batch);
2. emits one to three CALLs `{45452d2c, A|9, end}` on the mapped 32 KiB
   buffer;
3. rings `404` once.

No other MMIO accompanies a 3D doorbell.  X 2D operations bracket their
doorbells with BAR0 window slot 1 (`0014` base, `0018=60010020`,
`001c=00010020` or `00010010`, `0010=124b124b`, then `0010=0000ffff`).
Vendor X also writes `314=1` (`m9x12857`) and `31c=0` (`m9x27120`) directly;
without direct MMIO it issues ioctls `0xc010460e` and `0xc010460f` instead.

The command mappings match `captures/m9x-linux-gltri-20260914` except for:

- class-1 addresses;
- the fence-page word (`0b221002`);
- the drawable origin (`f2`);
- the scanout stride word (`f0210400` at 1024 columns).

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
