# Fresh prepared FAA program mapping and first consumption

The whole `prepared-preclear-draw-wait` source capture continues the original
public request using only the qualified fresh prepared `c0` counter receipt.
It stops before the new `252b9` completion read requesting `110`. A separate
real queue finish emits the retained `5068`-byte primary and target `120`.
Neither `110` nor `120` has been supplied to this emulator continuation.
The public request remains unreturned; this is not a pixel result.

Actual `2cb40` receives `[program,0,1]` before the initial query wait. The
program has logical extent `1080`, allocation kind0, local layout kind1 and
240 known bytes. After the original query/write waits return using the real
receipt, `2cba8` writes frontend314=1. Actual `59410` copies two existing
compact leaves into the selected CPU aperture child, through the CPU fallback
at `594a2`. The copy has pretoken0 and posttoken `30000c8f`; `59b82` emits
frontend31c=f,0,c. The existing GPU leaves are unchanged. The CPU destination
indices are distinct from GPU page indices and come from a genuine allocated
view, not captured-address inputs to a native producer.

The returned raw CPU alias is used by exactly60 DWORD writes at `1e5383`
(`rep movsd`), covering program offsets0..239. Their values reconstruct all240
known source program bytes exactly. No opaque remainder is initialized.
Actual `2c840` releases the view through `27100`, then `2c878` writes31c=0.
There are no other post-boundary hardware writes before the subsequent primary
posts in this bounded observation:314,31c[f,0,c],31c0, then404 posts.

This program does **not** produce a formatted selector1 descriptor: its cache
remains the constructor's invalid `[fffffffc,0,10000]` value. `27170` returns
descriptor+1c plus mapoffset; the proven raw aperture offset has low bits0.
A guessed F0 surface descriptor or tagged formatted surface upload is not
qualified by this trace. A native counterpart can use an owned raw linear
alias, derive its CPU root entries from the live program leaves, upload only
the source-known spans after the real first retirement, and release frontend
state. Its native alias may follow the native owned identity policy rather
than the vendor's different allocated CPU indices. This preserves a bounded
source transition; hardware equivalence and any cause of black pixels remain
unproved. The previous kick-only diagnostic is insufficient evidence for the
full mapping/upload ordering.

The only nonnull program pair in this prefix is C2/C3 at secondary304/308,
selecting the same program at offset32. Consequently the generated program
entry inventory is mask2; requiring both entry0 and entry32 rejects this
legitimate prefix. The resident database binding is separate.

All earlier LOCAL known spans remain byte-exact. The program existed at the
first boundary but had zero known bytes and no initial C2 reference. The
initial typed nonnull inventory references only the four actual local owners
5/7/10/11, plus the two independently proved clear owners1/4. Initial program
pairs are null and there is no indirect secondary or constants reference.
The new secondary and constants occupy previously unconsumed MDL spans;
the first actual primary indirect reference is5048, after the complete c0
source endpoint4980. Primary changes are append-only and earlier MDL known
bytes remain identical. Thus staging new secondary/constants early cannot
change any source-proved earlier consumption. Deferring the program's240B
upload to its actual mapping phase additionally preserves that CPU timing.
This conclusion concerns the retained finite input grammar, not arbitrary
hidden hardware state or opaque GPU records.

The exporter retains the original stack and exact earlier constructor/prep
objects. All four actual common-counter channels are joined to the immutable
c0 receipt. Unknown GPU-owned BAR/MDL/contiguous reads and pixel-copy entry
remain guarded; none was supplied. No direct Lock call was spliced into the
pending DRAW and no driver field was forced.

Reproduce from the isolated/source-complete tree:

```sh
sh tests/m9-faa-prepared-draw-source-20261007/capture.sh
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-prepared-program-mapping.py \
  --captures captures/m9-faa-prepared-startup-20261007 \
  --base-plan /home/ai/src/parhelia/xf86-video-parhelia/captures/m9-faa-prepared-startup-20261007/native-plan.json \
  --image /srv/t2/xf86-video-parhelia/xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll \
  --out tests/m9-faa-prepared-draw-source-20261007/program-mapping-proof.json
```

The three raw files begin
`normal-faa-prepared-preclear-draw-wait-`. The focused proof joins the mapper,
leaf stores, cache state, CPU upload, release and first input consumption.
Whole typed/native generation and the next hardware qualification are separate
required steps. Existing old profiles and prepared startup source files remain
unchanged. The earlier source-only controller proof belongs to preparation
commit3faf2db6; this new capture additionally records passive mapping metadata.
