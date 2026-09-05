# Licensing and redistribution audit

Copyright (C) 2026 René Rebe <rene@exactco.de>

What in this tree can ship in a GPL-2 source release of the driver, what cannot,
and the concrete steps to close the gap. This is an engineering checklist, not a
legal opinion — where the call is a judgement rather than a fact, it says so.

The operating rule throughout: **reverse engineering for interoperability is
fine, and facts recovered by it (register addresses, bit meanings, encodings,
sequences) are not copyrightable. Vendor object code and vendor data copied
byte-for-byte out of Matrox binaries are copyrightable and cannot be
redistributed.** Everything below is sorted by that line.

Audit basis: working tree at commit `337df82`, `git ls-files` (110 tracked
paths), full `git log --all --diff-filter=A` path list, and `git check-ignore`
against every vendor directory and installer present.

---

## 1. Must fix before a public release

In priority order. Items 1–5 are blocking; 6–9 are correctness and hygiene that
should not go out wrong.

| # | Item | Action |
|---|---|---|
| 1 | `Makefile.am`: `EXTRA_DIST = README.md docs tools` | **Stop dist'ing `tools/` wholesale.** This one line is what turns every problem below into an actual redistribution: `make dist` packs `tools/warp_ucode.h`, `tools/warp_programs.h` and `tools/p690.rom` into the release tarball. Replace with an explicit file list. |
| 2 | `tools/warp_programs.h` (tracked) | **Delete from the tree.** 8 arrays, 2868 bytes of vendor WARP microcode extracted by symbol from `mtx_parhelia.o`. It additionally carries `Copyright (c) 2026 Rene Rebe` + GPL-2 over Matrox's object code — a false licence grant, worse than the inclusion itself. Regenerate locally (§5). |
| 3 | `tools/warp_ucode.h` (tracked) | **Delete from the tree.** 1203 dwords / 4812 bytes, symbol `mtx2785`, the WARP RAM self-test. Regenerate locally (§5). |
| 4 | `tools/p690.rom` (tracked, 52736 bytes) | **Delete from the tree.** A complete Matrox VGA/VBE BIOS image (`MATROX/P650/P690/P750 VGA/VBE BIOS (V1.8) b16`, PCI 102b:2539). This is a whole vendor binary, committed. Users dump their own (§5.4). |
| 5 | `captures/warp-programs/mtx1824.bin`, `mtx2068.bin`, `mtx3213.bin` (tracked) | **Delete from the tree.** 204 + 120 + 192 bytes of vendor WARP program images extracted by symbol. Same class as #2, just in a different directory. |
| 6 | `README.md` says "License: MIT. See `LICENSE`." — `LICENSE` is GPL-2 | **Fix the README.** Every `src/` and most `tools/` files carry `SPDX-License-Identifier: GPL-2.0`. The project is GPL-2; the README is simply wrong. |
| 7 | MIT permission notices in `configure.ac`, `Makefile.am`, `src/Makefile.am`, `man/Makefile.am` | **Replace with GPL-2 headers.** Leftover from the xf86-video-* skeleton. A tree that is GPL-2 in the sources and MIT in the build system will be read as ambiguous by every packager. |
| 8 | `vbios-matrox-parhelia` (untracked, **not ignored**) | **Add to `.gitignore` now.** 43008-byte Parhelia 0x0528 VBIOS image sitting in the repo root, one `git add -A` away from being committed. `tools/vbios.py` even defaults to it as its `rom` argument. |
| 9 | `tools/__pycache__/*.pyc` (tracked) | **Untrack and ignore.** Build artefacts, not source. No `__pycache__`/`*.pyc` rule exists in `.gitignore`. |

Nothing here requires a git history rewrite for the *vendor trees* — see §2.5,
the history is clean of them. Items 2–5 and 9 *are* in history and the maintainer
has to decide whether deletion-going-forward is enough (§2.6).

---

## 2. Inventory

### 2.1 Our own original work — ships as GPL-2

All of `src/` (15 files, ~5900 lines): driver, modeset, PLL, DDC, PInS parser,
RandR, cursor, Xv, EXA/XAA, and the M9 DP/ring/EXA/XAA set. These are original
implementations written *against* the documented facts. They cite vendor symbol
names in comments (`mtx645`, `MTXHWCursorInit`, …) as provenance — that is a
citation, not a copy, and is exactly what an RE'd driver should do. Keep.

