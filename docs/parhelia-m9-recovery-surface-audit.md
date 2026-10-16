# M9/Phoenix recovery-surface audit

This note closes a narrow static-analysis question: whether the saved M9 Linux
core or Windows miniport contains a requester/3D timeout recovery or global
engine reset outside the already-audited interrupt paths.  Addresses below are
virtual addresses in `MxMM.sys` and `.text` offsets in
`KernelLibraries.o_shipped`.  `[BIN]` is a direct binary observation and
`[DER]` is a conclusion from joined observations.

The Windows `MSerX.inf` lists twelve M9120/M9138/M9140/M9148-era subsystem
variants, all under PCI device `102b:0540`.  Both install sections register the
same `MxMM` service and copy the same miniport/ICD family [SRC].  There is no
per-board reset binary to mine: the board names select setup/output support
around one shared recovery surface [DER].

## Windows miniport callback surface

`DriverEntry` at `0x55e05` builds the 0x54-byte legacy
`VIDEO_HW_INITIALIZATION_DATA` used by `VideoPortInitialize` [BIN].  The
function pointers are:

| structure offset | entry point | role |
|---:|---:|---|
| `+0x08` | `0x55ebd` | find adapter |
| `+0x0c` | `0x56304` | initialize |
| `+0x10` | `0x10875` | interrupt |
| `+0x14` | `0x58805` | StartIO |
| `+0x20` | `0x10505` | ResetHw |
| `+0x24` | `0x5676f` | child descriptor |
| `+0x28` | `0x57690` | get power state |
| `+0x2c` | `0x576e7` | set power state |
| `+0x30` | `0x588ad` | query interface |

The value at `+0x18` is the `0xc828` device-extension size, not another
callback.  The image imports `VIDEOPRT.SYS`/`VideoPortInitialize` and contains
no `Dxgk` import or string.  It is an XPDM miniport and exposes no WDDM
timeout-reset callback [BIN].

### `HwResetHw` is a display reset

`HwResetHw` at `0x10505` first checks device-ready bytes `+0x76/+0x77`, calls
`0x124d8`, and updates display-state bytes `+0x78/+0x79` [BIN].  Its remaining
calls are exactly:

```text
0x11b73  obtain/check device+4 -> display-manager +0x1ec
0x11bb2  read a display descriptor
0x11878  update display-manager state
0x11bb2  read the descriptor again
0x118d6  re-establish each of four head/output records
```

The descriptor test at `0x1058b` accepts upper word `0x1ccb`, and the loop at
`0x10597` visits heads 0 through 3.  Every hardware-bearing callee is rooted in
the display-manager object at `device + 4 -> +0x1ec`; the routine neither
references the requester helpers nor contains a `0x400..0x524` register
operand.  It always returns false at `0x105b3..0x105bc` [BIN].  This is the
legacy text-mode/display restoration hook, not a successful 3D reset [DER].

### Power transitions re-create board/display services

`HwSetPowerState` at `0x576e7` validates the requested state, serializes it,
and dispatches through `0x577b8`.  Monitor IDs go to `0x57818`, which iterates
the head list and calls output-power method `0x10ffe`.  Global device ID
`0xffffffff` goes to `0x578f4` [BIN].

For states other than 1, `0x578f4` performs the following ordered work after
its client-state checks [BIN]:

```text
0x69eb5; 0x11073; each live head -> 0x10ffe(0);
0x60f47; optional 0x6bfc2/0x6c035; 0x6a0d3(0x10);
0x68206; release callback/event state; 0x10f96
```

For state 1 it updates PCI/config state through `0x18dfe`, invokes the board
service constructor `0x6c058`, calls `0x60f47`, and publishes the new state
through `0x1254b` [BIN].  `0x6c058` reconstructs table-driven board services
through `0x68259`, `0x6bccb`, `0x6a7cc`, `0x6a5b9`, `0x6a442`, `0x686f6`,
`0x6c20d`, `0x6c2e1`, and `0x6a0d3`.

An instruction-level direct-edge closure from `0x578f4` covers 9,319 decoded
instructions and 605 direct calls.  It reaches none of requester quiescence
`0x146ee`, arm `0x224c8`, post `0x224f3`, or completion/kick `0x22533`, and
contains no literal requester-register operand in `0x400..0x524` [BIN].  The
closure has 25 indirect call/jump sites, including configuration accessors and
object callbacks, so this does not prove that a table-driven service never
touches requester state.  It does prove that the saved power path does not
publish a callable requester restart sequence [DER].  Treating a display power
cycle as render recovery would also destroy unrelated output state.

### StartIO and escape surface

