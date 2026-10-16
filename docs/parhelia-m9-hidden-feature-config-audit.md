# M9 hidden features and Direct3D implementation boundary

This audit separates features which the M9/Phoenix Windows driver can actually
pack or emit from configuration vocabulary and from older Parhelia/P-series
paths.  Addresses are virtual addresses in the saved 32-bit Windows images and
file/text addresses in the Linux ELF image.  `[BIN]` means direct binary data
flow, `[CAP]` a saved command capture, and `[DER]` a conclusion from joined
observations.

Run the deterministic audit with:

```sh
python3 tools/m9-hidden-feature-evidence.py
python3 tools/m9-hierarchical-z-evidence.py
```

The verifier pins these images and the complete Windows configuration
constructor:

| image | SHA-256 |
|---|---|
| `MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| `MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |
| Linux 1.15 `m9x_drv.so` | `15ff4ad6e450c2d5a27c3d114011014e8aaf97851d107a070f31baa26eafc7e5` |

## Driver exposure policy

Four different claims must remain separate:

1. **Hardware-backed** means a reachable M9 packer, emitter, or captured packet
   exists.  This is sufficient to implement an experimental internal path.
2. **Live-supported** additionally means the vendor M9148 Linux context
   advertised and accepted the public feature.  The authoritative inventory is
   `captures/m9x-linux-gl2-context-20260914/glxinfo-l.txt`.
3. **Dormant** means the Windows binary has a format or state path which the
   live Linux ICD did not expose, or a configuration read whose consumer is not
   joined.  It must stay behind a capability gate until a result capture passes.
4. **Unsafe** means applying the known control without its allocation, resolve,
   synchronization, or display contract could corrupt memory, hang the engine,
   or disrupt scanout.  A registry name alone never authorizes emission.

An implementation should expose a Gallium/OpenGL capability only when its
format, command path, resource lifetime, and result semantics all meet the
corresponding API contract.  Callable hardware is evidence for implementation,
not evidence that the vendor supported every public combination.

## OpenGL 2 and vendor-path audit result

The canonical feature inventory was compared against the saved Windows D3D/GL
backend and Linux Xorg/GL monolith in the following areas.  The only
previously unclassified final hardware path found in this pass was
hierarchical Z; its exact closure is in
`parhelia-m9-hierarchical-z-static-closure.md` [BIN][DER].

| audit area | canonical evidence already present | result of this pass |
|---|---|---|
| integer, 10-bit, float, depth, YUV and DXT formats | format operation matrix, sRGB/format closure, texture resource and tail evidence | No unlisted format family found. Per-format runtime exposure gates remain unchanged. |
| raster and fragment state | blend/raster lowering, fixed-state capture, render-surface/depth packers | Added the missing `HZFlags -> selector 12 -> ba/bb` performance path. |
| shader ISA, compiler and limits | SM2/3 audit, GL2 compiler/control-flow/result captures, linked-pair construction and placement | No extra public limit follows from registry `PixelShader`, `VertexShader`, or `ShaderOpt`; opaque pair serialization remains the compiler boundary. |
| multisample/FAA and queries | GigaColor/FAA closure and occlusion-query closure | Allocation/resolve and result-semantics validation gates remain; HZ is independent of FAA. |
| gamma and sRGB | cursor/gamma and sRGB/format closures | Display LUT gamma remains a KMS/output property; render sRGB remains a format/program property. No shared GL gamma state was found. |
| video and overlay | configuration audit and KMS/display boundary | Kept outside the render-node contract until plane, pitch, and synchronization behavior is proved. |
| blocked layout, copies and coherency | backing-layout, blocked-layout, allocation-mode and texture-coherency closures | No new manufacturable tiled layout follows from the extra config names. Existing physical-page validation boundary remains. |
| caches, bus mastering, DMA and submission | DMA/requester/rollover/context documents | HZ is a concrete depth-rejection cache. Other tuning names remain software configuration inputs unless a final register or allocation consumer is independently joined. |

## Exact hidden-feature classification