All of `tools/` except the three items in §2.3: the hardware test programs
(`parhelia-3dtri.c`, `-tri.c`, `-gears.c`, `-replay.c`), the capture harnesses
(`-glsweep.c`, `-texsweep.c`, `-bankb.c`, `-cursorcap.c`, `-drawcount.c`), the
`LD_PRELOAD` tracer (`-mtxtrace.c`), the disassemblers and extractors
(`parhelia-warpdis.py`, `parhelia-warpreg.py`, `parhelia-fragdis.py`,
`parhelia-extract-ucode.c`, `vbios.py`, `parhelia-glsweep-decode.py`), the
`dma32buf` kernel module and `dma-ring-test.sh`.

Note specifically: **the extraction and disassembly tools are fine to ship.** A
program that reads a vendor binary contains none of it. `parhelia-warpreg.py`
and `parhelia-extract-ucode.c` are the right shape already — they take the
vendor object as an argument. It is only their *cached output* (§2.3) that is a
problem.

`docs/` — 40 files, ~1.2 MB. Assessed in §6; ships.

### 2.2 Documentation of RE'd facts — ships

Register maps, bitfield tables, opcode tables, state→fragment decision trees,
call graphs, sequence descriptions. Facts about an interface, independently
recorded. `docs/parhelia-warp-codegen.md` §6.1 already states the correct
position for the fragment library and is the model for how the rest should read.

`captures/` **text** files (14 files, ~1.4 MB) — `mmiotrace` logs, `mtxtrace`
ioctl/MMIO logs, glsweep buffer diffs. These are *observations of a running
system*: register addresses, values and ioctl payloads. Facts. Ship or don't
ship as you prefer (they are large and only useful to RE work), but they are not
a redistribution problem.

### 2.3 Vendor object code / data, extracted verbatim — does not ship

| Path | Tracked | Size | What it actually is |
|---|---|---|---|
| `tools/warp_ucode.h` | yes | 4812 B (1203 dwords) | `mtx_parhelia.o` symbol `mtx2785`, the WARP RAM self-test program |
| `tools/warp_programs.h` | yes | 2868 B (717 dwords, 8 programs) | `mtx_parhelia.o` `.data` — the 7-program boot burst plus `mtx1824` |
| `captures/warp-programs/mtx{1824,2068,3213}.bin` | yes | 516 B total | the same programs as raw images |
| `tools/p690.rom` | yes | 52736 B | complete Matrox P650/P690/P750 VGA/VBE BIOS |
| `vbios-matrox-parhelia` | no, **and not ignored** | 43008 B | complete Matrox Parhelia 0x0528 VGA/VBE BIOS |

The two ROMs deserve a note of their own: a VBIOS image is *also* not
distributable by us, but it is materially different from the microcode — it is
already on the user's card, and any user can dump it from
`/sys/bus/pci/devices/*/rom` in one command. There is no reason to carry it.

The 574-fragment vendor microcode registry (`docs/parhelia-warp-codegen.md`) is
correctly **not** in the tree in any form. `parhelia-warpreg.py` reads it out of
the user's own vendor object at need. That is the pattern the rest should follow.

### 2.4 Vendor binaries present for analysis — must never be tracked

Present in the working tree, all confirmed `IGNORED` by `git check-ignore`:

| Path | Size | Ignore rule |
|---|---|---|
| `matroxdriver-x86_64-cvs-20090511/` | 27 M | `matroxdriver-*/` |
| `m9xdriver-x86_64-1.4.3-80847-20140908-build_98/` | 700 M | `m9xdriver-*/` |
| `xddm32_210_00_105_se_u_nowhql/` | 97 M | `xddm32_*/` |
| `xf86-video-mga-2.1.0/` | 2.6 M | `xf86-video-mga-*/` |
| `mtxdriver-installer-x86_64-cvs-20090511.run` | 6.6 M | `*.run` |
| `m9xdriver-...-build_98.run` | 187 M | `*.run` |
| `xddm32_210_105_se_u.exe` | 55 M | `*.exe` |
| `scratchpad/` | 152 M | `scratchpad/` |

