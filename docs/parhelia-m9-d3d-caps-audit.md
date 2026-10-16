# M9 Direct3D caps builders and masking

This is the M9/Phoenix caps path in saved Windows `MSerX/MxMD.dll`, not the
older `MTXPARD.dll` declaration. The image hash is
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
Run `tools/m9-d3d-caps-evidence.py` to pin the templates, GUIDs, builders, and
shader limits and to print the raw declarations as JSON.

## Object and builder layout

Constructor `0x76e90` zeroes the capability object and these public blocks:

| object offset | bytes | role |
|---:|---:|---|
| `0x370` | `0xc0` | common extended-cap state; initialized by `0x77a00` |
| `0x430` | `0x74` | common device-desc state; initialized by `0x77a00` |
| `0x4a4` | `0xd4` | built D3DCAPS8-compatible prefix (`0x77cb0`) |
| `0x578` | `0x130` | complete `D3DCAPS9` (`0x77fd0`) |
| `0x6a8`, `0x6f0` | `0x48` each | video-port capability blocks |
| `0x764` | `0x84` | copied capability/callback block |
| `0x7f8` | `0x30` | miscellaneous callback block |

`0x77a00` copies 43 dwords from static template `0x20a700` into `+0x374`
after writing size `0xc0` at `+0x370`. It copies 29 dwords from `0x20a688`
to `+0x430`, including size `0x74`. `0x77c10` masks both legacy halves before
`0x77cb0` and `0x77fd0` reshape them into D3DCAPS8/9.

The final D3DCAPS9 field inventory is below. “source” names the common object
field or literal written by `0x77fd0`; configuration-dependent values are not
collapsed to one guessed default.

| D3DCAPS9 field | final source/value |
|---|---|
| `DeviceType`, `AdapterOrdinal` | populated outside builder / zero in object |
| `Caps` | common `+0x148`; base ORs include `0x05400140`, `0x00020000`, `0x80000000`, and either `0x10000000` or `0x10000001`; feature query may add `0x01000200` |
| `Caps2` | common `+0x14c`; base `0x00600000 | 0x00029800`; feature queries add `0x20000000`, optionally `0x40000000`, and other gated bits |
| `Caps3` | common `+0x310` |
| `PresentationIntervals` | `0` |
| `CursorCaps` | `3` |
| `DevCaps` | common `+0x380`, static template `0x000aaef1`, masked by `0x000bffff`; configuration queries add HWTL/pure-device related bits before this mask |
| `PrimitiveMiscCaps` | common `+0x3dc & 0x1ff6`; static baseline `0x72` |
| `RasterCaps` | common `+0x3e0 & 0x00f7f199`; static baseline `0x00332181` |
| `ZCmpCaps`, `AlphaCmpCaps` | `0xff`: all eight compare functions |
| `SrcBlendCaps`, `DestBlendCaps` | `0x1fff`: factors through BOTHINVSRCALPHA; constant blend-factor caps clear |
| `ShadeCaps` | `0x000c528a` |
| `TextureCaps` | template `0x0001fc4d & 0x1dff = 0x1c4d`; POW2, cube-POW2, volume-POW2 remain clear |
| `TextureFilterCaps` | `0x0703073f`; point/linear/aniso and mip point/linear, with unsupported quad modes masked |
| `CubeTextureFilterCaps` | literal `0x0703073f` |
| `VolumeTextureFilterCaps` | literal `0x0303033f` |
| `TextureAddressCaps`, `VolumeTextureAddressCaps` | `0x1f`: wrap, mirror, clamp, border, independent UV; MIRRORONCE clear |
| `LineCaps` | literal `0x1f`: texture, Z, blend, alpha compare, fog; antialias clear |
| `MaxTextureWidth`, `MaxTextureHeight` | template `0x3fc0` (16320), `0x3fe0` (16352) |
| `MaxVolumeExtent` | literal `0x400` (1024) |
| `MaxTextureRepeat`, `MaxTextureAspectRatio`, `MaxAnisotropy` | template `0x2000` (8192), `0x800` (2048), `0x10` (16) |
| `MaxVertexW` | template float `0.0` |
| guard band L/T/R/B | `-15360`, `-15360`, `15359`, `15359` |
| `ExtentsAdjust` | template float `0.0` |
| `StencilCaps` | low byte of template `0x1ff` -> `0xff`; two-sided bit is masked here and advertised through separate state/caps routing |
| `FVFCaps` | template `8 | 0x100000`: eight texture-coordinate sets plus PSIZE |
| `TextureOpCaps` | `0x03feffff`: all fixed texture ops through LERP except reserved holes |
| `MaxTextureBlendStages`, `MaxSimultaneousTextures` | `8`, `8` |
| `VertexProcessingCaps` | template `0x17b & 0xfb = 0x7b` |
| `MaxActiveLights`, `MaxUserClipPlanes` | template `0x0c`, low word `6` |
| `MaxVertexBlendMatrices`, `MaxVertexBlendMatrixIndex` | template high word `4`, literal `0x1f` |
| `MaxPointSize` | float `2048.0` at `0x1f5470` |
| `MaxPrimitiveCount`, `MaxVertexIndex` | `0x000fffff` each |
| `MaxStreams` | runtime feature result: `1` or `16` |
| `MaxStreamStride` | `0xffff` |
| `VertexShaderVersion` | config method at feature `+0x268`, OR `0xfffe0000`, capped at `vs_3_0` |
| `MaxVertexShaderConst` | `0x100` (256 vec4 constants) |
| `PixelShaderVersion` | config method at feature `+0x26c`, OR `0xffff0000`, capped at `ps_3_0` |
| `PixelShader1xMaxValue` | `FLT_MAX` (`0x7f7fffff`) |
| `DevCaps2` | `0x41`: stream offsets and vertex elements sharing stream offsets |
| `MaxNpatchTessellationLevel`, `Reserved5` | `0.0`, `0` |
| adapter-group triple | `0`, `0`, `1` |
| `DeclTypes` | `0x33f`: UBYTE4/UBYTE4N/SHORT2N/SHORT4N/USHORT2N/USHORT4N/FLOAT16_2/FLOAT16_4; UDEC3/DEC3N clear |
| `NumSimultaneousRTs` | `4` |
| `StretchRectFilterCaps` | `0x03000300`: point and linear min/mag |
| `VS20Caps` | caps `1` (predication), dynamic depth 24, 32 temps, static depth 4 |
| `PS20Caps` | caps `0x1f` (all five PS2 capability bits), dynamic depth 24, 32 temps, static depth 4, 512 slots |
| `VertexTextureFilterCaps` | `0x01000100`: point min/mag only |
| max VS/PS instructions executed | `0xffff`, `0xffff` |
| max VS3/PS3 instruction slots | `0x200` only when corresponding configured version is at least 3.0; otherwise zero |

