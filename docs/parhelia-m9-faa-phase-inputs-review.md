# Independent review of whole FAA phase controls

Host review found no blocker in canonical kernel `d2821c3`'s whole init-only
and post-public-triangle profiles. This is source/domain/lifetime review and
CPU helper verification, not hardware retirement, rendered output or a raw
main/AA record interpretation. The source profile checkpoint is `6f27dae1`.

Reviewed exact file SHA-256 values:

| File | SHA-256 |
| --- | --- |
| `m9x/m9_faa_native.c` | `cdb41e012882c31571cb72674855147ca50f5718d050be5a0bc6cb40744d2ca2` |
| `m9x/m9_faa_init_only_data.h` | `a41fc42f04a8867aaf0e1a9ed35c27ddb58179e282330a62fb7d24eb721751d9` |
| `m9x/m9_faa_post_triangle_data.h` | `5841b16ffec97090a5907cbe9e5c5d4d749b7e528b9b72c428e282626f0d8b35` |
| `m9x/m9_requester.c` | `999d0817a6f4c725ea7538b6096fcd6a04b3c8a0b3f1760c928d0d2667a9bb5d` |
| RE `tools/m9-native-faa-image.c` | `a632d83bdc563ebdcff66710c584a8b56310b9f23c08f90f0466e6940cc736f8` |

The caller SHA includes the parent-owned, then uncommitted `--phase-control`
change and the zero-retirement-point rejection added during review. Its
positive sampled CPU control restores only the caller's black primary
background. Phase submission uses the real FIRST_TRIANGLE ioctl. Success
requires a nonzero returned point followed by actual output-access refusal
with `ENODATA`; it never reaches the pixel oracle or writes an output image.
The kernel's real wait and observations remain the retirement proof.

Both phase headers select output index `U32_MAX`. The runtime helper checks
that index against the live owner-array length before indexing, so output
binding/retired flags remain absent and the read returns `ENODATA` before
reading backing pixels. A wedged transport returns `EIO` instead. The owner
loops use each generated array's actual 27/30 entries; they do not borrow
the complete-output profile's extra temporary owner.

The independent retained-source comparison checker passes: the 4,868-byte
initialized primary prefix is identical between profiles. Init-only has its
own 12-byte inline final packet, target `b0`, 13 timeline fields and four
nonnull address fields. Post-draw has target `120`, 21 timeline fields and
41 nonnull address fields. Its final `121` token resides at secondary `+56c`;
the final primary packet contains the independently produced secondary
`560..570` range. All six direct start/end fields are included and retain
separate source tags one/two. Neither profile is obtained by truncating or
splicing an output stream.

The generic native runtime still derives address operands from live owners,
retains each flags30/B SYSTEM mapped prefix and the separate flags110/A
completion alias, patches every timeline before regenerating the companion
page clear, and submits the resulting live primary length. The requester
checks consumed SYSTEM leaves before `400/404`, preserves its virtual-owner
state on failure, and waits using equality of the real low counter with the
target. The same lock, one-attempt guard, source-init origin `10/next20`,
restore identity/tag checks and cache/lifetime gates remain active.

Original native encoder and companion-clear functions pass for both whole
headers at two native placements and all eight page alignments. Checks cover
source encoder equality, mapped-prefix bounds, final tracker placement,
variable command count/length, already-rebased moved suffix, other-owner and
unknown-byte preservation, plus corrupted/undersized clear rejection. The
caller builds with `-Wall -Wextra -Werror -O2` against the current native UAPI
and installed libdrm's userspace DRM ABI. No executable was run on hardware.

Reproduction from the source-profile RE worktree, using canonical kernel C
and each canonical header:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-phase-comparison.py \
  --capture-root captures/m9128-device-inputs-20261006 \
  --out /tmp/m9-faa-phase-inputs-review.json
```

Run `tools/check-m9-faa-phase-native-cpu.py` for each phase with its retained
`--plan`, `--blob`, `--kernel-header`, canonical `--kernel-source`, and
`--vendor-clear-proof normal-faa-clear-page-runs.json`. Both actual helper
checks passed independently. The separate Windows full-PDEV/miniport mode
transaction and cold hardware admission remain open; these phase controls
provide no new reply or evidence for an inherited register toggle.