`scratchpad/` warrants an explicit mention because it is the least obvious:
`libGL.asm` (51 M), `pard.asm` (26 M), `mtxk.asm`/`mtxk_r.asm` (20 M),
`mtx_drv.asm` (6 M) are `objdump` output of vendor binaries — mechanical
transformations of vendor code and therefore derivative works. They are correctly
ignored and must stay local. **Verdict: the `.gitignore` vendor coverage is
complete for every tree and installer present. The only gaps are
`vbios-matrox-parhelia` and `__pycache__` (§1 items 8–9).**

Note `xf86-video-mga-2.1.0/` is not a "vendor blob" — it is MIT-licensed X.Org
code and could legitimately be referenced. It is still right to keep it out of
this repo; just don't describe it as proprietary in whatever notice you write.

### 2.5 Git history

Clean of vendor trees. 110 paths have ever been added across all refs; no `.o`,
`.so`, `.asm`, `.run`, `.exe`, no `matroxdriver*`/`m9xdriver*`/`xddm32*`/
`scratchpad` path has ever been committed. The largest blob in history is a
331 KB capture text file.

### 2.6 The one history question

`tools/p690.rom`, `tools/warp_ucode.h`, `tools/warp_programs.h` and
`captures/warp-programs/*.bin` **were committed** and remain reachable in
history. Deleting them in a new commit removes them from the tarball and from
checkouts of the tip, but a clone still carries them.

Two defensible answers, and I do not think one is obviously right:

- **Delete going forward** (simplest). The release tarball is clean, which is
  what packagers redistribute. Reasonable if the repo has no public clones yet.
- **`git filter-repo` the four paths out of history** before the repo is
  published. Cheap now (110 paths, small history), impossible-in-practice later.

Since this repo does not appear to be public yet, the second is the low-cost
choice and I would take it. If the repo is already public, do the first and
don't pretend the history is clean.

---

## 3. Per-item disposition

One recommendation each, as requested.

**`tools/warp_ucode.h` → delete; replace with build-time extraction (§5).**
It has exactly one consumer, `tools/parhelia-3dtri.c:47`, which loads it only
under an explicit "load the self-test" path and documents at line 371 that it is
deliberately *not* loaded on the normal path. So the shipped tool loses a
debug option, not a feature. `tools/parhelia-extract-ucode.c` already
regenerates it byte-for-byte with its own defaults:
`parhelia-extract-ucode mtx_parhelia.o -c -o warp_ucode.h`.

**`tools/warp_programs.h` → delete; replace with build-time extraction (§5),
and rewrite for real (§4).** Two consumers in `parhelia-3dtri.c` (line 48,
used at 391–397). This one *is* load-bearing for the 3D bring-up tool: without
the boot burst the WARP engine consumes vertices and rasterises nothing. So it
needs both the short-term extraction step and the long-term clean-room
replacement. The false copyright header must go with the file regardless.

**`captures/warp-programs/*.bin` → delete outright.** No consumer in the tree;
`grep` finds no reference. They are a convenience cache of three symbols that
`parhelia-extract-ucode -s mtxNNNN` regenerates on demand. Nothing is lost.

**`tools/p690.rom` → delete; document how to dump one.** Two references, both
in comments (`src/parhelia_pins.c:15`, `src/parhelia_pll.c:107`) citing it as
the source of an RE'd fact. Rewrite those to name the ROM *version string*
rather than a path in the tree — the fact survives, the file goes. `vbios.py`
should keep taking a path argument, with no default.

**`vbios-matrox-parhelia` → keep locally, add to `.gitignore`.** This is
legitimate local research material. Just make it impossible to commit by
accident, and drop `vbios.py`'s `default="vbios-matrox-parhelia"` so the tool
does not imply the file ships.

**`captures/cmdbuf-draw-0528-{pre,post}.bin` → keep.** 139264 bytes each, but
sparse — 1134 and 857 non-zero bytes respectively. I checked directly: neither
contains any of the three WARP program images nor any prefix of `warp_ucode`.
They are snapshots of the vendor ICD's command-buffer mapping, i.e. recordings
of *state the vendor driver programmed*, which is the protocol-fact class, not
the object-code class. Low risk. Worth a one-line provenance note in
`captures/` saying what they are and how they were made.

