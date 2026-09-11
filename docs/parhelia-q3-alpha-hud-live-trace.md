# Parhelia Q3 alpha/HUD trace, 2026-09-11

Target: `root@192.168.1.111`, X PID 12013. Mesa was `/root/parhelia-batch/lib/libgallium-26.2.2.so`; module remained the `/tmp`-staged build and `pack_draw_packets=Y`.

## Alpha-test draws

`PARHELIA_TRACE_ALPHA_TEST=1 PARHELIA_TRACE_ALPHA_CUTOUT=1` produced two unique native alpha-test states and no `alpha-cutout micro` event:

- 8x16, format 84, texture hash `0xa2f82423f2494713`, 23 zero-alpha and 105 nonzero-alpha texels.
- 32x64, format 84, texture hash `0x1392c28f51bc4eb3`, 0 zero-alpha and 2048 nonzero-alpha texels.
- Both use `GEQUAL 128`, depth test/write with `LEQUAL`, linear min/mag, mip-none, repeat, no blending, RGBA mask.

Both report `mip_defined=0x1`. `ph_use_alpha_micro_cutout()` says that only levels beyond zero must be undefined, but line 1012 requires `!texture->mip_defined`, so neither live draw can enter it. The predicate should require exactly base level: `texture->mip_defined == BITFIELD_BIT(0)`.

## Bottom HUD corruption

The striped bottom digits are source-over draws, not alpha-test draws. Every repeated draw in that region selected `two-pass=1 micro=0 alpha-test=0`, including:

- 32x32 hash `0xba1b38b73dc17fdd`, p0 at (2,480) and (187,480), 326 occurrences each.
- 32x32 hash `0x822adefe62cfcfde`, p0 at (34,480), 326 occurrences.
- Dynamic 32x32 hashes at (219,480) and (251,480), including `0xd9fecdc36f36a943` 149 times.

Removing `PARHELIA_BATCH_STATE_STREAM` left the horizontal stripe pattern unchanged. This rules out a missing state-stream/pixel-pipe boundary as the cause. The affected texture images are 32x32; the retained hardware companion probes use 64x64 textures. The next discriminating fix is to allocate each mask/premultiplied companion with a minimum 64x64 descriptor/storage, pad the layout-2 image, and scale ST by logical/physical dimensions. This directly tests the unproven sub-64 companion texture geometry while preserving GPU-only two-pass blending.

Artifacts:

- `/tmp/q3-alpha-trace.log`
- `/tmp/q3-alpha-trace.png`
- `/tmp/q3-alpha-source-trace.log`
- `/tmp/q3-no-state-stream.png`
- `/tmp/q3-state-stream-hud.png`
- `/tmp/q3-no-state-stream-hud.png`
- `/tmp/q3-batch-trace.log`

## Batch-only trace

The requested 12-second `PARHELIA_TRACE_BATCH=1` trace is 259,307 bytes with 2,553 aggregate lines. Dominant exact shapes:

- 268: submits=2 commands=332 packets=42 stages=2 relocs=12 vertices=96
- 258: submits=18 commands=5295 packets=997 stages=18 relocs=709 vertices=18570
- 257: submits=8 commands=3779 packets=974 stages=7 relocs=865 vertices=23004
- 256: submits=21 commands=5666 packets=1001 stages=21 relocs=666 vertices=16848
- 256: submits=10 commands=3844 packets=938 stages=10 relocs=788 vertices=17952

Final restored state: untraced Q3 PID 14083 with `PARHELIA_BATCH_STATE_STREAM=1`, X PID 12013, `pack_draw_packets=Y`.
