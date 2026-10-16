# M9128 inherited initialization admission

The vendor's initialized getter is a CPU lifetime predicate. Neither that
getter nor PCI memory decoding proves a completed cold hardware transaction.
This checkpoint closes the saved Linux X caller, software bucket and two
provider paths. It supplies no new hardware writes or black-image cause.

Run the exact instruction and CPU fixture checker with the saved binaries:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-inherited-init.py \
  --image /home/ai/src/parhelia/xf86-video-parhelia/m9xdriver-x86_64-1.4.3-80847-20140908-build_98/xserver/1_13_0/m9x_drv.so \
  --core /home/ai/src/parhelia/xf86-video-parhelia/m9xdriver-x86_64-1.4.3-80847-20140908-build_98/kernel/KernelLibraries.o_shipped
```

The checker verifies complete image hashes, instruction bytes, actual ELF
relocations, getter/setter execution and the kernel's software predicate. Its
controlled CPU bucket is explicitly a fixture. It does not construct a real
board, supply an OS ioctl response or emulate a hardware status read.

## Getter and explicit caller choice

Linux X `m9x2348` at `0x2c83f0` calls `m9x17751` at `0x2ca310` with bucket type
`0x12`, index zero and element size four, then loads the returned DWORD. The
lookup joins the board's CPU bucket count and offset to the backing pointer
at object `+8`. There is no MMIO access in the getter or admitted lookup.

Board construction calls `0x2c8410` at `0x2bef24`; that constructor allocates
the bucket element and writes zero at `0x2c8463`. Setter `0x2c83d0` uses the
same lookup and writes the caller's DWORD. The source distinguishes three
producers of its value:

| producer | actual source action |
|---|---|
| `0x2b4fe0` | Under the board lock, calls the setter with one at `0x2b4ff1..0x2b4ff6`; performs no hardware initialization. |
| `0x2b71f0` | Calls the setter with zero at `0x2b7247..0x2b7249` before reconfiguration. |
| `0x2c8190` | Cold initialization writes one at `0x2c8381`, including its exhausted failure path. |

The low-level wrapper `m9x1325` at `0x2b06e0` accepts an explicit caller
boolean. Zero calls `0x2b4fe0` and marks the CPU word one. Nonzero calls
`0x2b5010`, which enters the serialized `0x2b5650` enable gate. That gate reads
the word: zero enters cold transaction `0x2c8190`; nonzero calls the warm
helper `0x2eb7b0`.

The vtable join is concrete: board constructor `0x294030` calls `0x2afcd0`,
which calls `0x2b03f0`. Its actual GOT relocation installs vtable address point
`0x12193b0`; slot `+0x20` contains `0x2b06e0`. Enable wrapper `0x294670` calls
the retained provider through `0x300bb0`. A nonzero provider result selects
ESI zero at `0x29469d`, then invokes that slot at `0x2946a5`. Zero selects ESI
one at `0x29481f`, then invokes it at `0x29482a`. There is also an explicit
enable call with ESI one at `0x294597..0x2945a2`.

## Both provider paths

Provider selection in `0x294bf0` is bounded by the caller's provider mode.
Mode one selects `0x302f70`. Mode two first tries constructor `0x305170`,
queries it with `0x305a80`, opens it with `0x305a30`, and checks the actual
reported ABI version `1/4/3`. A successful mode-two provider is retained at
`0x294fb7`; failure falls back to `0x302f70`. The retained provider is placed
in board `+0x70`, then `0x300b40` records it at the wrapper's `+8`, published
as board `+0x78`.

For the mode-one/fallback provider, the actual constructor `0x302f70` resolves
GOT `0x1258930` to table `0x1219520` and installs its address point `+0x10`.
The initialized getter at slot `+0x18` is `0x858b50`: exactly `xor eax,eax; ret`.
`0x302fd0` returns one without replacing the table. This selected provider
therefore requests the enable gate rather than marking the bucket initialized.
This is not a claim that every X session selects the fallback provider.

For the successful kernel provider, constructor `0x305170` resolves GOT
`0x125d9a8` to table `0x12197e0`. Slot `+0x18` is `0x3058f0`, which issues
ioctl `0x80044615` using its live file descriptor. On success it returns the
four-byte reply; on failure it returns zero.

The saved kernel core closes that query's meaning. Driver table `+0x50`
relocates to `m9x367` at `.text+0x5140`, then dispatches to `m9x1185` at
`.text+0x4e80`. That dispatcher masks the request to its low eight bits. Jump
table `.rodata+0x2250`, entry `0x15`, relocates to `.text+0x5032`, which copies
four bytes to the caller. The copied word was formed at
`.text+0x4e9a..0x4eaf` as:

```c
initialized = core_device->field_18 != NULL;
```

The core constructors at `.text+0x3ec0/0x3ef0` clear that pointer. Producer
`.text+0x3f80` calls `m9x295` and stores its returned owner at
`.text+0x3fa4`. The query observes software ownership; it does not read the
memory controller or validate its cold setup. A fresh core with no such owner
returns zero and requests initialization. A retained owner lets the X caller
mark its own bucket initialized. The actual query reply for a newly captured
physical boot remains separate evidence.

## Hardware branch and native distinction

The warm helper dispatches on the actual board chip field. For `0x0540`,
`0x2eb7c8` jumps to `0x2f2c10`; the `0x2539` sibling has a different helper.
The M9 helper reads `0x1c10/0x1fdc` and can return immediately when
`(1c10 & 3) == 2` and `1fdc` bit 16 is set. Otherwise it runs the source clock
and display transition: per-head calls, `1cxx` writes and the final `1800`
bit-13 clear. This is the saved warm display/clock branch. It does not upload
the FAA shader/RS owners or arm `400/404`.

The cold branch and its PInS-specific clock/MC programming are documented in
[the recovered cold transaction](parhelia-m9-cold-init-re.md). It includes
reset/configuration ordering and destructive VRAM verification. The software
bucket is set even after verification retries are exhausted, so treating it
as a hardware-success predicate would be incorrect.

Native `m9x_firmware_initialized()` in `m9x/m9x_drv.c` currently reads PCI
command and tests `PCI_COMMAND_MEMORY`. That only checks aperture decoding.
It does not reproduce either software provider policy or establish the cold
transaction's board inputs. This is an admission distinction. The present
native FAA hardware results do not establish that cold programming is missing
or that it causes black output: VRAM publication/readback and CPU mapping
succeed, VS outputs exist, and genuine requester retirement is observed.

The separate private source-owned context admission skipped the extra Linux
setup submission while retaining fresh CPU origin `0x10/next 0x20`. The parent
reported fresh-boot `40e7a2fa`, module `8664d0c`, real expected retirement
`0x140`, CPU positive control passing, and unchanged black output/main/AA
observations. That distinguishes the extra Linux submission from this inherited
hardware boundary; it does not execute the vendor cold transaction.

No native cold/reset patch follows from this checkpoint. The remaining exact
prerequisite question is whether the physical M9128 boot establishes the
board-specific cold state assumed before the captured Windows MD service
graph. The saved Windows miniport publishes a global PLL callback, but its
actual indirect invocation and cold/warm policy remain unclosed. Neither a
CPU getter value, three matched MD service controls nor PCI decoding closes
that lifetime. Any implementation must retain actual board PInS inputs and
the complete bounded transaction rather than copying selected register words.
