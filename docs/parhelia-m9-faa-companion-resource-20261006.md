# M9 FAA companion resources and physical backing boundary

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0-only

This evidence uses genuine MSerX MxMD.dll with SHA256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
It is separate from P690/Parhelia. No physical allocation, enabled API FAA
admission, GPU execution, successful store write or antialiased image is claimed.
The retained artifact is `captures/m9-faa-companion-20261006/pool6-layout.json`.

## Pool6 is a distinct ordinary context resource

The normal context request is1280 bytes/pool6 through owner vtable slot44.
Real76540 constructs flags8002, profile56, final request-format0, width320,
height1. Config/profile/card registration execute genuine code. Real19560
creates the shared owner using separately retained M9128 cleanroom postinit
f04=0448803e, then stops before unproved platform98/a0/hardware10 descriptors.
Controlled ABI glue connects an actual owner constructor to those genuine CPU
owners; the device/cache slot is2 in this fixture, not hardware admission.

Execution proceeds through76540/a3870/51870/51990/45eb0/54660 and stops at546ed
before its actual dispatch to455c0 through absent device+6988. The real resource
has kind0, logical320x1, 32 stored bits and4 bytes per element. Layout is
format78, selector0, row span320, one row, row bytes320*4 and extent500 hex.
Its layout flags1 omit address-admission mask2; address+15c and backing+1e4 are
zero. No mapping or contents exist. Constructor host zeroes are not the initial
contents of a real physical buffer.

19560 feeds25a70 with platform+98, hardware+10 and its page-aligned extent from
platform+a0 (optionally limited by a genuine configuration getter). The resulting
owner would feed44b90 and become device+6988. The meanings/producers of those
three physical inputs must be joined, not replaced with BAR guesses. The present
proof deliberately stops before them and before the absent allocator dispatch.

Index24 uses this cached pool6 resource's selector2 address through getter kind5
and relocation2b. It is distinct from the main fragment store, the second buffer,
AA companion and20-byte FAA sentinel. The known dirty40 raw sequence is24 then
E6=6dc00000,1,1 thenBC/BD. Encoding that sequence does not establish this resource's
backing, contents or GPU lifetime. Parent's context save/restore investigation
owns the500-byte resource's register-state payload and command lifecycle.

## Correct AA companion and optional HZ graph

Allocatorac7e0 at ac980..ac9c9 selects profile+1c54 = entry49 and request-format5.
The genuine4fe00 profile constructor produces class4, hardware format3d and64
stored bits. Request flag01000000 allows multisample selection; it does not
alone prove the card admits it.

Helper477f0 first invokes hardware+fe70 configuration+338 vtable+c getter twice.
Both masks08 and10 (bits3/4) must be set. It then searches table1f1df0, stride18
through offset378, for the requested hardware format in one of its three
format/flag pairs with flag mask02. Constructor51990 calls it at51a3c only when
request flag01000000 is present. If admitted, flag02000000 chooses selector3;
otherwise the normal AA request selects2 and marks resource+3d0. The actual configured MD owner now executes that getter at62310, returning
101f8, and genuine477f0 admits3d (result1) while rejecting an absent80 format
(result0). No helper return is patched. The CPU host's unsupported feature
transport exercises vendor defaults; normal M9128 initialized ownership and
physical allocation are still the native/bootstrap agents' join. This closes
the configured-owner selector2 predicate, not a hardware execution result.

The old claim that flag2 creates a linked kind9 companion was too broad.
Actual517f0 requires request-format9 and height1, as well as other shape gates.
The AA format5 request refuses it, including a height1 control. A format9/height1
control passes; format9/height240 refuses. Its generic cross-links380/384/388 are
not the BA/BB owner in the actual AA binder.

Binderaa750 ataaa7d checks companion+39c, reads optional HZ at+38c and requests
descriptor selector12. This is the plane created by54be0 for class4, using
profile56/format0b at ceil(width/8) by ceil(height/4). The64-bit companion uses
HZFlags mask4. Actual HZ capability/configuration determines whether+39c is set;
BA/BB may be absent. HZ is not a replacement for the AA companion itself.

## Conditional layout and descriptor arithmetic

Real45eb0 and467e0 execute with explicit unbacked layout inputs: format3d,
mode1, selector2, flags120 for AA; format78, mode0, selector1, flags140 for HZ.
The configured default getters and actual f04-derived shared fields2/1 are
retained. Address outputs are unbacked tags and must not be submitted.

| Recipe | Aligned row span/rows | Actual requested extent | Descriptor payload |
| --- | --- | --- | --- |
| AA320x240, selector1 negative control |320/240|96020|B9=7a210140|
| AA320x240, conditional selector2 |320/240|96020|B9=7a220140|
| AA1024x768, conditional selector2 |1024/768|600020|B9=7a220400|
| AA321x241, conditional selector2 |384/256|c0000|B9=7a220180|
| AA16x16, conditional selector2 |64/16|2000|B9=7a020040|
| HZ for320x240:40x60 |40/72|2d00|BB=f0010140|
| HZ for1024x768:128x192 |128/200|19000|BB=f0010400|

All extents/descriptors are hexadecimal. The two standard AA dimensions need
payload width*height*8 plus32 bytes; the odd/small controls prove that formula
alone is not a general allocator. The pre-fb4e710 Linux diagnostic allocated8 bytes
per source pixel (the64-bit size is already recognized), forced descriptor
layout1 and left BA/BB bound to ordinary depth HZ. fb4e710 now matches the actual companion
layout and padding-aware guard; its own optional HZ graph remains open.

The ordinary HZ rows/extent match existing implementation, but the AA companion
must own/initialize its plane rather than borrow the ordinary depth surface's.
Existing55070 conservative initialization is007ff000; a complete initialized
AA resource capture must confirm its active HZ configuration and ownership.

## Reproduce

Run the established emulator Python, without target access:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/pool6-layout.py \
  captures/m9-faa-companion-20261006/pool6-layout.json
python3 tools/m9emu/check-pool6-layout.py
```

The checker verifies the real call chain, refusal boundary, absent backing,
profile outputs, independent linked-request controls, conditional dimension
matrix and byte-pinned producer/consumer spans. This closes geometry and
corrects the ownership graph; physical domain/contents, final lifetime and
normal enabled submission remain open.

## Expanded guard and alignment reference

Root's `aa-layout-matrix.py` executes genuine45eb0 on28 small/odd/standard
shapes with the same unbacked format3d/mode1/selector2/flags120 and separately
retained f04-derived fields2/1. Every case agrees with row-span aligned64,
allocation-height aligned16 and extent `(row*rows + max(0,4-tail))*8`, where
`tail=(rows-height)*row+row-width`. The guard is four stored elements minus
available padding, rather than an unconditional32-byte addition. For319x240
the request is96018;320x240 is96020;321x240 isb4000. Exact1920x1200 needs
1194020,2048x1536 needs1800020. Native growth must reserve this guard and
padding, independent of the32MiB fragment-store cap.

This reference extends the shape proof; it still supplies no GPU backing or
initial contents. Native fb4e710 now uses this padding-aware extent and genuine selector2
descriptor coherently for format3d. The actual-C regression compares all28
vendor size/row/height/descriptor values and rejects invalid dimensions without
changing the output. Fault-injected ownership passes ASAN/UBSAN. Native
FAA-off controls pass in the combined bd24336 deployment. Optional AA-owned HZ,
physical backing, initialized pool6/sentinel and complete command lifecycle
remain open; this is not FAA-on proof.
