# Sampled-primary and temporary resolve roles

The actual `ac1f0` allocation-success flow resolves into its newly allocated
ordinary SYSTEM temporary. It does not pass the sampled primary as a source
argument to `abc20`. The allocation-failure branch instead resolves tiles
**into** the sampled primary. These are source-established destination
alternatives, not evidence for a missing primary-to-temporary copy.

The actual retained inner graph is outer owner `+4`. Its fields `+4/+8/+c`
identify sampled primary, companion and main. `51870` returns the temporary;
`ac2a0..ac2a4` copies EAX to EBX and branches to `ac408` only when that
return is zero. The successful `ac2c8..ac2d4` sequence pushes main, companion,
temporary, producing `abc20(temporary, companion, main, rectangle)`. The
complete full180 source manifest records exactly this call and distinct
primary/temporary objects. No new GPU completion or shader output was supplied.

The static failure branch computes clipped tiles no larger than 64×64 and
uses actual `47720` on the companion layout to translate their rectangles.
`ac4e3..ac4fe` pushes main, companion, sampled primary instead. Then
`ac585..ac588` selects that primary as the `afbe0` copy source. An explicit
caller CPU buffer takes the `ac60a` copy call; the other branch maps the
separate caller argument through `55850`. This audit does not claim that
allocation failure or either copy returned successfully. A genuine OS
allocation-failure capture must establish that branch without changing the
resource flags, return register, instruction pointer or GPU replies.

Inside `abc20`, argument0 supplies selector9's destination pair/descriptor
(`abf4e..abf9f`). Argument1 supplies selector13 companion and its linked
selector12 HZ. Argument2 supplies selector14 main, its linked auxiliary and
logical backing extent. The pool comes from the live device/head service
(`abfcb..abffc`), rather than another primary argument. The full180 manifest,
typed relocation operands, native retained owners and byte-exact input blob
join all six final bindings: output27, companion4, HZ5, pool22, main2 and
auxiliary3. The companion address has two observed resource candidates; its
actual caller object selects the graph-owned candidate. The eight texture
callbacks are null and supply no sampled-primary input. Draw separately binds
the sampled primary as its output.

The separate `aa750` policy uses AA shadow `+14=1f,+20=0`, whereas this
`abc20` policy uses `1d,ffff`. Their difference is retained as source evidence,
not a license to replace the genuine caller or copy guessed state. The ordinary
temporary also takes the null sentinel-range path, as established by the
[draw/resolve view audit](parhelia-m9-faa-draw-resolve-views.md).

This leaves an important limit: the CPU argument and explicit binding proof
does not decode the GPU's main/companion records or prove whether they contain
indirect color references. The phased native run has real main/AA writes and
black output27, but omitted the borrowed sampled-primary bytes. Capturing that
owner can establish where color was produced. Finding color there alone would
not prove that `abc20` requires an extra copy, because the actual successful
source caller also omits primary as an explicit input. There is no causal
runtime patch or FAA pixel qualification from this audit.

The reproducible checker pins the shipping DLL instructions, joins the current
actual arguments and all explicit final operands, and rejects five controls:
primary substituted as the successful destination, swapped companion/main,
wrong output retained owner, and a non-null supposedly disabled texture pair.
The committed JSON records input and image hashes and the static/executed scope.

```sh
python3 tools/check-m9-faa-resolve-resource-roles.py \
  --image xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll \
  --out /tmp/faa-resolve-resource-roles.json
```

No captured address is used to construct a native command. All values in this
audit are comparisons of retained source evidence.
