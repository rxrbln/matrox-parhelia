# M9128 mode notification boundary

The examined normal miniport notification publishes CPU event bits and CPU
snapshots. Its proved consumer returns those bits to an OS caller. This is
not a proved omitted FAA hardware enable or shader/context upload. The
source-specific hardware admission question remains open.

All PCs refer to MSerX `MxMM.sys`, SHA256
`bf743961b4846e90f32cbdf868d69240d7c403fcbd0a8d9cd6d8783ef9bd62c6`.
The source checker executes original producer `5d812` and consumer `74e3e`
for all 16 low-four-bit event combinations on explicit synthetic CPU ABI
cells. It uses the actual handle-zero branch. No OS event call, hardware
read, GPU reply, real device constructor, or rendering success is supplied.

## Producer and actual consumer

`HwInitialize` calls `5d812` at `56575`, with the live miniport pointer and
event mask four. `5d812` requires a nonzero caller owner and nonzero low
four mask bits, then ORs the mask into CPU global `7dc64`. It signals the
retained handle at `7dc68` only when that handle is nonzero. The actual
`248ee` wrapper's import at `45b38` is `ntoskrnl.exe!KeSetEvent`; its three
arguments are the caller's event, zero increment, and false wait.

Request handler `74e3e` validates its caller pointers, takes low four bits
of `7dc64` at `74e54`, clears the entire CPU event word at `74e5c`, and writes
the returned mask to caller output `+14` at `74e82`. It also writes zero
to the caller status DWORD. This bounded producer/consumer pair has no
MMIO, command-queue or shader-owner access. Any later OS consumer action
must be joined separately; an event bit cannot be reinterpreted as a GPU
enable flag.

## Separate snapshot and mode-commit notification

The previously examined operation-record commit `2536b` calls `2a179(2)`,
which calls `1b14d(2)` when the CPU error field is zero. That argument
increments profile `+1e4`. `1b14d` calls `5d812` only for argument one,
not for this argument-two mode commit. Its conditional `5cd77` snapshot
publication is a different path, gated by the retained global owners and
miniport notification fields.

At `5cd9d`, `5cd77` puts immediate `6be1c` into EDI and passes it to `6cd0b`.
This number is an allocation byte count. `6cd0b` adds `fff`, aligns down to
a 4 KiB multiple, records the rounded length, allocates that many bytes,
and clears the backing before publishing it in a CPU owner list. The fact
that `6be1c` also names an instruction inside the image does not make this
argument a callback. It is not called by the allocation producer. The
snapshot producer later stores that length into its CPU snapshot header.

This closes the direct event and allocation semantics. It does not claim
that every later consumer of those published snapshots is CPU-only, nor
that the complete Windows mode transaction has executed in the ordinary
FAA capture. No native event, reset, clock, register or metadata patch is
derived from these notification records.

## Check

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-mode-notification.py
```

The checker pins the whole producer/consumer/wrapper/allocation spans,
verifies the actual import name, and guards against reaching an OS event
or hardware helper during its CPU fixture.
