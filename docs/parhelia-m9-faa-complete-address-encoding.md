# Complete-output address and extent audit

The bounded complete180 profile has no demonstrated address-encoding or
retained-extent defect. Its 52 compiled fixups and31 owner definitions match
the actual full180 source plan exactly. The extracted production
`m9_faa_encode` agrees with shipping vendor CPU instructions in310 comparisons:
every site at its captured placement and two different native placements,
plus low-bit/zero/upper-address helper controls. This is a CPU encoder proof,
not a model of GPU address translation or a successful FAA image.

All seven encoders used by this profile overwrite their destination DWORD in
the source. None merges retained upper descriptor bits that the native helper
discards. Actual `24886` writes page base and selector/null control;
`248ab` writes page-relative program entry in units of eight bytes. The
nonzero-offset native program bindings have the matching adjacent entry
relocation and reconstruct their complete byte address. Source `248c8`
replaces the low two address bits with the selector. The range variants
`24909` and `24919` clear their source-defined control bits. `24930` and
`256f1` preserve the address and OR the selector. The used source offsets are
four-byte aligned and strictly inside their retained logical/mapped extents.
The source's separate packed-relative `248f2` branch is not used by this
profile and is not treated as an ordinary absolute pointer.

Production `m9_faa_native.c:894` implements those encoders. Its relocation
loop at1279 checks source/destination ownership, destination alignment,
logical/mapped source extent, addition overflow and the immutable source
word, then checks that the captured operand reproduces before overwriting it.
The current generated header supplies the same typed fixups audited here.
The result makes no claim about untyped address-looking shader words; those
remain covered by their separate source/ISA evidence.

Several non-address literals deliberately remain fixed to the diagnostic
profile. Main's logical extent is `136080`, while native allocation capacity
rounds up to `137000`; the source-derived AD value is logical extent divided
by32, `9b04`, rather than the rounded capacity. Companion logical extent is
`97000` within its `98000` backing. Ordinary output is320×240 with a320×256,
`50000`-byte backing. The fixed descriptor/rectangle producers are documented
in [the resolve-field audit](parhelia-m9-faa-output-resolve-fields.md).
This profile does not derive arbitrary surface format or dimensions from GEM
size and is not general FAA application support. No missing live address was
identified, and no product change follows from this audit. Existing mask and
variable-clear proofs were not repeated.

The checker runs only shipping CPU encoder branches in Unicorn with an owned
CPU output DWORD and a return stack. It does not run GPU commands, supply
resource pixels, modify source control state or claim a full vendor caller
returned. It also compiles the actual production helper separately and
records its source/header, source plan/blob and vendor image hashes.

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-complete-address-encoding.py \
  --kernel-source /path/to/m9x/m9_faa_native.c \
  --kernel-header /path/to/m9x/m9_faa_preclear_output_write_data.h \
  --image xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll \
  --out /tmp/faa-complete-address-encoding.json
```

The retained JSON was produced against integrated kernel `d76d07d`. Hardware
GPU-visible mapping, retirement and pixels are separate obligations.