| feature | M9 evidence and final path | classification and driver action |
|---|---|---|
| 10-bit GigaColor | `MxMD` maps `A2B10G10R10` and `A2R10G10B10` to unique codes `0x68/0x6a`, exact channel masks, sampler/RT construction, rounded native D3DCOLOR clear conversion, and baseline operations for 2-D/cube/volume, RT, sRGB read/write, autogen and vertex texture. Linux reads `3D\GigaColorFullScreen` and `3D\GigaColorRender` at `0x330a6f/0x330ac9` into config `+0x268/+0x270` [BIN]. | **Hardware-backed, dormant.** Add formats only after native draw/blend/sample/backing/readback. Scanout, LUT and dither need separate depth-30 validation. |
| GigaColor presentation | `MxMGLX` diagnostic at file `0x7f15c5` says page flipping is unsupported with GigaColor [BIN]. | **Presentation restriction.** Do not infer that a 10-bit renderable is flippable. Blit/scanout conversion and LUT mode are separate. |
| FAA/multisample | `AAMode=0x80000842` stores internal type 8 and reports eight quality levels; the table constructor rewrites eligible rows to nonmaskable, while one GetDriverInfo record path recomputes type 8. Compiler/lowerer/emitter reaches four repeated `ad` writes plus `af`; `ad[20:0]=min(allocation_bytes>>5,0xfffff)`, exact `af` sources are pinned, and zero mask suppresses output [BIN]. | **Hardware-backed, unsafe.** Live FBConfigs have zero sample buffers. Record the query branch consumed by the runtime and capture allocation growth, fragment layout, patterns, nonzero masks/invert, resolve, present and query scaling. |
| hierarchical Z | `3D\HZFlags` defaults to 7 in both saved stacks. Windows gates 16/32/64-bit depth resources with bits 0/1/2, allocates format-`0x0b` metadata at `ceil(width/8) x ceil(height/4)`, initializes/conservatively resets it with `0x007ff000`, seeds complete clears, invalidates all metadata for partial or uncoupled writes, gives ownership to the creator, tracks parent and auxiliary together, and binds selector 12 through surface shadow `+0x74/+0x78/+0x7c` to final `ba/bb` [BIN][DER]. | **Hardware-backed performance feature.** Static lifecycle and maintenance are closed. Keep a null-`ba/bb` fallback and require a bounded correctness test before default enablement; ordinary depth remains direct hardware `b8/b9`. |
| sRGB render writes | D3DRS 194 handler `0x1037e7` sets frontend `+0xf4 bit11`; dirty `0x10000`. Compiler gate `0x137242` is output-zero-only and inserts an exact 25-slot native suffix at `0x174ea0`: 15 direct literal slots plus two five-slot helpers. Clear `0x13e0f0` independently converts RT0 RGB when surface-set `+0xf4 bit0x800` is set [BIN][DER]. | **Hardware-backed, dormant.** Replay/capture the complete suffix and validate blending and pre-LUT BO values before exposure. |
| sRGB texture sampling | The baseline format table advertises `SRGBREAD`. Mapped D3D9 state 29 reaches both sampler dispatchers, stores sampler `+0x46` bit 5, and survives state blocks/query normalization. Neither pinned final native lowerer consumes bit 5. The unreachable state-11 fallback remains a useful negative control because live state 11 is `TEXCOORDINDEX`, not sRGB [BIN][DER]. | **Accepted frontend state; execution unresolved.** Keep sampler-side sRGB caps off until a later derived-record consumer or explicit shader conversion is joined and result-validated. |
| floating-point textures | Descriptor initializer `0x4fe00` and selector-2 builder `0x467e0` produce `R16F=0x54`, `G16R16F=0x74`, `A16B16G16R16F=0x3c`, `R32F=0x75`, `G32R32F=0x3b`, `A32B32G32R32F=0x34` at index `78[31:25]` [BIN]. | **Hardware-backed, dormant.** The live ICD has no texture-float extension. Upload/sample captures must establish filtering and renderability per format. |
| YUV textures/video | `UYVY` and `YUY2` map to internal IDs `0x43/0x44` and descriptor code `0x58`. `DdVPE*`, `DxVaSupport`, `VPEEnable`, overlay, and field-flip configuration paths exist [BIN]. | **Hardware-backed media path, dormant for GL.** Do not expose YUV Gallium formats until plane order, chroma siting, pitch, and sampler conversion are measured. |
| DXT texture compression | The packer maps DXT1 to `0x10`, DXT2/3 to `0x15`, and DXT4/5 to `0x17`; block geometry and mip-tail offsets are exact [BIN]. | **Hardware-backed, dormant.** The live Linux ICD does not advertise S3TC. Test DXT1 and DXT5 sampling and edge blocks before enabling. |
| anisotropic filtering | Sampler lowerer maps requested levels 1..3/4..7/8..15/16 to index `7c[17:16]` values 0/1/2/3 [BIN]. | **Hardware-backed, dormant.** Live max anisotropy is zero and no extension is advertised; validate footprints and LOD interaction first. |
| high-quality cube maps | Windows reads `3D\HqCubemaps` at `0xb9251`, default zero, into config `+0x44`; Linux reads it at `0x32f3b0` into config `+0x90`. Cube resource construction and sampler dimension paths are real [BIN]. | **Dormant and unjoined.** No use of either config field has been joined to a final sampler bit or program key. |
| occlusion query | `3D\Queries` defaults to 3 at `0xb9386`; GL create/begin/end route through MxMD `0x119650/0x1198b0`. `0xfc4b0` emits the relocation-backed five-dword snapshot body and kind 9 returns unsigned `end0-begin0` [BIN]. | **Hardware-backed, experimental.** Validate visible counts, readiness, slot reuse/wrap, second word, and MSAA interaction before default exposure. |
| point rasterization | Topology and generated stream expansion are captured. Point fill reaches final `c1[11:0]`; fixed point extent reaches the M9 program/setup path [BIN][CAP]. | **Hardware-backed.** Unit points are implementable; point sprites and attenuation still need raster-result validation. |
| wide/smooth/stippled lines | Windows reads `OGL\WideLines`, `OGL\SmoothWideLines`, and `OGL\LineStipple`, all default zero. Wide point/line extent machinery is known, but the M9 final line mode and stipple packet are not fully isolated [BIN]. | **Partially backed, dormant.** Keep non-unit/smooth/stipple line caps gated pending command/result deltas. |
| stereo and overlays | Windows reads `OGL\Overlay`, `OGL\Stereo`, `OGL\StereoDisplayMode` and DirectX overlay controls. WGL diagnostics disable page flipping for overlay PFDs and manage separate layered/right buffers [BIN]. | **Presentation subsystem only, unsafe in render driver.** Implement through proven KMS planes/page-flip/vblank policy, not guessed 3D words. |
| ordinary context switch | TaskMgr `0xf8fd0` serializes outgoing state, installs incoming context at `+0x5b8`, and restores through the same ordered command stream; resources carry submission fences [BIN]. | **Hardware-backed.** This is the required context-isolation path. |
| fast context switching | No `FastContextSwitching` string exists in any pinned M9 image. TaskMgr `+0x5bc` has no live alternate-context producer [BIN]. | **Absent on this M9 stack.** Do not import the classic Parhelia option or invent an A/B slot. |
| fast trilinear / multiple texture pipes | `TEFastTrilinear` and `TEMultiplePipes` are absent from all pinned M9 images. M9 instead has a concrete four-unit sampler emitter `0xf4350` and normal min/mag/mip controls [BIN]. | **Legacy P-series names, not M9 controls.** Optimize batching and the known sampler path without copying old register meanings. |
| volumetric shadow | `VolumetricShadow` is absent from all pinned M9 images. Ordinary depth-texture compare is separately joined to sampler state and index `7c`/program lowering [BIN]. | **Legacy name absent.** Implement API shadow compare from the M9 sampler/program evidence only. |
| texture compression switch | `TextureCompress` is absent from all pinned M9 images even though DXT descriptor paths exist [BIN]. | **Legacy knob absent; formats dormant.** Capability follows validated formats, not this old option. |
| cache and bus-master tuning names | `IndexBufferCacheSize`, `BmBuffers`, `BmCacheFifoSize`, `BmRequestSize`, `TlbPrefCnt`, and `LockCachedPointerInUserMem` have exact constructor rows and defaults [BIN]. | **Configuration inputs, not register definitions.** Do not translate their numeric defaults into MMIO. Use the separately closed requester, fence, relocation, and rollover contracts. |