`HwStartIO` at `0x58805` serializes a request and calls dispatcher `0x6c755`.
That dispatcher recognizes these request-number ranges [BIN]:

```text
0x230000..0x231ffc  -> 0x7014b
0x232000..0x23207c  -> 0x708de
0x232080..0x2320fc  -> 0x70a8a
0x232100..0x23217c  -> 0x70eab
0x232180..0x2321fc  -> special 0x71618 for 0x2321bc
0x232280..0x2322fc  -> 0x7165f
0x232300..0x23237c  -> 0x5d927
0x232400..0x23247c  -> 0x6adea
0x233000..0x233ffc  -> 0x7190e
```

All other requests are rejected at `0x6c840`.  The direct-edge closure of all
these handlers covers 65,200 decoded instructions and 4,447 direct calls.  It
contains no call to the four requester helpers above [BIN].  There are 202
indirect dispatch sites, so an object method can still submit normal work; the
defensible conclusion is that no escape supplies a statically recoverable
reset contract, not that StartIO can never reach requester work [DER].

The complete PE direct-call references to the four low-level requester helpers
are pinned by `tools/m9-recovery-surface-evidence.py`.  All sites are in the
`0x14540..0x23e2b` requester/context service family; none is in ResetHw, power,
StartIO, or their direct dispatcher functions [BIN].

## ICD error recovery restores resources, not the engine

The saved Windows and Linux OpenGL drivers retain C++ method names for
`WGLICDLink::CallDriverErrorRecovery`,
`WGLICDLink::SendCommandStreamErrorRecovery`, and `WGLICDLink::Restore`.
These names make it possible to inspect the vendor's highest-level recovery
policy without guessing from an IRQ bit [BIN].  The implementations diverge
sharply between the two builds.

In `MxMGLX.dll`, `SendCommandStreamErrorRecovery` is the routine at
`0x1002e560` [BIN].  Its only direct caller is the command-stream flush at
`0x10003280`.  The caller initializes a retry byte to zero, invokes the helper
after a nonzero send result, and resubmits only if the helper changes that byte
[BIN].  The helper handles statuses as follows:

| send status | action | return/retry |
|---:|---|---|
| `0x0a`, `0x0b` | call `Restore` at `0x1002dfe0` until true, sleeping 10 ms between attempts | return `0x57`; retry byte remains false |
| `0x3f` | call context-side method `0x100a2460` on `context+0x121c` with argument zero | return zero; set retry byte true |
| other | no recovery action | return zero, except a null context returns one |

`CallDriverErrorRecovery` maps to `0x10030b50`; its sole direct caller is
`CallDriver` at `0x10030fa3` [BIN].  It accepts status 2 or `0x0a..0x0c`, unless
recovery is already active at `WGLICDLink+0x25`.  It then calls the same
`Restore` routine until true, sleeping 100 ms between attempts, clears the
caller's retry byte, and returns `0x57`.  Other statuses return one.  This loop
has no deadline [BIN].

The complete `Restore` body is `0x1002dfe0..0x1002e313`.  It guards against
re-entry, walks monitor/context/resource state, tears down and re-creates
surfaces, and updates software bookkeeping [BIN].  Its direct calls include
monitor/resource helpers, but not the imported `ExtEscape` thunk at
`0x1072801c`.  The two error helpers themselves call only `Restore`, the
context-side method for status `0x3f`, and imported `Sleep`; neither sends a
dedicated recovery escape [BIN].  Ordinary reconstruction helpers can still
submit normal miniport requests through indirect or deeper calls, so this does
not establish that restoration is kernel-independent.  It establishes that
the Windows ICD publishes resource reconstruction and retry as its recovery
contract, not a 3D engine reset [DER].

The Linux `m9x_drv.so` build closes the boundary more strongly.  Its named
methods are unconditional assertions against the literal expression `false`
in `WGLICDLinkLinux.cpp` [BIN]:

| method | entry | assertion line |
|---|---:|---:|
| `CallDriverErrorRecovery` | `m9x21649`, `0x5ce180` | `0x8bf` |
| `SendCommandStreamErrorRecovery` | `m9x11978`, `0x5ce360` | `0x648` |
| `Restore` | `m9x2254`, `0x5ce390` | `0x620` |

This is not only dead diagnostic material.  Command-stream function `m9x1135`
calls sender `m9x7606` at `0x5ba255`; every nonzero result reaches
`SendCommandStreamErrorRecovery` at `0x5ba279`, which asserts [BIN].  The sole
direct call to `Restore` is behind `m9x17456`, whose entire implementation is
`xor eax,eax; ret`; that guard therefore reports false before the Restore loop
can run.  `CallDriverErrorRecovery` has no direct caller in `.text` [BIN].

