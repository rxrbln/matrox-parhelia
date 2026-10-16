# Normal FAA direct address and counter producers

`normal-faa-native-addresses.json` complements the normal known-input manifest.
The observer runs the same genuine normal CPU frame and stops before the
retirement status load. It makes no mapping call and supplies no GPU effects.
Run `tools/m9emu/m9_faa_native_addresses.py` with the same runtime/PCI/DC
arguments as `m9_faa_inputs.py`; choose its output with `--out`. Audit with:

```sh
python3 tools/m9emu/check-faa-native-addresses.py \
  captures/m9128-device-inputs-20261006/normal-faa-native-addresses.json
```

## Direct primary pointer exports

`24cb2` and `24cd5` store the actual SECADDRESS/SECEND payloads directly,
outside `25680`. Preserve the primary header `45452d2c`, selector2 on the
start and kind1 end-mode2. The counted secondary is not copied or stripped.

| Primary DMA offset (`1001000` owner) | CPU store | Actual payload |
| --- | --- | --- |
| `1348` | `24cb2` | `1042002` |
| `134c` | `24cd5` | `104225e` |
| `1354` | `24cb2` | `104225e` |
| `1358` | `24cd5` | `1042562` |

The exact secondary byte ranges are `1042000..104225c` and
`104225c..1042560`. Each payload is joined to its actual queue buffer,
CPU start/end, translation field and address selector. Their offsets in the
system DMA owner are established by real CPU writes, not numeric matching.

## Counter tokens and activation addresses

Twenty `2557b` stores emit E3 completion tokens `21,31,...,151`, derived
from the genuine CPU timeline low counter OR1. Two `25338` stores emit GPU
wait index43 tokens `f0` and `130`. They bypass `25680`; their precise stage,
DMA slot and chronological CPU-write sequence are retained. The final CPU
wait desires `150` and stops before its first counter load.

Four shader-loader E3 fields also carry the current timeline low without OR1.
They follow selector E6=`04000001` and bypass `2557b` as well as `25680`:

| Primary DMA offset | CPU store | Current timeline low |
| --- | --- | --- |
| `124` | `f5898` | `30` |
| `4f0` | `f5898` | `50` |
| `fbc` | `f5b30` | `50` |
| `1510` | `f5898` | `140` |

Immediately before each store, the actual indirect calls at `f5893` or
`f5b2b` invoke queue vtable slot `+70`, resolving to `25860`. Its pinned
instructions follow `(queue+2c)->4` to the platform and return its `+38`
low and `+3c` high in EAX/EDX. The passive observer records the source fields,
callee and instruction bytes before the call, then proves that the immediate
E3 store matches the returned low. All four observed high words are zero.
These are timeline values with low control bits zero; native rebasing must
preserve that form. Their GPU-side selector effects are not supplied.

The complete normal timeline inventory is therefore **26 fields**: twenty
completion tokens, four selected E3 fields and two GPU waits. E3 coverage is
18 primary fields plus six counted-secondary fields. The checker pins all
observed CPU producer sites and exact DMA slots; a generic register decoder
alone does not establish coverage inside the inline program-upload grammar.
The earlier 22-field checkpoint omitted these four shader-loader stores and
must not be used for native execution.

The actual common buffer is system DMA `2007000`, 159 bytes. CPU writes
prove its first128 bytes, including literal16 at offsets `0,20,40,60` and
zeros elsewhere in this prefix. Its last31 bytes remain unknown. The
captured16 is an initializer, not a GPU completion for this frame.

Seven activation MMIO address payloads bypass the packet relocation helper:

| MMIO | CPU store | Actual payload | Proven source |
| --- | --- | --- | --- |
| `400` | `23a09` | `1001002` | Primary DMA translation/cursor and selector2 |
| `1068` | `23a3b` | `2007022` | Platform field18, common buffer offset20, tag2 |
| `106c` | `23a9d` | `2007002` | Platform field30, common buffer offset0, tag2 |
| `1070` | `23aaf` | `2007042` | Platform field60, common buffer offset40, tag2 |
| `10ec` | `23ae4` | `a81201` | Default restore descriptor base+200 aligned32, descriptor tag1 |
| `1094` | `23b0a` | `a81200` | Default restore descriptor base+200 aligned32 |
| `1098` | `23b41` | `c00c06` | Descriptor base+200+(count-1)*600, maskffffffe6 OR6 |

The last three follow `(queue+15c)->20`, whose actual descriptor GPU base is
`a81000` and count1024. This default queue restore pool is separate from the
FAA pool6 resource at `11e1000`. Its bytes are not inferred from numeric
overlap with resource candidates or from untouched guest RAM.

The observer records all actual MD MMIO events, but the seven rows above are
selected by the exact producer instruction and source graph. Other allocator
MMIO address fields belong to separate aperture/root initialization contracts;
this evidence does not authorize replaying that entire MMIO list.

## FAA pool preparation chronology

Both actual `17e480` triangle launches precede `fc99b -> 24f20`, then
`fc9e9` invokes the sentinel restore producer. The actual raw preparation
image is76 bytes and has this exact sequence:

```text
45454524 11e1001
453ce6e6 6dc00000 42000000 000000a1
44444444 30081898 30081898 30081898 30081898
44444444 30081898 30081898 30081898 30081898
45452f2e 11e1285 11e12c5
```

The final pair names FAA pool offsets `284..2c4` with low tag1. CPU command
production proves this binding and order, not the actual GPU save footprint
or the bytes subsequently fetched from the pool. There is no CPU memset of
FAA pool6. A native owner must preserve the actual save/restore timeline and
join the GPU producer before declaring unspecified pool bytes initialized.
