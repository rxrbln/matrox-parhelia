# Historical remaining-work snapshot — September 2026

Superseded by [the current audit](parhelia-hw-gl-feature-audit.md).

## Remaining work

1. **X/glamor rendering:** lower glamor's text (8-bit glyph atlas; A8/R8 are
   not supported texture formats) and tile programs, or the general
   fragment-program item 6 below.  Check the FBO-attachment readback failure.
2. **TTM moves and eviction:** VRAM objects can still only be moved by a CPU
   copy through BAR0, which does not reach their storage, so P690 render and
   texture objects stay resident and VRAM exhaustion is fatal instead of
   evicting.  `wip/kernel-vram-eviction-wip.patch` implements the move through
   the aperture.

   **Retested 2026-09-13, and the failure is now localised.** The patch still
   applies to this tree (one header hunk needs re-applying by hand, the
   `parhelia_submit_lock`/`unlock` and `parhelia_vram_*` declarations) and
   builds clean on top of the WARP cache-invalidate fix.

   * Transfers work repeatedly: `--transfer-probe` and
     `--native-transfer-probe` pass, and pass again after a draw.
   * `--clear` passes repeatedly.
   * **`--texture` wedges the engine** once other probes have run in the same
     boot - `submit timeout at CALL 23/23: BM=03000000 ... wedged` - and every
     later ioctl then returns `-EIO`. Straight after a module load it passes,
     which is why the original note called it "the first probe after load".
     It is not the first probe: it is an accumulated-state effect.
   * **`vram_evictions` and `vram_restores` are both 0 at the moment of the
     wedge.** No object was ever moved, so the aperture move code is not what
     breaks it. That exonerates `parhelia_vram_copy_resources()` and
     `parhelia_vram_move_pages()` and points at the other half of the patch:
     the `submit_lock` to `parhelia_submit_lock`/`submit_owner` refactor, which
     touches every submit path whether or not a move ever happens.

   So the bisect the original note left unfinished has a starting point now:
   apply only the locking refactor, without the move implementation, and see
   whether `--texture` still wedges. If it does, the fault is in the refactor
   and the move code may be sound as written.

   The kernel tree is left **without** the patch applied, and verified healthy
   (`--clear`, `--texture`, 53 self-tests) so nobody inherits a wedging
   driver; the patch stays in `wip/`.
3. **CPU mappings of legacy render BOs:** stop returning a raw `mmap_offset`
   from `CREATE_RENDER` for tiled render BOs and let the transfer ioctls accept
   them (the anchor path), then fix or retire the `render()`-based probes.
   The `LINEAR` companion BO mapping showed the same stale behaviour.
4. **Cursor base:** `parhelia_cursor.c` still programs a flat BAR address; a
   cursor BO in bank B needs the bank encoding like DC_START.
5. **Kernel validation:** `parhelia_validate_reglist()` checks register indices
   and relocations but not fill/blit geometry (`DSTXY`/`LEN` against the
   relocated BO), so a render-node client can write past its BO.
6. Fragment programs: the data-flow compiler is the default (see its
   section); measure CMP, BIAS/SIGNED, the result scale on intermediates and
   the intermediate range with `parhelia-gl-combiner-test` group 4.
7. Native WARP transform/lighting/fog/texgen, and hardware viewport,
   depth-range, clip planes and polygon offset (currently in Gallium's draw
   module; functionally correct).  Native line strips/loops and the hardware
   point-size registers would reduce CPU expansion.
8. Texture completeness: 1-D, 3-D, cube, S3TC, border/mirrored wrapping, LOD
   fields, four units, 10-bit sampling, FAA.
9. Sampler extent: resolved - the cap is 2048 and large images are exact; the
   512 wrap was the address probe's own clip window (Mesa `907e6fb`).
10. Bilinear filtering at 1:1 on a non-power-of-two width or an odd height
   misses one or two texels at the last column or row; every point-sampled
   case of the same shapes is exact, and the chain itself is byte exact, so
   this is a filter-edge question.
11. `PARHELIA_DC_CTRL_ON` is `0x00040201`, whose bit 9 is the primary-LUT
   bypass; the candidate is `0x00040001`, to verify on scanout.
9. The 64x32 (0x0528) tile formula probably has the same y-bit exchange as the
   corrected 64x16 one; unverified without that chip.
10. **System-RAM textures:** every texture is uploaded into the 128 MiB VRAM.
   There is no AGP/PCIe GART on the P690; PCI bus mastering (memory class 3)
   is proven only for vertex fetch.  Whether the texture unit can fetch a
   class-3 (bus-master) texture base, and at what fill rate, is untested.  It
   would allow on-demand/system-memory textures when VRAM is exhausted, but not
   higher frame rates: sampling is already bandwidth bound in local VRAM.
11. **Texture throughput:** the ganged pipe route is implemented and on by
    default; the remaining ceiling is memory bandwidth.  Q3 needs less
    traffic per pixel (clears cost ~3.8 ms/frame, the alpha two-pass ~4.5 ms)
    to approach 60 fps.
12. **Multi-primitive draw packets:** the P690 draws only the first primitive
    of a 0x97/0x8f range (4+ triangles or a strip hang); find the state that
    makes it consume whole ranges (the 0528 vendor captures use 24-vertex
    packets).  Per-packet cost is 0.4 us, a full state batch 17-34 us.
13. Make kernel `pack_adjacent_packets` and Mesa state-stream aggregation the
    default after an image check (4x lower per-batch cost in the probe).
14. Replace the two-pass/micro/CPU alpha routes with the native sampled-alpha
    path (22 two-pass submits per Q3 frame, ~4.5 ms).

Do not enable GigaColor 10-bit render/sampling or FAA by default from static
packet evidence.  Their saved vendor paths still require isolated target tests;
the current truthful caps reject those formats/sample counts.

