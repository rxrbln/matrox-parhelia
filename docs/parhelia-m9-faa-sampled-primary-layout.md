# Retired sampled primary and its CPU view

The late-output control changes the physical sampled primary from zero to
1308 nonzero DWORDs /2819 nonzero bytes in its327680-byte owner. The raw SHA,
FNV04099694, native retained owner and real180 retirement are joined by
`tools/m9-faa-sampled-primary-audit.py` to the archived receipt. This establishes
GPU writes to that owner. It does not decode a pixel or FAA record schema.

The most frequent raw words areff0000f0 and000000ff, each261 times. They must
not be labelled red pixels, alpha, coverage or pointers from resemblance. A
bounded comparison of aligned16-byte windows having at least8 nonzero bytes
finds no exact match in the source-known240-byte C2 program or115160-byte RS
database. That comparison neither decodes these words nor rules out a different
GPU state/store producer.

Actual source separates the primary's engine descriptor from its CPU view.
The sampled layout uses format66, kind2,32 stored bits,320×240 visible
and320×256 allocated. Selector9 emitsCC220140/100f0. The actual direct resource
CPU mapper invokes selector1 and emitsF0220140/10100. The source47350 format
table47694/47670 dispatches format66 to474d2, whose original instruction
explicitly writes format78. Both descriptors name the same source-owned backing
with selector1. The CPU aperture publication is25d96/25dd2/25e0e/25e73.

A raw physical BAR read bypasses that CPU aperture. No recovered CPU consumer
in these proofs converts physical sampled words to ordinary colors; the view's
hardware transformation is an explicit open contract. The verified8b020→8c4a0
ordinary-output coordinate helper proves address arithmetic for the different
kind1 SYSTEM temporary. It does not prove that the sampled BAR bytes are its
RGBA rows. Public sampled Lock also follows its own staging resource path;
the direct2cb40 map is a separately qualified internal CPU view.

The active source viewport is explicitly0,0,320,240 in the retained public DP2
command bytes. The actual owned geometry contains(80,180,.5,1),
(240,180,.5,1),(160,60,.5,1), so its geometric triangle area is9600 pixels.
The final mode9 trigger is45fcf3ec/0/0/f00140, covering320×240. These fields
provide no small rectangle or viewport explanation for sparse physical words.
The actualabdaa producer reads main backing extent136080 and shifts it by5
toAD9b04. This is a logical storage extent encoding, not an observed fragment
count. Neither1308 primary words nor601 main words can be compared directly to
the triangle area without a recovered GPU record/coverage ABI.

`M9_FAA_SAMPLED_DUMP` now permits a bounded separate view observation in
`tools/m9-native-faa-image.c`. Its value is the **exact output filename**. After
the ordinary output oracle has been saved and calculated, it reads the same
retained color handle with flags0 and F0220140/10100 into a separate buffer.
It saves307200 raw visible-row bytes and logs word/byte counts and FNV. A valid
phase-control ENODATA result can also request that read. The helper makes no
additional GPU submission, does not alter the pre-submit input or oracle
status, and emits no PPM or resolved-color claim. Reading this hardware view
and reading raw physical327680 bytes are different observations; the last16
allocation rows are excluded from the view dump.

```sh
python3 tools/m9-faa-sampled-primary-audit.py --image /srv/t2/xf86-video-parhelia/xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll --out /tmp/sampled-layout.json
python3 tools/check-m9-faa-sampled-dump-cpu.py --drm-include /path/to/kernel/drivers/gpu/drm/m9x
```

The offline audit passes, including corrupted-raw and altered-viewport refusal
controls. The actual optional helper passes ASAN/UBSAN opt-out, read-failure,
exact filename and byte-preservation controls; the complete image tool builds
with warnings treated as errors. No GPU bytes are imported into the emulator,
and no new hardware read or submission was performed for this checkpoint.
