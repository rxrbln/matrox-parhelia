# Parhelia — hardware capture list

The handful of values that could not be pinned by static reverse engineering
(they are produced at draw time by the proprietary driver's thread-local
dispatch, or the static defaults are ambiguous). Each is resolved by one small
MMIO register capture on real hardware while the proprietary `libGL` / X driver
draws a known primitive. This consolidates the "capture item" notes scattered
across the other docs.

**How to capture:** map BAR1 read-only (sysfs, as `tools/parhelia-tri.c` does),
poll/dump the listed registers immediately after issuing the named GL call
through the proprietary stack, and diff against the reset/default values noted.
A simple approach is an `LD_PRELOAD` shim that snapshots the register window
after each `glDrawArrays`/`glClear`.

### Resolved statically (no capture needed)

- **Vertex diffuse colour order = `[a,b,g,r]`** (0x1184 latch = red) — proven via
  the plane reversal rule (position `[w,z,y,x]`, texcoord `[q,r,t,s]`).
- **WARP opcode is always 0xc07** — there is no 6-vs-7 runtime choice; shade
  model is a separate register (0x1024). Opcode-6's hardware meaning
  (list vs strip/reuse) is moot: the driver decomposes to independent triangles.
- **Depth z ∈ [0,1]** for pre-transformed (RHW, w=1) vertices; hardware scales to
  the 24-bit S8Z24 range. (If depth misbehaves on hw, the one alternative is
  z × 16777215.)
- **VS source negate = bit 24** (`0x01000000`); **VS temp bank = bit 30**
  (`0x40000000`); banks are one-hot in [30:28].

### Remaining capture items

| # | Unknown | Register(s) to watch | Trigger | Hint |
|---|---|---|---|---|
| 1 | S8Z24 depth format enum | 0x1234 | bind a 24/8 depth-stencil visual | small code (cf. colour=3) |
| 2 | Fragment op → 0x126c opcode-base (per op) | 0x126c bits[low] | one fragment shader per op (MUL/MAD/DP3…) | done in the kernel module; also readable there |
| 3 | Blend-factor bank (key 0x5) absolute base | contiguous 4 offsets | `glBlendFunc(SRC_ALPHA, 1-SRC_ALPHA)` | receives codes 0x05 / 0x06 |
| 4 | Texture wrap/clamp mode field | 0x11c8–0x11d8 | `glTexParameteri(GL_REPEAT)` vs CLAMP | not in the scaler path |
| 5 | VS declaration-stream (dcl/def) entry layout | key=0x45/0x46 blob | upload a known shader | second token vector |
| 6 | DWGCTL bits[19:14] per-draw flags | 0x1300 | multitexture / specular draws | tex/spec enables |

(The fragment **iterated-primary-colour input selector**, previously a capture
item, is now **pinned to 0x10** statically — see `parhelia-fragment-isa.md`. #2
above is the remaining fragment-shader piece: the concrete opcode-base bits that
the *kernel* module packs into 0x126c for each of the 11 ALU ops — readable by
disassembling that module or by one capture per op.)

#3 (blend bank base) and the T&L constant-RAM base sit behind the ICD's
thread-local dispatch and are expected to stay capture-only — though a driver
rarely needs the constant-RAM base because uniforms are slot-indexed.

None of these block a first driver: a CPU-transform, flat/Gouraud, depth-tested,
alpha/blended driver is fully specified without them; they refine correctness
(exact colour channel, shading mode, depth precision) and unlock texturing wrap
modes and the fragment combiner.
