# Draw and resolve views, including sentinel feedback

The complete source/native audit finds no different main, auxiliary,
companion, HZ or pool owner between the public triangle and final ordinary
output resolve. Each typed binding joins the same retained allocation,
domain, zero view offset and vendor encoder in both stages. The main logical
extent encoding remains `136080 >> 5 = 9b04`, AF remains zero, and companion
and HZ descriptors remain `7a220140` and `f0010140`. Native allocation capacity
does not substitute for the logical main extent.

Actual `fb920` emits main/aux BC/BD through callbacks at `fbdbc`/`fbdd3`, using
shadow pairs `+80/+94`. Actual `abc20` builds those read pairs from the genuine
main and linked auxiliary selector14 caches, while `abdaa..abdc4` obtains the
logical extent from main's retained backing object. The complete known GPU
stream and native fixup table preserve those fields. This audit does not
interpret the device's opaque main/AA records as pixels or claim a view fix.

There is an additional earlier state-save operation. `fce50` calls `fc8f0`
with its saved sentinel and pool pairs. `fc9c4`, `fca5b`, `fcae1` derive sentinel offsets
`8`, `12`, `16`; actual kind30 callbacks use encoder `256f1`. The three bindings
precede the final main/AA rebind and trigger. Their source control words are
`a1000009`, `a3000009`, `a4000009`, followed by `40008000`. These controls are
pinned shipping immediates, not guessed addresses or a decoded GPU record ABI.
The native table correctly derives all three pointers from its live retained
sentinel owner. The initial source-known words at these offsets are zero;
the actual GPU-produced values must be observed rather than supplied.

The first sentinel DWORD `a4a3a1a1` encodes the indexed-header byte sequence
`A1,A1,A3,A4`. Its complete initial20B record is header, `28041968`, zero,
zero, zero. The triangle's actual `fcb60` range binding joins this retained
sentinel `+0/+20`, through kinds2C/2E. The save controls choose A1/A3/A4 and
their address slots match the corresponding payload positions in this record.
The exact hardware meaning of those selected state fields is not proven here.

Crucially, the **final ordinary-output resolve emits no sentinel range**.
Its ordinary temporary destination takes `abc84..abcdf`'s null sentinel path,
and the new `fcb60` shadow skips that range. The only resolve range joins the
distinct pool at `+284/+2c4`, kinds2C/2F. The earlier manual sampled-primary
resolve lifecycle cannot be substituted for this flow. Saved sentinel words
are produced-state evidence; they are not a demonstrated final resolve filter
input. The source/native checker now asserts this distinction explicitly.

The qualified public-red and post-triangle raw archives retain the main, AA,
pool and preprocessing buffers, but omit these exact sentinel words. Their
before/after sentinel hashes and nonzero counts cannot recover them. The old
`m9-faa-root-join` sentinel binary belongs to an earlier different candidate;
its bytes are not substituted for the current input. Missing observation is
not proof of an incorrect pointer, empty record list or resolve predicate.

`tools/check-m9-faa-draw-resolve-views.py` independently joins actual typed
operands, native owner/fixup tables, byte-exact input blob, AA shadow fields,
shipping producer instructions and the three feedback headers/controls. Run
the profile's strict whole-flow checker first, then:

```sh
FAA_CAP=captures/m9128-device-inputs-20261006
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-draw-resolve-views.py \
  --manifest "$FAA_CAP/normal-faa-output-constant-red-input-manifest-101f8.json" \
  --plan "$FAA_CAP/normal-faa-output-constant-red-native-plan.json" \
  --blob "$FAA_CAP/normal-faa-output-constant-red-known-inputs.bin" \
  --out /tmp/faa-draw-resolve-views.json
```

The genuine PDEV flags control uses its corresponding manifest/plan/blob and
passes the same audit. With a future retained instrumented test, add
`--prefix-log /path/to/proof/dmesg-after.txt`. The checker derives the sentinel
owner from the actual profile's three callbacks, demands one before/retired
pair from the same native BAR owner, checks all 20 source-known initial bytes,
and exports the observed words at `8/12/16`. It assigns no meaning to their
values. Hardware identity, boot, actual retirement and lifetime must still
pass the separate qualification/archive checks.

The companion kernel change logs `min(20, observed extent)` raw bytes for
every admitted owner using its existing first observation chunk. There is no
hardcoded sentinel index/address, additional GPU read, command or memory
write. This is a narrowly discriminating observation of a previously missing
produced-state record, not an FAA implementation change or image claim.