## Comprehensive Direct3D path map

The following is the call-reachable M9 D3D implementation surface. It is a map
for the new driver, not a request to reproduce the XPDM ABI.

| domain | reachable Windows path | exact product / remaining boundary |
|---|---|---|
| D3D callback and parser | `DdGetDriverInfo 0x3db50`; callback builder `0x77130`; `D3dDrawPrimitives2 0xbeeb0 -> 0xddf20 -> 0xde020 -> 0xde200`; 256-op table `0x20c9a0` [BIN] | Real DP2 frontend. The old pointer-bearing ABI must not cross the DRM boundary. |
| render state | dispatcher `0x103370`; depth/stencil handlers `0x10360b..0x103a43`; blend/alpha/sRGB handlers `0x10367c..0x103829`; raster handlers `0x10405d/0x1040c9/0x104172` [BIN] | Exact frontend shadows and dirty groups. Final depth/stencil `a8..aa`, MRT masks, multisample `ad/af`; some fill/cull/shade effects are compiled into native state. |
| sampler state | fixed dispatcher `0x105850`, programmable mirror `0x105cf0`, lowerers `0x1366f0/0x18ba27..0x18c22b`, emitter `0xf4350` [BIN] | Four units selected by dynamic index `70+unit`; texture address/descriptors `78..7a`, filter/wrap/aniso `7c`, LOD `7d`, border `7e..81`, bump `82..85`, reserved `87`. D3D9 sampler state 29 stores sRGB-read enable in sampler `+0x46` bit 5 in both fixed and programmable paths and survives state blocks/query normalization, but no consumer of that bit is present in the pinned final native lowerers. |
| texture-stage state | parser `0xd0770` calls `0x105340`; selector builder `0x187950` joins operation/resource dimension [BIN] | Fixed combine and dimension selection feed program compilation. Special shader-side texture forms still require native-slot captures. |
| texture formats/layout | public mapper `0x4ee30`, 86-entry initializer `0x4fe00`, layout constructor `0x45eb0`, descriptor builder `0x467e0`, volume constructor `0x531e0`, and exact 52-row format-operation template `0x20a008` [BIN] | Common integer, 10-bit, float, vector, luminance, depth, YUV, and DXT codes plus ordinary mip recurrence and packed tail. The operation template pins 2-D/cube/volume, RT, sRGB, autogen and vertex-texture declarations per format; runtime policy can only remove bits. All 35 class-3 calculator routes and all 14 non-32-bit aperture formulas are exact; only live table-root/page ownership and physical-page validation remain. |
| vertex streams/indices | DP2 SETSTREAMSOURCE `0xe0ea0`, user-memory `0xe01c0`, SETINDICES `0xe0da0`; final draw handlers `0xe02d0/0xe04f0/0xe0610`; topology helper `0x108780` [BIN] | Sixteen streams and 16/32-bit indexed draw paths. New Mesa may CPU-repack to the captured canonical layout first. |
| vertex declarations | CREATEVERTEXSHADER `0xdfc20`; parser `0x10f060 -> 0x10b690`; descriptor append `0xa9820` [BIN] | Exact classic declaration token classes, formats, offsets, and register mapping. |
| shader frontend/compiler | Iris parser/compiler documented from `0x1a4c90` dispatch and special lowerers; linker `0x13bf80`; native upload `0xf74d0`; program/vector emitters `0xf6f20/0xf78c0` [BIN] | Matrix/ALU/DDX/DDY lowering, the seven-mask KIL basis, nine-slot TEXLDD expansion, ten texture metadata expansions, eight shadow comparisons, ordered descriptor substitution and graph-builder records are exact. Private pair allocation, key reuse, reclamation and COW publication are exact. The opaque pair byte serializer, B/P/L linked resources, semantic branch placement, unrecorded predicates, output routes and hardware-stop semantics remain gates for arbitrary GL2. |
| resource/control bind | final indices `69/6a/6b/6c`; live `69` binds the external fixed-fragment database, with address translation unresolved, while `6c` selects a separate aligned record in the 0x21000-byte system allocation and retains low tag bit 0; the alternative inline path uses selector `68` and data ports `60..67` [BIN][CAP][DER] | Use separately validated, typed relocations for resources `69` and `6c`; never copy captured GPU addresses or interpret the old address-directed BAR snapshot as a linear view. `6c` atomically selects an opaque linked VS/FS pair and exposes no universal native start/count or per-stage entry schema. |
| constants | GL uniform updater `0x100b9f70`; common Windows updater `0x10559510`; private opcodes `0x65` vertex and `0x66` fragment through `0x100a05e0` [BIN] | Exact 16-byte range records, per-stage arrays, FIFO drain order, and resource transport. Inline mode-2 bank remains optional/unjoined. |
| MRT/FBO | declaration lowerer `0x181fa0`; surface emitter `0xfb920`; four color pairs `b0/b1` through `b6/b7`; per-target masks and native blend fragments [BIN]. Saved FBO producer/sample chain closes normal dependency ordering [CAP]. | Four linear attachments are structurally closed. A four-unique-output result capture is still required before advertising MRT broadly. |
| depth/stencil | lowerer `0x137cf0..0x138292`; final `a8` control, `a9/aa` face ref/masks, address/descriptor `b8/b9`; `HZFlags` allocator `0x54be0`, lifecycle/maintenance `0x54e80..0x5553f`, and selector-12 binder `0xaaa7d` identify and maintain `ba/bb` as hierarchical-Z metadata; clear `0x13e4c0 -> 0xb4fe0` [BIN][CAP] | Depth enable/write/compare, two-sided stencil, and the complete static HZ protocol are exact. Validate each packed depth/stencil format and HZ rejection behavior on hardware before default enablement. |
| clear | parser flag dispatch `0x13de20`; color `0x13e0f0`; depth/stencil `0x13e4c0`; fast whole allocation `0xb5e70`; rectangle draw `0xb5030`; CPU fallback `0xb5a80` [BIN] | Use only the path whose layout, bounds, masks, and synchronization are proved. |
| queries | DP2 `0xd1032..0xd1091`; interface methods `0x119650/0x119720/0x1198b0`; snapshot builder `0x144b40`; packet leaf `0xfc4b0`; result `0x1199a0` [BIN] | Exact experimental occlusion protocol; see validation gates above. |
| resource copy/blit | copy entry `0xb21b0`; packed-tail adjusters `0x56a10/0x56c60`; GPU task `0x1b8d0 -> 0x1b650`; source/destination relocation builder `0x1b420` [BIN] | Hardware copy preflights both resources against existing fences. Layout formulas are exact; physical-page validation and copy-domain visibility remain gated by the live table-root/page join. |
| present/vblank | `3D\FlipOnVBlank`, `OGL\VSyncEvent`, `OGL\TearFree`, `OGL\IndependentBackBuffers`; WGL page-flip eligibility diagnostics [BIN] | Presentation policy belongs to winsys/KMS. It does not prove a 3D resolve or a render format. |
| overlay/video | `DdVPE*`, `DxVaSupport`, `VPEEnable`, `OverlayEnable`, field-flip and auto-flip settings; miniport display/overlay services [BIN] | Separate media/display subsystem; no safe render-node API follows from the config keys. |
| state ordering/coherency | top serializer `0xfadf0` orders late, surfaces `0xfb920`, common `0xf4f50`, programs `0xf6f20`, inputs `0xfb260`; TaskMgr fences references before reuse [BIN] | Emit producer before consumer, preserve BO access classes, and prohibit simultaneous render/write sampling feedback. |
| requester/submission | userspace publish `0x1afd0` writes requester `0x504`; synchronous drain `0x1ac40`; kernel doorbell path `m9x1039` writes `0x404`; wrap `m9x1200` [BIN] | Kernel owns validation, typed relocations, wrap, bounded waits, and fences. Vendor raw-pointer ioctls are unsuitable. |
| IRQ | Windows ISR `0x10875`; queue selectors 9/10, selector 10 can repost saved endpoint to `0x404`; Linux `m9x547` acknowledges banks [BIN] | Repost/completion and display events are joined; there is no proven 3D fault IRQ or transparent reset. |
| context | TaskMgr switch `0xf8fd0`, state method `0xf85c0`, submission/lifetime `0xf9b70`; Linux resource retirement `m9x954` [BIN] | Serialized context isolation and fence retirement are implementable. No fast alternate-context switch is proved. |
| reset/recovery | miniport `HwResetHw 0x10505` restores display only; high-level ICD `Restore 0x1002dfe0` rebuilds user resources after external recovery [BIN] | No callable M9 requester/3D reset sequence exists in saved binaries. Kernel recovery must fail closed after bounded quiesce and report device loss. |