The older D3DCAPS8 builder `0x77cb0` uses the same common fields but clamps
the configured stages to `vs_1_1` at `0x77f5e` and `ps_1_4` at `0x77f9b`.
This proves that the 1.x declaration is an API-version mask, not a hardware
limit. The D3D9 builder clamps both stages at 3.0 and publishes concrete 2.0
flow-control, temp, and instruction limits.

## Per-device and configuration masking

No PCI-ID comparison occurs in these builders. Runtime decisions are virtual
feature/config queries reached through the device's `+0xfe70` feature object:

* `+0x220` and `+0x224` add two independent DevCaps feature bits;
* `+0x338` returns a bitset whose `0x08`, `0x40`, `0x04`, `0x01`, and `0x02`
  gates alter HWTL/callback/capability groups;
* `+0x22c` selects one versus sixteen streams and adds two caps families;
* `+0x264` controls whether guard-band floats are retained or zeroed;
* `+0x268` and `+0x26c` provide the configured VS and PS versions;
* `+0x270` clears bit `0x200` from two texture/raster capability words;
* `+0x27c` selects the `Caps` low bit and related Caps2/Caps3 families;
* `+0x248` can add `D3DCAPS2_CANAUTOGENMIPMAP`.

These are real masks over a common M9 template. They show callable support for
NPOT, anisotropy 16, four MRTs, SM3 declarations, vertex texture point fetch,
predication, all PS2 optional features, and post-1.x flow control. They do not
prove every combination renders correctly on M9148; public Mesa caps still
need result tests and exact native-program record closure.

## DdGetDriverInfo GUID-to-block route

`DdGetDriverInfo` at `0x3db50` compares the 20 GUIDs at
`0x1e88ec..0x1e8a1c`. Exact named joins include:

| GUID | object block/action |
|---|---|
| `GUID_DDStereoMode` (`0x1e88ec`) | special handler rather than a raw copy |
| `GUID_DDMoreSurfaceCaps` (`0x1e890c`) | `+0x348`, at most `0x28` bytes |
| `GUID_D3DParseUnknownCommandCallback` (`0x1e892c`) | callback pointer `+0x7f0` |
| `GUID_NonLocalVidMemCaps` (`0x1e896c`) | `+0x314`, at most `0x34` bytes |
| `GUID_D3DCallbacks3` (`0x1e897c`) | `+0xa8`, at most `0x18` bytes |
| `GUID_D3DCallbacks2` (`0x1e898c`) | `+0x8c`, at most `0x1c` bytes |
| `GUID_D3DExtendedCaps` (`0x1e89ac`) | `+0x430`, at most `0x74` bytes |
| `GUID_Miscellaneous2Callbacks` (`0x1e8a0c`) | `+0xc0`, at most `0x18` bytes |

The remaining GUIDs return bounded blocks at `+0`, `+0x370`, `+0x6a8`,
`+0x6f0`, `+0x764`, and `+0x7f8`, or use a specialized handler. Their raw UUIDs
are emitted by the verifier. Naming an unknown UUID is left open rather than
borrowing a structure name from an older DDI revision.
