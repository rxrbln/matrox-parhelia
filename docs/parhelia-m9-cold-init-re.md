# M9/Phoenix cold initialization: recovered vendor sequence

For the current runtime evidence, limits and executable observer setup, see
[initialization findings and tracing harness](parhelia-m9-cold-init-tracing.md).
That capture is still in progress; the static sequence below is not itself
a recording of a cold boot.

This note answers one narrow bring-up question for PCI device `102b:0540`:
what the saved vendor stacks do when M9 memory and clocks are not already
initialized.  Addresses are virtual addresses in the ELF `m9x_drv.so`, virtual
addresses in the PE `MxMM.sys`, and section-relative addresses in the relocatable
Linux kernel core.  `[BIN]` means a direct binary/source observation and `[DER]`
means a conclusion from joined observations.

The short answer is now firm:

* the saved Linux **kernel** wrapper and `KernelLibraries.o_shipped` do not cold
  initialize the board;
* the saved Linux **X driver** contains and calls a real Phoenix cold path.  It
  reads PInS through its serial-flash communication layer, programs system PLLs
  and the memory controller directly, and verifies VRAM;
* the Windows miniport publishes a hardware-initialization callback which
  performs a related global PLL sequence.  Its actual caller is hidden behind a
  function table, so the precise Windows cold/warm gate is not statically joined;
* none of these cold routines arms the 3D requester.  Requester construction,
  interrupt enable, and rendering-service creation are later lifetime phases.

Run `python3 tools/m9-cold-init-evidence.py` from the repository root to verify
the exact inputs, function bodies, direct-call joins, and key constants used by
this note.

## 1. Evidence set

| input | SHA-256 |
|---|---|
| `kernel/src/m9x_base.c` | `64a373b539660e59637bed937bbcf713ae9cd378989be29ad72ae473789e804f` |
| `kernel/KernelLibraries.o_shipped` | `bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7` |
| `xserver/1_13_0/m9x_drv.so` | `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273` |
| `MSerX/MxMM.sys` | `bf743961b4846e90f32cbdf868d69240d7c403fcbd0a8d9cd6d8783ef9bd62c6` |

The Linux X image retains the diagnostic names `HLInit_Construct`,
`HLInit_InitializeHardware`, `HLInit_DetectBoardInfo`,
`HLInit_InitializeHardware_Mem_initialized`, `HLPInS80_GetBoardInfo`,
`PhxInit_InitializeMemoryController`, its `_Initialized` and `_Problem`
messages, and `PinS80_CommInitDeviceSequence` [BIN].  The joins below do not
depend on names alone; the evidence script pins every relevant body.

## 2. The kernel side is transport, not POST

`m9x_probe()` in `m9x_base.c` calls `pci_enable_device()`, disables legacy VGA
decoding, allocates/reuses the software device, and registers it.  It does not
request bus mastering or write memory/clock registers [SRC].

The first open eventually enters `m9x_dev_init()` [SRC]:

1. record BAR0 as the framebuffer aperture and add write combining;
2. choose the MMIO resource as slot 1, or slot 2 when BAR0 is 64-bit, and map it;
3. move and enable the PCI ROM BAR at the framebuffer base, map 128 KiB, and
   copy it byte for byte;
4. allocate the kernel context bitmap and context;
5. pass BAR, MMIO, and the copied ROM to `core->connect()`;
6. disable the PCI ROM BAR;
7. only after a successful connect, install the host IRQ and invoke
   `core->start_interrupt()`.

The `.rela.data` entry at driver-table offset `+0x20` identifies connect as
`m9x616` at `.text+0x5280`.  It constructs software objects through `m9x320`
at `0x4c50`; `m9x320` calls `m9x798` to copy resource information and parse the
ROM PInS memory-size field, then calls `m9x1117` at `0xc540`.  `m9x1117` is only
a prologue, `eax=1`, and return.  No function in this connect chain performs
the system-clock or `0xf00..0xf60` memory-controller sequence [BIN].