## Configuration namespace

### Windows MxMD

Constructor `0xb8ea0..0xbace7` reads the following exact section/name entries.
The number after `@` is the configuration-object byte offset and the value
after `=` is the default. These fields are software configuration, not M9
register offsets [BIN]. The verifier emits the same 96 rows as JSON.

```text
3D: ForceAGPTextures@000=0 AGPSize@004=-1 SurfaceLocation@010=0x80
    AAMode@02c=0x80000842 AllowMultipass@030=0 CEAvailable@034=0
    ForceHPVertexCopy@03c=0 FragSize@040=0x40c00000 HqCubemaps@044=0
    HZFlags@048=7 IndexBufferCacheSize@04c=0x10000 MatchRefRastBugs@050=3
    Queries@054=3 ShaderOpt@058=0x23 TEEnable@05c=0
    TEFilteringOptions@060=0x04ffffff TELodBias@064=0 TempBufferSize@068=0x400
    DdBlitSoft@014=0 DdVPEControl@018=0 DdVPEStripWidth@01c=0
    DdVPEGaussSigma@020=0 FlipOnVBlank@160=3
DirectX: SwapTexturesVidMemOptions@008=0 D3DEnable@06c=0 FeatureControl@070=0x82
    DisableClipTLVerts@074=0 GuardBandClipping@07c=0 HwPureDevice@080=0
    HwTransformLight@084=0 PixelShader@088=0x300 VertexBuffers@08c=0
    VertexShader@090=0x300 DdMapMemoryOptimization@128=-1 DdFlags@12c=0x8007
    DxVaSupport@150=1 DxVaSupportIndicator@154=0
    AvoidScanLineBlitStrategy@164=1 OverlayEnable@168=1 WaitOnFieldFlip@16c=1
    OverlayAutoFlipEnable@170=0 VPEEnable@174=0 VideoAnalyst@178=0
    LockCachedPointerInUserMem@180=1 DRMErrorOSD@184=0
OGL: TripleHead@094=0 PolygonOffset@098=0 WideLines@09c=0
    SmoothWideLines@0a0=0 LineStipple@0a4=0 TextureOptions@0a8=0
    Debug@0ac=0 Overlay@0b0=0 Stereo@0b4=0 StereoDisplayMode@0b8=0
    VSyncEvent@0bc=0 MultiMonDrawPixels@0c0=0 MultiMonitor@0c4=0
    AlternateVendor@0c8=0 Version@0cc=0x200 ForceWindowIntensityInCD@0d0=0
    DisplayListMode@0d4=0x20000000 IndependentBackBuffers@0d8=0
    HwTransformLight@0dc=0 L8_3D_L16_3D@0e0=0 DisableUCPEdges@0e4=0
    TearFree@0e8=0 Multisample@0ec=0 TLSOpt@0f0=0
    OffScreenCopyDisabled@0f4=0 TextureMaxSizePower2@0f8=0
System: MmMaxApertureSize@028=0 PhysicalMemory@00c=0 DebugPatches@024=0
    BmDebugFlags@108=0x80 BmDebugInfo@10c=0 BmDbgOutLvl@110=0
    BmBuffers@114=0x08080100 BmCacheFifoSize@118=0x20182008
    BmRequestSize@11c=0x884 MmFlags@120=0x101f8 MmDebugFlags@124=0
    EAR30PSCTL2@130=0 EAR30PSSYNC@134=0 EAR98@138=0
    PsBmMicrocode@13c=0 PsBmConstants@140=0 CeBmMicrocode@144=0
    CeBmConstants@148=0 VSyncInt@14c=0 TlbPrefCnt@17c=1
DisplayDriver: PuntAll2d@0fc=0 Simulate2d@100=0 StretchBlt@104=0
    AlphaTextGamma@158=0x16 TextAntiAliasing@15c=1
```

