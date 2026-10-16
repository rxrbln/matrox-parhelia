# Public-VS live color and projected triangle oracle

The authenticated 796-byte public-VS upload has 92 layout bytes, 328 program
bytes, 152 constants bytes and 224 geometry bytes (SHA-256
`0bead00de84ffa985269193e6266525c5379441415e50d1ef60732432d27d928`).
The layout's FS constant range is `[16,32)`; `[0,16)` is the compiler's
immutable utility float4 `(1,0,0,1)`. The live FS float4 at `[16,32)` is
`(0,1,1,0.5)`, followed by the VS user constant `(0.25,0.5,0.75,1)` at
`[32,48)`. The actual FS program uses M9128 measured constant selectors
`e0/e1`, two eight-byte pairs relative to its bound range. Therefore a
live-state draw must bind owner 28 at `[16,32)`, not `[0,32)`, to render cyan.

The source original PS request used red at the start of its bound range.
Its natural DRAW template records the source writer `f5a57` at secondary
offset 2360 writing `0x00500400`, a four-pair count for `[0,32)`, with the
address pair at 2364/2368. For the authenticated live FS layout, the same
producer rule yields `0x00500200` and addresses owner28+16/+32. In the
combined 60-byte-primary template these fields are offsets 2420/2424/2428.
Changing only the address pair while leaving the old four-pair count would
encode an inconsistent range. The vertex stage already derives its count by
`0x00500000 | ((end-begin)/8 << 8)` from the live layout.

The source public VS copies input clip positions with `w=1`. Viewport
`(0,0,320,240)` maps vertices `(-0.5,-0.5)`, `(0.5,-0.5)`, `(0,0.5)` to
screen `(80,180)`, `(240,180)`, `(160,60)`. The bounded F021 oracle checks
solid cyan inside that triangle, zero outside, antialiased edge coverage, and
alpha near half coverage. It deliberately rejects the previous red utility
triangle. A CPU-only test recolors an archived red coverage mask to exercise
the oracle and six corruptions; this is not a public-VS GPU image or a pixel
receipt.

Run `python3 tests/m9-faa-graph-public-vs-20261007/check-pixel-oracle.py`.
After the real `0x190` Lock receipt and a source-qualified F021 mapper return,
run `tools/m9emu/m9_faa_public_vs_pixel_oracle.py --input INPUT --raw RAW` on
the exact same-boot capsule. Its expected pixels are a live-state requirement,
not evidence that the current native packet already satisfies it.