**`src/parhelia_m9_dp.c` `m9_tx_t1`–`t5` → keep, but re-derive when you can.**
37 register/value pairs transcribed from the Windows M9 driver's `.rdata`
(cited in the file as `[MxMM .rdata 0x464d8 / 0x46538 / 0x46560 / 0x46588 /
0x465e8]`). This is the greyest item in the tree and I want to be plain about
it: it is a verbatim copy of vendor *data*, not code, it is small, it is an
initialisation sequence for a third-party DisplayPort transmitter (not for
Matrox silicon), and it is functionally required for interoperability — all of
which argue it is uncopyrightable interface data. That is a judgement call, not
a certainty. The clean fix is cheap and removes the question entirely: capture
the same I2C traffic on hardware with the tracer and re-derive the tables from
the capture, then cite the capture instead of the `.rdata` offsets. Do that
before release if the M9 path matters; keep it and cite honestly if not.

**`scratchpad/` and the four vendor trees → keep for local research, add a
notice.** Add a short `scratchpad/README` (itself ignored, or tracked and
listing nothing) or a section in the main README stating that RE work expects
the user to supply their own copy of the vendor packages, that none of it is
distributed, and where the ignore rules are. Cheap, and it tells the next
contributor the rule before they trip over it.

---

## 4. What has to be rewritten clean-room

Separate from "must not ship" — this is the list of things that must eventually
*exist as our own code* for the driver to work without any vendor package.

| piece | state | spec to write it from |
|---|---|---|
| WARP vertex microcode library | vendor object code only | `docs/parhelia-warp-codegen.md` §1 (state→fragment table), §6.2, §6.3 (minimum viable path: 13 instructions for transform+colour); ISA in `docs/parhelia-vertex-isa.md` PART 2 |
| WARP boot program set | vendor object code only (`warp_programs.h`) | same; the burst is 222 instructions, the store holds 512 |
| fragment/combiner programs | not in tree; generated | `docs/parhelia-fragment-isa.md` — the encoding is solved, so this one is genuinely writable now |

`docs/parhelia-warp-codegen.md` §6.1 already says this in the right terms and
should be linked from the README as the project's stated position.

---

## 5. Extraction at build time — the pattern

For anything genuinely needed at runtime that we may not ship: **do not ship it,
and do not download it. Require the user to point the build at their own copy of
the vendor driver, and extract from that.** This is how `linux-firmware`-less
drivers, `b43-fwcutter`, `broadcom-wl`, and the historical `ipw2100`/`ndiswrapper`
packaging all handled the same problem, and it is what T2's own package system
expects for licence-restricted sources.

### 5.1 Shape

1. `configure` gains `--with-vendor-driver=DIR`, default unset.
2. Unset ⇒ the vendor-derived pieces are **not built**, and the features that
   need them are compiled out with a clear `configure` summary line. The driver
   must be fully functional for 2D/modeset/DDC/Xv without it. This is the
   default a distribution packages.
3. Set ⇒ the build runs `tools/parhelia-extract-ucode` against
   `$DIR/kernel/mtx_parhelia.o` and generates the headers into the *build*
   directory as `BUILT_SOURCES`, never into `$(srcdir)`.
4. Generated headers are in `.gitignore` and in `CLEANFILES`, never in
   `EXTRA_DIST`. `make distcheck` must produce a tarball with none of them.
5. `configure` verifies what it found — symbol size and a checksum — and fails
   with a useful message rather than silently producing a driver that uploads
   garbage to the engine.

### 5.2 Concretely

`tools/parhelia-extract-ucode` already does the whole job and resolves symbols
through the section header table rather than hard-coded offsets:

```
parhelia-extract-ucode mtx_parhelia.o -s mtx2785 -c -n warp_ucode  -o warp_ucode.h
parhelia-extract-ucode mtx_parhelia.o -s mtx3605 -c -n warp_prog_mtx3605 -o ...
```

The boot burst needs seven invocations (`mtx3605`, `mtx3694`, `mtx1223`,
`mtx1739`, `mtx458`, `mtx3213`, `mtx2068`) plus a generated descriptor table.
The missing piece is a small wrapper — a `-s sym1,sym2,...` mode or a shell
script emitting the `struct warp_program[]` — that regenerates today's
`warp_programs.h` exactly. That wrapper is our code and ships.