The earlier zero entries for the BM microcode slots do not describe the
actual locked MxMD constructor defaults. The complete public RED source
trace observes PsBmMicrocode and CeBmMicrocode locked at1, with real
resource-program transport consumers; see
[the owner and consumer proof](parhelia-m9-faa-bm-program-options.md).
This does not authorize substituting unvalidated program bytes or constants.

### Linux M9 additions

The Linux 1.15 constructor has two ABI-cloned copies. In the checked copy,
section string `3D` is at file `0xe9dddc` [BIN]:

| path | read | object store | state |
|---|---:|---:|---|
| `3D\HqCubemaps` | `0x32f3b0` | `+0x90` | default/control read, final consumer unjoined |
| `3D\GigaColorFullScreen` | `0x330a6f` | `+0x268` | default zero, display policy |
| `3D\GigaColorRender` | `0x330ac9` | `+0x270` | default zero, render policy |

The old Parhelia names `VolumetricShadow`, `TEMultiplePipes`,
`TEFastTrilinear`, `TextureCompress`, and `FastContextSwitching` occur in none
of the three pinned M9 images. They must not be assigned M9 meanings from a
different architecture.

## Remaining GL2/static boundary after this audit

The static Windows paths now close MRT address/descriptor fan-out, FBO resource
ordering, depth/stencil fields, query packet/result arithmetic, common resource
layouts, and all ordinary sampler fields. They do not close these last pieces:

