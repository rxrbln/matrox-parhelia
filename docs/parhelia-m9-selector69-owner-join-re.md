# M9 Windows external-selector-69 owner join

This note closes one narrow ownership question in the saved 32-bit Windows
driver: which resource object reaches draw-builder `+0x54`, and how that field
relates to the public final-state object at `ctx+0x2528`.  The evidence is
checked by `tools/m9-selector69-owner-join-evidence.py` against
`MxMD.dll` SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.

## The pointer identity

The device constructor calls `0x13a080` with `ecx = device+0x2b28` at
`0x10ff98..0x10ffa0`.  Inside that constructor, `0x13a0a3` forms
`builder+0x18`, and `0x13a0a6` stores it at `device+0x2b20`:

```text
10ff98  lea  ecx,[esi+0x2b28]
10ff9e  mov  edx,esi
10ffa0  call 0x13a080

13a0a3  lea  eax,[ecx+0x18]
13a0a6  mov  [edx+0x2b20],eax
```

Context publication at `0x102d48..0x102d54` copies `device+0x2b20` into
`ctx+0x2528`.  Therefore:

```text
ctx+0x2528 == device+0x2b20 == (device+0x2b28)+0x18
p2528+0x3c == draw_builder+0x54
```

This is an exact pointer/offset identity, not a field-name analogy.

## The owning resource and both setter call sites

The resource owner keeps the object used by this path at owner `+0x44`.
Both direct calls to setter `0x109380` load that field:

```text
11bd2a  mov  edx,[edi+0x44]
11bd2d  mov  eax,[edi+0x10]
11bd30  call 0x109380       ; owner/context rebind

11bf24  mov  edx,[ebx+0x44]
11bf27  mov  eax,[ebx+0x10]
11bf2a  call 0x109380       ; initial owner construction
```

The setter resolves `p2528 = [ctx+0x2528]`, compares and writes
`p2528+0x3c`, increments its serial at `+0x38`, and sets dirty bit `0x40000`
at `p2528+0x00` (`0x109380..0x1093b3`).  By the identity above, those are
draw-builder `+0x54`, `+0x50`, and `+0x18` respectively.

Owner `+0x3c` is a different object.  Rebind code passes it to `0x109340`,
which stores it at `ctx+0x2520+0x54`; it is not the pointer consumed from
draw-builder `+0x54`.  This distinction prevents the two adjacent owner
resources from being conflated.

## Draw-time consumption

The only direct caller of draw wrapper `0x13c2c0` is `0x110946`, which passes
`device+0x2b28`.  At `0x13c37c`, the wrapper tests builder `+0x18` bit
`0x40000`; the taken path is the only direct call to `0x13b1b0`, at
`0x13c385`.

`0x13b1b0` then reads builder `+0x54` at `0x13b1d2`.  If non-null, it calls
that object's vtable slot `+0x20` with type `5` and output pointer
`current_c04+0xa0` (`0x13b1d9..0x13b1ed`).  The existing Windows emission
evidence identifies this pair as the external selector-69 resource pair.

Confidence is high for the ownership and copy join: every pointer publication,
both setter call sites, the dirty gate, and the sole draw-time consumer are
direct instructions in the pinned binary.  The resource object's higher-level
API class name remains unknown; the evidence supports calling it the external
selector-69 resource object, not assigning a stronger source-language type.