Therefore loading/opening the kernel module is not evidence that an un-POSTed
M9 has been initialized.  It also explains why the kernel layer can work on a
normally booted card: it consumes resources and state established elsewhere.

## 3. Board discovery and PInS do not require executing VBIOS

The X driver's board construction reaches `m9x19036` at `0x2beb40` from
`0x2b07d2`.  It passes the board's PInS destination at `board+0x2ec` to
`m9x21339` at `0x2c6190` [BIN].  That routine does the following:

1. constructs communication mode zero with `m9x3203`;
2. walks option-ROM images, validating `PCIR` and their chained image lengths;
3. reads the two-byte PInS near pointer at serial-flash offset `0x7ffc`;
4. reads the word at that pointer and requires signature `0x412e`;
5. reads length at `PInS+2` and version at `PInS+4`;
6. copies exactly `length` bytes to `board+0x2ec`, then closes the communication
   object.

Communication mode zero enters `m9x1755`; its byte-command/read sequence probes
recognized serial EEPROM/flash IDs and builds a bounded device descriptor.
`m9x7200` dispatches every read through that object's read method.  This joins
the previously inferred SPI/serial-flash PInS store to the Linux hardware path
[BIN, DER].  It is distinct from the kernel wrapper's temporary PCI ROM mapping.

The X ABI glue also maps the legacy shadow ROM at physical `0xc0000`, validates
`PCIR`/`102b:0540`, and can log `BIOS was not run?`.  That check and later
`xf86InitInt10`/VBE support are not called by the Phoenix memory-controller
sequence described below [BIN].  The cold memory path therefore does not ask
VBIOS to program DRAM.  It does still require a readable board PInS image;
without one, board-specific clocks, memory topology, and register patches are
not authoritative.

## 4. Exact warm/cold gate

`m9x25201` at `0x2b5650` is the serialized enable-hardware wrapper [BIN].  Under
the board lock it prepares configuration with `m9x17687(0)`, calls the state
getter `m9x2348`, and invokes `m9x21171` at `0x2c8190` only when that state is
zero.  A nonzero state skips all cold programming and continues to common
finish/unlock code.

`m9x21171` is `HLInit_InitializeHardware`.  For the ordinary path it attempts
this sequence up to **2000 times**, stopping when memory verification succeeds:

```text
m9x4358()             -> Phoenix m9x15968: reset/configuration preamble
m9x7139(dev, 4)       -> Phoenix display/VGA disable/reset path
m9x15067()            -> Phoenix m9x9441: clock holds and output disable
m9x2399()             -> Phoenix m9x2800: interrupt masks/acks and common state
m9x31786()            -> Phoenix m9x4312: memory-controller initialization
m9x16655()            -> no Phoenix 0540 operation; used by the 2539 branch
```

The `0x0540` dispatch in `m9x31786` goes directly to `m9x4312`; this join is
the proof that the code is the M9 path rather than the P690 sibling [BIN].

After the loop, the wrapper initializes output/communication state for indices
0 through 3 through paired `m9x6420`/`m9x6871` calls, performs device detection
through `m9x3938`, and iterates the constructed head records through
`m9x27366` [BIN].  This is display-service handoff, not a modeset and not a 3D
requester arm.

### 4.1 Reset/configuration preamble: `m9x15968`

The exact order is [BIN]:

1. save MMIO `0x0e4c`, `0x0e50`, and 64 PCI configuration dwords covering
   `0x00..0xfc`;
2. read MMIO `0x0358`, clear bits 17 and 18 (`byte2 &= 0xf9`), set bit 0, write;
3. restore the saved PCI configuration dwords, except special offsets
   `0x04/0x44/0x48`, then restore PCI command and MMIO `0x0e4c/0x0e50`;
4. in the retained `0x0358` value set bits 16..18 and clear bit 0, then write;
5. wait 5000 microseconds;
6. read `0x0318`, set bits 28 and 29, and write it back.

The binary does not name the individual `0x0358` bits.  Treat them as an atomic
reset/configuration protocol; do not substitute inferred reset names for its
read/modify/write order.

### 4.2 Clock holds and blank outputs: `m9x9441`