| gap | exact reason | minimal evidence |
|---|---|---|
| sampler-side sRGB | D3D9 state 29 is call-reachable for pixel and vertex samplers, stores sampler `+0x46` bit 5, survives state blocks and query normalization, and is distinct from state 11 `TEXCOORDINDEX`; neither pinned final native lowerer consumes the stored bit | keep disabled or implement explicit shader lowering; capture a visible off/on result and join the derived sampler record if a later binary path consumes it |
| four-target shader routing | four surface pairs and target compilers exist, but four distinct final colors have not been result-validated | one fragment shader writes unique sentinel colors to all four linear attachments, followed by readback |
| multisample/FAA | `ad[20:0]` allocation sizing is exact; reserved DP2 opcode 5 is the sole direct caller of the 1..31 auxiliary dispatcher, whose state 25 stores the float that resets to `1.0f` and packs as `af[8:0]=0x100`; its public API meaning and the sampled layout/resolve contract remain unknown | vendor multisample surface, auxiliary-state differential, edge pattern, partial mask, resolve, and occlusion count |
| arbitrary GL2 native code | ALU/matrix/derivative/KIL/TEXLDD/shadow packed forms, private pair allocation/cache/COW publication and external transport are exact; the opaque pair byte serializer, semantic control-flow placement, B/P/L linked resources, output routes, unrecorded predicates and hardware-stop semantics remain | construct one bounded new pair, then validate LOD/gradient/vertex-texture, predicate, branch/loop and output results through it |
| uncommon resource binds | exact format-operation masks establish the vendor API upper bound, while descriptor codes and advertisement do not prove precision or new-driver correctness | per-format upload/sample/render/copy matrix, with odd dimensions and mip tails |
| class-3 physical backing join | all calculator bodies, dispatches and non-32-bit aperture permutations are exact, but the captured index-`78` record is not joined to its live table root and owned pages | capture `request+0x90`, root, decoded PTEs and coordinate-coded physical pages together |
| recovery | neither kernel object nor miniport exposes a requester/3D reset leaf | controlled fault with complete pre-ack register/IRQ snapshot; remain fail-closed until then |

Everything else in this document is sufficient to write guarded code paths and
offline packet tests. Public capability enablement still depends on the result
tests stated in the policy table.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