The package contains sixteen `m9x_drv.so` siblings for X-server ABIs 6.9,
7.1--7.5.99, and 1.9--1.15.  Their ABI-facing code shifts the linked addresses,
but every image contains the same three signature-referenced assertion bodies,
with the same source file, literal `false`, and source lines `0x8bf`, `0x648`,
and `0x620` [BIN].  The checker verifies the complete sibling set and each
image hash.  There is therefore no alternate packaged Linux ABI module with
an operational recovery implementation [DER].

Consequently, the saved Linux OpenGL stack has no operational user-mode
recovery path.  A command-send failure is fail-stop, while the Windows path
rebuilds resources and may retry without revealing a hardware reset sequence
[DER].  Neither implementation supplies a safe reset primitive for a new DRM
driver.

## Linux close, removal, and disable paths

The open wrapper has no power-management lifecycle to inspect: its
`pci_driver` contains only `.probe` and `.remove`, and `m9x_core_driver_t`
exports only init/cleanup, enable/disable, connect/disconnect, open/close,
interrupt, and ioctl entries [SRC].  There is no suspend, resume, reset,
timeout, watchdog, or recovery callback.

The exported core table in `.rela.data` confirms the same 13 operations [BIN].
In particular, `disable` wrapper `m9x302` calls `m9x449` at `0x3f30`, whose
entire body is only `push rbp; mov rbp,rsp; leave; ret` [BIN].  Hot removal with
open files only stops interrupts, invalidates the device handle, and wakes
clients.  With no open file it calls this empty core disable wrapper [SRC/BIN].

Ordinary close is teardown rather than recovery [SRC/BIN]:

```text
m9x_release -> m9x_disconnect -> m9x_ctx_cleanup -> core close m9x965
  -> m9x717 -> m9x1210 -> m9x106 -> m9x211
```

The requester's destruction stage is `m9x504` at `0x2b770`.  If active, it
sets object `+0xd8 = 1`, clears `+0xd4`, and calls `m9x1016(+0xb0, 1)`.  It does
not test a result; `m9x1016` returns no success value and its stable
`0x508/0x500/0x50c` polling path has no count or time deadline.  The destructor
then invokes the two child teardown methods and clears requester objects
`+0xc0/+0xc8/+0xd0` [BIN].  It never restores the descriptor, posts `0x404`, or
submits a validating fence.  This is an unbounded quiesce-before-free contract,
not hang recovery [DER].

## Strongest static conclusion and bounded recovery

No saved production entry point provides a 3D/global reset contract [DER].
The Windows callback explicitly named ResetHw is display-only and reports
failure; Windows power resume reconstructs board/display services without a
direct requester restart; its complete StartIO direct graph exposes no reset
join.  Windows ICD recovery reconstructs resources but issues no dedicated
reset escape.  Linux has no PM/reset callback, its ICD recovery method asserts,
its core disable operation is empty, and close performs an unbounded requester
drain followed by destruction.  These facts add lifecycle, user-mode recovery,
and escape coverage to the earlier exhaustive immediate MMIO-write audit.
They do not prove that the silicon lacks an undocumented reset bit or that no
indirect/table-driven path ever touches a requester word.

A new DRM driver's safest bounded contract is therefore:

1. Stop admission, serialize recovery, mark the current job/fence generation
   failed, and snapshot requester, front-end, interrupt, and fence state before
   acknowledgement or reprogramming.
2. Attempt the proven stable requester quiescence test under an absolute
   deadline.  Abort immediately on removal (`0x8f0 == 0xffffffff`).
3. Only after quiescence, make one requester-local park/restore attempt using
   the recovered `m9x714` descriptor order, restore address windows, kick
   `0x314`, and require a newly seeded no-op fence to advance.
4. If either quiescence or the validation fence fails, keep rendering wedged
   and preserve scanout.  Do not call ResetHw, cycle display power, treat
   `0x31c` as reset, or invent a PCI-function reset.
5. Do not free memory still reachable by a non-quiescent requester.  Terminal
   teardown must first prevent further host DMA (including clearing PCI bus
   mastering when the function remains present), then quarantine outstanding
   objects; the device must not resume without a proven reinitialization path.

The smallest experiment which could strengthen this result is a single
bounded no-op-fence hang injection with pre/post snapshots of `0x8f0`,
`0x31c`, `0x400/0x404/0x448`, `0x500..0x524`, all IRQ status/masks, and fence
memory.  Run exactly one known requester-local restore and record whether a new
fence advances while scanout remains stable.  Failure establishes the point at
which the driver must stay failed closed; success validates requester recovery
only, not a global 3D reset.