This routine saves the four controller clock selects and global video control,
then writes [BIN]:

```text
0x1c00 |= 0x02000001
0x1c04 |= 0x00000001
0x1c08 |= 0x02000001
0x1c0c |= 0x00000001
0x1c24 |= 0x00000101
delay 10 us
0x1c10  = 0x00000002
0x1c80  = 0x01001400
0x1c84  = 0x01001400
0x1c68  = PInS-patched read/modify/write, if a patch exists
0x1c9c  = 0x00040000
0x1ca0  = 0x00040000
0x1ca4  = 0x00040000
0x1ca8  = 0x00040000
```

This places the display block in native state with DAC and digital data paths
disabled while memory changes.  It does not restore active scanout.  The normal
output setup after the cold loop applies connector mappings and mode-specific
PInS patches.

### 4.3 Interrupt/common initialization: `m9x2800`

The exact initial writes are [BIN]:

```text
0x08fc = 0x00000000       high-bank interrupt mask
0x08f8 = 0x032f7f1f       selective W1C acknowledge
0x08dc = 0x00000000       other interrupt-bank mask
0x03a8 = 0x00020a05
```

For board identifier `0x1ca8:0000`, it also reads `0x1c4c`, sets bit 13, clears
bit 3, and writes it back.  The routine masks interrupt generation; it does not
enable the host IRQ.  Host IRQ installation and `start_interrupt()` happen in
the kernel wrapper after connect, while individual source enable and requester
completion setup occur in later services.

## 5. Memory-controller transaction

`m9x4312` at `0x2f5e20` is the complete outer transaction [BIN]:

1. save `0x1c00/04/08/0c` and `0x1800/1a00/1980/1b80`;
2. set bit 0 in each clock-select register;
3. clear bit 0 in each display-controller register;
4. wait 1000 microseconds;
5. program direct MC defaults/patches with `m9x9754`;
6. select indirect register `0xfe` and write control zero with `m9x2466(0)`;
7. initialize system clocks with `m9x13609`;
8. initialize the MC with `m9x21748`;
9. verify memory with `m9x24643`;
10. emit `_Initialized` or `_Problem` trace;
11. restore all eight saved display/clock wrapper registers in original order.

The return value is the memory-verification result.  The restore happens on
both success and failure.  It does not roll back the `0x0fxx` controller state,
so a retry starts from the just-programmed MC state.

### 5.1 Direct defaults and PInS overrides

`m9x9754` applies the following fallback values through `m9x29492` [BIN]:

| register | fallback |
|---:|---:|
| `0x0f3c` | `0x0e0f0420` |
| `0x0f40` | `0x081f060c` |
| `0x0f44` | `0x0d080c18` |
| `0x0f48` | `0x071f0408` |
| `0x0f4c` | `0x091f0904` |
| `0x0f54` | `0x00000040` |
| `0x0f58` | `0x32000000` |
| `0x0f5c` | `0x00000000` |
| `0x0f60` | `0x00000000` |

`m9x29492` first asks the PInS patch-table lookup for the same register.  When a
patch exists it applies its data/mask; otherwise it writes the fallback.  These
are safe defaults only within that vendor patch policy, not replacements for
the board PInS table.

### 5.2 System synthesizers: `m9x13609`

The system-clock routine writes oscillator control `0x0e24` from cached board
state and waits 100 microseconds.  It saves `0x0e00..0x0e20`, puts each of the
nine clock selectors in hold, writes `0x00020000` to all nine, and waits 1000
microseconds [BIN].

It compares the desired system clock assignments at board offsets
`+0x408..+0x428` (`PInS+0x11c..+0x13c`). `m9x15901` (`2d1660`) tests
whether the nine desired selectors use synthesizers 0..4; `m9x2741`
(`2d1910`) is another pure source-compatibility predicate for the later
selection checks. It does not program a synthesizer. The admitted programming
branches call `m9x12171` with the PInS target records. After 100 microseconds
it restores the nine selectors with their enable/hold bits and waits another
100 microseconds [BIN]. For an original-instruction execution on actual
SPI-verified M9128 inputs, see the [bounded clock proof](parhelia-m9-faa-cold-mc-writes.md).