### 5.3 What the build must never do

- Fetch the vendor `.run`/`.exe` over the network as part of `make`.
- Embed a copy "just for CI".
- Silently fall back to a checked-in header if extraction fails.

### 5.4 The VBIOS is a different, easier case

The ROM is on the user's own card. `vbios.py` and the PInS parser should read
`/sys/bus/pci/devices/<dev>/rom` (`echo 1 > rom` first) or take a path. Nothing
needs shipping and nothing needs a vendor package. Document the two-line dump
recipe in the README and delete both ROM images from the tree.

### 5.5 The honest caveat

Extraction-at-build-time makes *our* distribution clean. It does not make the
user's resulting binary redistributable — a driver built `--with-vendor-driver`
contains Matrox code and that build must not be handed on. Say so in the
`configure` output and in the README, in one sentence, rather than leaving it
implied. This is precisely why §4 (rewrite the library) remains the real fix and
§5 is the bridge.

---

## 6. Documentation: is anything a transcription?

Checked all 40 files in `docs/` mechanically — longest run of consecutive
data-dense lines, and longest run of consecutive disassembly-style lines, per
file. **Result: nothing in `docs/` reads as a transcription.**

- Longest contiguous quoted-disassembly block anywhere: **8 lines**
  (`docs/parhelia-raster-clip.md:331`, `docs/parhelia-vertex-isa.md:561`).
  Both are short annotated excerpts used to prove a specific claim — the
  viewport z scale/bias formula, and the 12-byte instruction stride — each with
  more commentary than code around it. That is a citation, and the correct way
  to write this kind of document.
- Longest data-dense run: **16 lines** (`docs/parhelia-gl-limits.md:778`), and
  it is a markdown table of extension→evidence rows, not a data dump. Same for
  `docs/parhelia-state-slot-map.md:375` (a pointer-slot table) and
  `docs/parhelia-fragment-isa.md:1128` (a D3DSIO opcode→handler table).
- No file contains a microcode image, a fragment listing, or a bulk register
  dump.

Two things worth watching rather than fixing:

1. The docs quote vendor `.text` **addresses** heavily (`0x146aa7`, `mtx2179`
   @ `.text+0xa2400`, …). Addresses are facts about a binary and carry no
   expressive content — this is fine, and it is also what makes the work
   auditable. Keep it.
2. `docs/parhelia-fragment-isa.md` (104 KB) and `docs/parhelia-warp-codegen.md`
   (95 KB) are the densest. Both are structural descriptions with short quoted
   snippets. If either later grows a "here is the full fragment listing"
   appendix, that would cross the line — the rule to hold is that a doc
   describes the *encoding* and never contains a complete vendor program.

I am reasonably confident here, but the check was mechanical (run-length over
line shape). If a maintainer wants certainty on the two large ISA docs, a read
of their code fences is a half-hour job.

---

## 7. Header audit — `src/` and `tools/`

Convention: `SPDX-License-Identifier: GPL-2.0` plus
`Copyright (c) 2026 Rene Rebe <rene@exactco.de>`.

### 7.1 Missing the copyright line entirely

| file | has SPDX | note |
|---|---|---|
| `src/parhelia_driver.c` | yes | the main driver file — add |
| `src/parhelia_exa.c` | yes | add |
| `src/parhelia_pll.c` | yes | add |
| `tools/vbios.py` | no | no notice of any kind — add both |
| `tools/dma-ring-test.sh` | no | no notice of any kind — add both |
| `tools/warp_ucode.h` | no | moot — file is being deleted (§1) |

### 7.2 Wrong or stale notice

`Copyright 2024 the xf86-video-parhelia authors` — skeleton boilerplate, not a
real attribution:

- `Makefile.am`, `src/Makefile.am`, `configure.ac` — **also carry an MIT
  permission grant**, contradicting the GPL-2 `LICENSE`. Fix both at once.
- `man/Makefile.am` — carries the 2024 line *and* the 2026 René Rebe line; drop
  the stale one.
- `tools/parhelia-gears.c`, `tools/parhelia-tri.c` — `Copyright 2024 the
  xf86-video-parhelia authors. GPL-2.0.` Licence is right, attribution is
  boilerplate; replace with the real name.

