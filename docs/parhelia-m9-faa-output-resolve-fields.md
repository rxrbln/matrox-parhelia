# Ordinary FAA resolve field audit

The source-closed ordinary-output producer and canonical generated kernel
table agree on the bounded resolve fields audited here. This host-only audit
does not explain the black hardware image or prove correct GPU pixels. It
adds no status reply, initialization, guessed cache operation or full capture.

Actual `abc20` joins destination selector9 at `abf6c..abf9f`, companion
selector13 at `abf0a..abf46`, HZ selector12 at `abe89..abec5`, main selector14
at `abd08..abd44`, and the genuinely linked AUX selector14 at
`abd75..abda1`. The retained output flush has actual relocation kinds
`2/6/7/9/a` for these bases. The native ownership joins resolve them to
owners `27/4/5/2/3`, respectively, all at offset zero with selector1. This
comes from actual typed producers and retained object/backing relationships,
not numerical address overlap. The unrelated profile48 SYSTEM backing is
not selected by these five resolve bindings.

Several fields bypass address relocation but are bounded layout parameters:

| Actual producer | Retained output | Native interpretation |
| --- | --- | --- |
| `abf7a` selector9 and `fbb19` descriptor store | `c8210140` | Destination format `64`, layout selector1, span320, layout flags bit3 |
| `abf0a` selector13 and `fbbd3` descriptor store | `7a220140` | Actual 64-bit companion view for the retained320x240 case |
| `abe89` selector12 and `fbc2c` descriptor store | `f0010140` | Actual retained HZ view |
| `abdb3..abdc4`, then `fbe16/1f/28/31` | Four `9b04` words | Main backing logical extent `136080 >> 5`, packed into21 bits |
| `f8e94` and following actual mode9 stores | `45fcf3ec,0,0,f00140` | Actual resolve trigger and320x240 rectangle |
| `fcb2f` | `40008000` | Immediate save-control word, not a relocation address |

The source main backing extent is `136080`, even though native storage
capacity rounds up to `137000`. Capacity padding must not replace the
logical allocation footprint used by the mode9 resolve. Both the generated
owner specification and four emitted words retain that admitted logical
size. This fixed state is limited to the guarded320x240 diagnostic; another
surface size, layout or shader needs its actual producer-derived fields.

The actual output layout has flags `b`, so selector9's special address branch
at `46bd8` is inactive: bit2 is clear. The output address is read from
layout `+15c` by `47350`, with selector1 from `+144`. The descriptor words
pack format, span and layout fields; they do not incorporate the GPU base.
For this admitted layout, changing the live destination GPUVA requires its
actual base relocation, while `c8210140` remains unchanged. This does not
extend to selector9 layouts whose special branch calls `46e40` or modifies
packed address bits.

`check-faa-output-resolve-fields.py` pins the vendor instruction bytes and
joins the nine nonrelocation stores through their actual primary MDL write
chronology. It verifies all five typed source-owner joins, the retained
destination format/layout/shape, main extent and full rectangle trigger.
It regenerates the native header from the canonical plan and source-known
blob and requires byte-for-byte equality with the current kernel header.
Address rebasing and variable clear suffix movement remain covered by the
separate actual-helper fixtures; this audit does not substitute a new
parameter inventory or prove GPU cache semantics.

The following passes against canonical source and kernel files:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-output-resolve-fields.py --capture-dir /home/ai/src/parhelia/xf86-video-parhelia/captures/m9128-device-inputs-20261006 --kernel-header /home/ai/src/parhelia/src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm/m9x/m9_faa_native_data.h --native-generator /home/ai/src/parhelia/xf86-video-parhelia/tools/m9-native-faa-output-template.py
```