The divider format and frequency equation already recovered for this family are:

```text
word = (b0-1) | ((b1-1)<<6) | ((b2-1)<<11) | ((b3-1)<<16)
program word|BIT(31)|BIT(21), then word, wait, poll BIT(24)
Fout = 27000 kHz * b0 * b1 / b2 / (2*b3)
```

This is system-clock setup during cold initialization.  Per-mode pixel PLL
selection and the output clock-source mapping remain part of atomic modesetting.

### 5.3 MC register image and enable pulses: `m9x21748`

The direct patch-aware fallbacks are [BIN]:

```text
f2c=02c8e8a7  f30=23a63474  f34=026655a3
f38=0000002b  f04=0440803e
```

For indirect registers, `m9x29492(..., class=1)` looks up patch key
`index|0x8000` and accesses the selected register through `0x0f00/0x0f18/0x0f1c`.
The fallback image is [BIN]:

```text
ff=00000001  00=00000000  02=0000005d
07=00000000  08=00000000  09=00000000  0a=00000000  0b=00000000
11=00000000  12=00000249  13=00000249  16=00000055  17=00000055
1a=ffffffff  1b=ffffffff  1c=ffffffff  1d=ffffffff
23=00000249  24=00000249  25=00090000
26=dd22ee11  27=7788bb44  2a=00000000
```

It then selects indirect control one, forces `0x0f0c` bit 21, and constructs
`0x0f14` from a PInS record or fallback `0x00d00006`.  The packing is the low
15 bits plus source byte bits `2:0 -> 18:16`, `5:3 -> 21:19`, bit 6 -> 22,
and bit 7 -> 23; bit 0 is cleared.  The enable protocol is [BIN]:

1. write packed `0x0f14` with bit 25 set; delay 10 microseconds;
2. write `0x0f10=1`;
3. write `0x0f14` with bit 24 set;
4. write `0x0f10=0x00010000`;
5. write `0x0f14`, then clear bit 24 and write again; delay 10 microseconds;
6. apply the `0x0f10` patch or fallback `0x000d7200`, retain bits
   `0x007fff00`, clear bit 16, and write;
7. pulse `0x0f14` bit 28.

Board flag `PInS+0x46`/cached `board+0x332` bit 0 selects the tail.  When set,
the routine delays 10 microseconds, applies `0x0f08` fallback `0x04a00000`,
forces `0x0f0c` bit 21, and delays again.  When clear, it first sets MMIO
`0x0000` bit 30 and reapplies indirect `0x12/0x13/0x16/0x17`, then converges on
the same tail [BIN].

### 5.4 What the vendor calls memory initialization

`m9x24643` reads `0x0f28`.  `0xffffffff` returns success immediately; otherwise
initial success is `((value >> 16) & 0x11) == 1` [BIN].  It then sets:

```text
0x0004 = 0x00000000
0x0008 = 0xf0000000
0x000c = 0x00010001
0x0000 = 0xa0000000
```

PInS communication selector 16 can run a board hook before verification and
selector 17 can run one afterward.  With no selector-16 hook and the status
test passing, the driver destructively verifies almost 512 KiB of aperture
memory (the last byte touched is at `0x7fffb`):

* dwords `0x0000..0xfffe` receive their index;
* dwords `0x10000..0x1fffe` receive `index<<16`;
* both ranges are read back and compared.

Any mismatch clears success, subject to the vendor assertion/error policy.
There is no adaptive timing search or observed training sweep in this body.
The accurate description is **PInS-patched MC programming followed by a
destructive VRAM self-test**.

## 6. Failure behavior and a requirement for the new driver

If the self-test fails, `m9x4312` emits trace `0x5104`, restores the eight outer
display/clock registers, and returns false.  `m9x21171` repeats the entire reset,
blank, interrupt, PLL, MC, and verification sequence.  After attempt 2000 it
emits `HLInit_InitializeHardware` error trace `0x2202` [BIN].