`tools/warp_programs.h` — `Copyright (c) 2026 Rene Rebe` + "Licensed under the
GNU General Public License version 2" applied to 2868 bytes of Matrox object
code. This is the single worst header in the tree: it is not merely a missing
notice, it purports to licence someone else's code. It goes with the file.

### 7.3 Missing SPDX tag but licensed in prose

Correctly attributed and GPL-2 in prose, just no machine-readable tag. Low
priority, worth a sweep:

`tools/parhelia-bankb.c`, `tools/parhelia-cursorcap.c`,
`tools/parhelia-drawcount.c`, `tools/parhelia-glsweep.c`,
`tools/parhelia-texsweep.c`, `tools/parhelia-mtxtrace.c`,
`tools/parhelia-replay.c`, and the four Python tools
(`parhelia-warpdis.py`, `parhelia-warpreg.py`, `parhelia-fragdis.py`,
`parhelia-glsweep-decode.py` — these carry the full GPL-2 prose block, which is
fine, but an SPDX line makes them greppable).

### 7.4 Correct as-is

`src/parhelia.h`, `parhelia_reg.h`, `parhelia_cursor.c`, `parhelia_ddc.c`,
`parhelia_mode.c`, `parhelia_pins.c`, `parhelia_randr.c`, `parhelia_video.c`,
`parhelia_xaa.c`, `parhelia_m9_dp.c`, `parhelia_m9_exa.c`, `parhelia_m9_ring.c`,
`parhelia_m9_xaa.c`, `tools/parhelia-3dtri.c`,
`tools/parhelia-extract-ucode.c`, `tools/dma32buf/dma32buf.c`.

---

## 8. Release checklist

```
[ ] git rm tools/warp_ucode.h tools/warp_programs.h tools/p690.rom
[ ] git rm captures/warp-programs/*.bin  (and rmdir the directory)
[ ] git rm -r --cached tools/__pycache__
[ ] .gitignore += vbios-matrox-parhelia, *.rom, __pycache__/, *.pyc
[ ] Makefile.am: EXTRA_DIST -> explicit file list, no bare "tools"
[ ] configure.ac + all Makefile.am: MIT notice -> GPL-2 header
[ ] README.md: "MIT" -> GPL-2; add a "Vendor material" section (§5.5, §2.4)
[ ] tools/parhelia-3dtri.c: build the WARP paths only when the generated
    headers exist; do not fail the build without them
[ ] tools/vbios.py: drop default="vbios-matrox-parhelia"; document the
    /sys/.../rom dump instead
[ ] src/parhelia_pins.c:15, src/parhelia_pll.c:107: cite the ROM version
    string, not tools/p690.rom
[ ] configure --with-vendor-driver=DIR + BUILT_SOURCES extraction (§5)
[ ] add copyright lines: parhelia_driver.c, parhelia_exa.c, parhelia_pll.c,
    vbios.py, dma-ring-test.sh
[ ] drop "Copyright 2024 the xf86-video-parhelia authors" everywhere
[ ] SPDX sweep over tools/ (§7.3)
[ ] decide: filter-repo the four blob paths out of history, or don't (§2.6)
[ ] make distcheck; then: tar tzf the tarball and confirm no .rom, no
    warp_*.h, no *.bin, no vendor directory
```

The last line is the one that actually proves the job is done. Everything above
it is how you get there.

---

## 9. Where I am not certain

Stated plainly, so nobody reads confidence into these:

- **`src/parhelia_m9_dp.c` transmitter tables** (§3). Small verbatim vendor
  data, strong interoperability argument, but it is a copy. I recommend
  re-deriving from a hardware I2C capture rather than arguing the point.
- **`captures/cmdbuf-draw-0528-*.bin`** (§3). I verified they contain no vendor
  microcode. Whether a 139 KB dump of a vendor process's command buffer is
  purely factual is a judgement; I read it as factual, and it is sparse enough
  (≈1 KB of non-zero bytes) that the question is mostly academic.
- **History rewrite** (§2.6). A packaging/process decision, not a technical one.
- **Doc transcription check** (§6). Mechanical, not a line-by-line read of every
  code fence in the two 100 KB ISA documents.
- Everything in this file is an engineering assessment. If the project is going
  to be carried by a distribution, the distribution's own licence review is the
  one that counts.
