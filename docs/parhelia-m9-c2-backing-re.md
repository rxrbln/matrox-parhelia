# M9148 first-triangle C2 backing

This note closes the allocation and address contract for the common resource
selected through command index `c2`.  It also states the remaining content
boundary precisely.  The companion checker is
`tools/m9-c2-backing-evidence.py`.

The pinned inputs are the Linux `m9x_drv.so` from package build 98, Windows
`MxMD.dll` 2.10.00.105, and
`captures/m9x-linux-ff-tnl-20260914/raw-preserve.tgz`.  The checker verifies
their full SHA-256 hashes before interpreting them.

## Exact allocation contract

Allocation records 52 and 53 from process 5644 are two instances of the same
special resource request.  Ignoring the second request's opaque word at
`+0x1c8`, their public fields agree:

| Field | Value |
| --- | ---: |
| request kind `+0x08` | `3` |
| placement `+0x20` | `4`, local VRAM |
| geometry `+0x198/+0x19c` | `0x400 x 1` |
| requested usable extent `+0x1e8` | `0x1080` bytes |
| request alignment `+0x1f4` | `0x1000` |
| returned usable extent `+0x218` | `0x1080` bytes |

Record 52 returns `0x11f61000` in both base fields `+0x208/+0x210`.
Record 53 returns `0x11f63000`.  The exact resource payload is therefore
`0x1080` bytes, while two adjacent identical allocations demonstrate a
`0x2000` page-rounded reservation stride.  A clean-room allocation uses a
`0x1080` logical payload and `PAGE_ALIGN(0x1080) == 0x2000` physical storage.
The final `0x780` bytes are allocator padding; no saved evidence assigns them
resource contents.

The decoded first draw binds:

```text
preprocess: c2=11f61000 c3=00000004, draw 0..4
raster:     c2=11f61000 c3=00000000, draw 0..3
```

Thus `c2` is the returned allocation base itself.  It has no leading page,
byte displacement, or low-bit tag.  Both passes retain that same base; only
`c3` changes.

Placement-4 addresses occupy the M9148 local engine domain beginning at
`0x10000000`.  The resource is at local BAR byte offset:

```text
11f61000 - 10000000 = 01f61000
```

Both capture processes map the complete 256 MiB local aperture with mmap
offset `0xd0000000`, so the corresponding aperture location is mapping base
plus `0x01f61000` (physical BAR address `0xd1f61000` in this capture).  This
relation describes CPU reachability; it does not prove a CPU store occurred.

## Linux and Windows emitter identity

Linux `m9x29970` at `0x4ae470` takes its resource pair from state `+0x148`,
emits header `c2,c3,8f`, and calls the relocation callback twice:

```text
packet slot 2, usage 0x13
packet slot 3, usage 0x14
```

Windows routine `0xf5730` is the exact 32-bit twin.  Its pair is at state
`+0xc4/+0xc8`; `0xf57da..0xf581b` emits the same header and registers the same
two slots and usage IDs.  The alternate branches in both binaries emit the
literal form `c2=4,c3=0`.  These routines bind or select the resource.  They
contain no resource-content upload or clear.

The paired usages require the object to remain live across preprocessing and
raster.  Their narrow hardware read/write directions remain unidentified, so
the diagnostic should retain the complete logical object as read/write until
the job retires.

## Initialization and first-use boundary

The archive contains two related processes and they must not be merged into a
single event sequence:

* Process 5644 saved allocation records 52 and 53.  Its first later GPU submit,
  EXEC 57, contains only the literal `c2=4,c3=0` form and no command operand
  equal to `0x11f61000`.  It does not initialize the backing through C2.
* Process 5632 saved the exported draw.  Submits 53 and 54 are fence-only;
  submit 55 is the first saved two-pass draw and is byte-identical to
  `case-00-command/draw-{bm,execute}.bin`.

The capture wrapper saved 560-byte allocation descriptors, 32 KiB command
pages, 2112-byte EXEC descriptors, and 0x21000-byte mapped system-resource
images.  It saved no `0x1080`-byte C2 image and did not trap stores through
the globally mapped local aperture.  The saved data therefore cannot reveal
the bytes present when preprocessing first binds C2.  Static inspection of
the two emitters narrows the missing possibilities to an earlier CPU aperture
store or local allocator policy, but does not distinguish them.

Zeroing the logical `0x1080` bytes is a deterministic guarded-probe policy.
It is not vendor-identical initialization evidence.  If a zeroed object fails,
the next evidence step is a local-aperture snapshot immediately before and
after the first preprocessing launch.  Reinterpreting allocator residue or
the unsaved padding as a vendor image is unsupported.

## Clean-room implementation audit

The optional checker mode audits a kernel source file without changing it:

```sh
python3 tools/m9-c2-backing-evidence.py --self-test \
  --kernel-source ../src.linux.x86-64-desktop.260909.123559.3725172/\
linux-7.2/drivers/gpu/drm/m9x/m9_triangle.c
```

It requires a `0x1080` C2 payload, the existing `PAGE_ALIGN` allocation path,
direct `job->c2 = m9_triangle_bus(...)`, both range-0/range-1 C2 relocation
sites, and the known captured-operand self-test.  It rejects the former
leading-page model because that would allocate `0x3000` and bind `bus+0x1000`,
contradicting both allocation records and both command sites.
