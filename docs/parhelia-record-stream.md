# The region-2 record stream: grammar and item layouts

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

All 3D state reaches the engine through this stream. The command ring carries
only a draw packet and its fence; everything else -- matrices, lights, combiner
programs, texture state -- is written here as records that the kernel translates
into register writes and opcode-0x41 block loads.

Derived from the vendor ICD and HAL binaries and validated by exact consumption
across 39 captured streams. Where a field is inferred rather than read out of
the binary it is marked.

## Key 0x46 -- the combiner program object, 414 dwords

Parsed by **`mtx2132`** (`.text+0x6e1d0`); `mtx3577` (`.text+0x74160`) is only a
cache lookup over the 0x29a8-byte program object.

    item[0..1]     u64 program id / serial
                   (mtx558 compares id+1 against hwstate+0x12ef to skip
                    redundant uploads)
    item[2]        N = number of ACTIVE combiner blocks
    item[3..372]   10 x 37-dword combiner blocks; only the first N are walked
    item[373]      byte flag -> progobj+0x2a80; ==1 enables the texture-unit pass
    item[374]      texture unit count
    item[375..410] 4 x 9-dword texture-unit descriptors, stride 0x24
    item[411..413] not read on any traced path

The array is dimensioned exactly **10** because `4 + 10*0x94 = 0x5CC` is where
the next field begins -- an eleventh block would overlap it.

    6e387:  mov edx,DWORD PTR [rdx]      ; N = P[0]
    6e389:  add r13,0x4                  ; first block at P+4
    6e3ab:  add r13,0x94                 ; stride 148 bytes = 37 dwords
    6e3b5:  add DWORD PTR [rsp+0x18],0x2 ; hw combiner slot counter += 2 per block

### A block is a combiner-slot PAIR, not a texture stage

Each 37-dword block programs **two** hardware combiner slots, so ten blocks =
**20 combiner stages**, which is what the RGB/ALPHA instruction pairing in
`parhelia-fragment-isa.md` describes. The **four real texture units** are the
separate `0x24`-stride section at item[375..410].

    +0x00  dw 0..12   RGB half, 13 dwords            -> mtx1653(dst+0x158, ...)
    +0x34  dw 13      byte: alpha half separate?
                      if 0 the ALPHA half ALIASES the RGB half (cmove @6e402)
    +0x38  dw 14..26  ALPHA half, 13 dwords          -> mtx2619(dst+0x1c8, ...)
    +0x6c  dw 27      byte, only read when mode == 2
    +0x70  dw 28..31  reserved
    +0x80  dw 32      byte: constant present
    +0x84  dw 33..36  4 floats = stage constant

The constant is converted `x * 1024.0f`, `cvttss2si`, clamped to
`[-1024, +1023]` -- i.e. **Q1.10**, matching the constant encoding already
documented for the fragment ISA. Two independent derivations of the same format.

Observed across captures, which is what pins item[2] and the texture section:

| stream | id | N | item[374] | texdesc[0].dw0 |
|---|---|---|---|---|
| glxgears / clear_color (untextured) | 2 | 1 | 4 | 0 |
| tex_tri_nearest (`glEnable(GL_TEXTURE_2D)`) | 3 | 1 | 4 | **1** |
| matrix_rotate | 5 | 1 | 4 | 0 |

## Key 0x21 -- lights

`mtx227` (`.text+0x46e20`): **op 0 = enable, op 1 = disable, op 2 = set
parameters.** Only op 2 carries a payload (`46f68: add r15d,0x68` = 104 bytes =
26 dwords; item advance 0x70).

There are **12 light slots**, not 8 (`46f92: cmp ecx,0xb; jg` over
`hwstate+0x1c20`), with the active count in `hwstate+0x1be0` bits 14..17.

Proven fields of the 26-dword payload, from `mtx1770` (`.text+0x1b9b0`):

    dw0        light type: 2 = spot, 3 = directional
    dw13..15   direction
    dw16..18   position
    dw25       spot cutoff, in DEGREES as written by the client

The interior float groups (dw1..4, dw5..8, dw9..12) and dw19..21 / dw22..24 are
only `memcmp`/`memcpy`'d wholesale by the kernel. Labelling them diffuse /
specular / ambient / spot-direction is **inference from the GL defaults visible
in the capture, not read from the binary.**

### The kernel writes back INTO region 2

This matters for any replay tool and for the open kernel driver:

* for **type 3**, the kernel copies dw13..15 into dw16..18 **in place**;
* for **type 2**, it rewrites dw25 to `cosf(x * PI/180)` **in place**
  (`mtx3769` is a bare `fcos` wrapper).

So **region 2 is not read-only to the kernel.** An earlier reading of this --
that position appears twice because the client writes both an object-space and
an eye-space copy -- was wrong: the client writes it once and the kernel
duplicates it.

Note that no dword is `K`-flagged inside a light record in the current captures,
but that is *not* evidence against the write-back: every light in them is type 3,
so the cutoff conversion never runs, and both position copies were already equal
so the duplication changed nothing.

## Key 0x0c -- flat state ids

The id is a **flat 32-bit value**, with no 16-bit split anywhere in `mtx2617`'s
4758-byte body. State lives at `hwstate[0x10 + id*4]`, with a 256-bit override
bitmap at `hwstate+0x414`, and an explicit `cmp esi,0xab` feeding a **172-entry
jump table** at `.rodata+0xba0` -- matching the observed id range 0x00..0xAC.

### A decoder self-test, from a latent vendor bug

The bitmap probe indexes `id - 1` (`199c8: lea ecx,[rsi-0x1]`), so a literal
**id 0 would read ~512 MB past `hwstate`** in the vendor driver. Real streams
never contain id 0 -- ids start at 0x07. So **a decoder that starts emitting
key-0x0c id 0 is almost certainly mis-slicing the tail of a preceding
variable-length record.** Cheap to check and worth keeping permanently.