It then nevertheless writes one to the global initialized-state bucket before
returning [BIN].  This is a concrete vendor failure-handling defect.  A new DRM
driver must retain a failed state, keep outputs and requester disabled, return
the error to probe/resume, and require an explicit full retry.  It must also
bound each PLL poll and the overall memory retry count; blindly copying 2000
destructive attempts is unsuitable.

## 7. Windows evidence and its static limit

`MxMM.sys` `DriverEntry` installs `FindAdapter=0x55ebd` and
`HwInitialize=0x56304`.  The new-device path reaches helper `0x568e7`, which
constructs the low-level OS/hardware function table through `0x17dc4` [BIN].
Table slot `+0x88` is `0x179a2`.

The callback at `0x179a2` performs this exact global PLL procedure [BIN]:

1. set `0x03c0` bit 31;
2. save interrupt masks `0x08c8` and `0x08dc`, then write both masks zero;
3. acknowledge `0x08d8` and `0x08c4` with `0xffffffff`;
4. set `0x03c0` bits `0x00110000`;
5. poll `0x03c4` bit 31, at most 1000 iterations with 1000-microsecond delays;
6. on success, set `0x03c0` bit 31, delay 15000 microseconds, set bit 21 and
   bit 31, delay another 15000 microseconds, then clear `0x00110000`;
7. restore the two saved interrupt masks.

On poll timeout it exits without restoring those masks or clearing the control
bits [BIN].  The adjacent Windows diagnostics are `Test PLL lock`, `Program
PLL`, and `Hardware init`, which identifies this as clock/global hardware setup,
not a 3D-engine reset [BIN].

The function is called indirectly through the constructed table.  Static direct
xrefs therefore establish table publication and the miniport lifecycle that
owns it, but not the exact branch which invokes it or whether a particular
Windows boot reaches it before or after VBIOS POST.  No stronger Windows-only
claim is justified without tracing indirect calls.

## 8. Boundary with requester, units, and display modes

The cold path initializes enough common state for later services, but it does
not publish or ring requester addresses `0x0400/0x0404` or
`0x0500..0x0524` [BIN].  The requester, writeback fence, 3D context image, and
individual interrupt-source enables belong to the kernel/core and GL service
lifetime documented in `parhelia-m9-dma.md`, `parhelia-m9-3d-static-re.md`, and
`parhelia-m9-context-lifecycle-re.md`.

Likewise, `m9x9441` only makes the VGA/native transition safe and blanks every
physical output.  EDID, pixel PLL choice, CRTC timing, PInS output companion
patches, external transmitter reset/training, and final unblank happen later as
described in `parhelia-m9.md`.  A from-scratch driver should keep these phases
separate:

```text
PCI D0 + BAR access
  -> direct PInS acquisition and validation
  -> bounded cold reset/system-clock/MC/self-test
  -> interrupt controller masked and acknowledged
  -> DRM memory/requester/context construction
  -> IRQ source enable
  -> atomic display programming and output unblank
```

## 9. Remaining irreducible evidence gaps

The saved binaries now provide an implementable cold MC sequence, but three
items still require the exact board ROM or a controlled reset capture before a
new driver can claim universal cold boot:

1. **The target board's PInS bytes.**  Defaults are patched by board data for
   clocks, MC, and output electrical settings.  A generic `0540` image cannot
   safely replace the exact subsystem's table.
2. **The lowest serial-flash transaction layer.**  The Linux binary proves the
   PInS acquisition route and supported flash probes, but its GPIO/MMIO-level
   SPI transfer has not been reduced here into a standalone clean-room routine.
   A validated PCI-ROM/firmware copy is a safe first implementation fallback.
3. **A truly un-POSTed observation.**  Static code proves that the vendor can
   program PLL/MC without invoking VBIOS, but cannot prove power-rail, strap, or
   board-microcontroller state before the first accessible MMIO read.  Capture
   from D3cold/secondary-device power-up or a saved cold ROM trace is needed to
   validate those preconditions.

These gaps do not block a first driver on firmware-POSTed hardware.  They do
block claiming recovery from every electrically cold or corrupt-clock state.
