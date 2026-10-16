# M9 programmable-stage transport and hardware entry association

This note records the static join that is currently possible between linked
GLSL stages in `MxMGLX.dll`, the corresponding Linux `m9x_drv.so` path, and
MxMD's native-program bind.  Addresses are virtual addresses.  `[BIN]` means a
direct instruction/data observation and `[DER]` a conclusion from those
observations.

The binaries used here are:

* Windows `MxMGLX.dll`, SHA-256
  `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86`;
* Windows `MxMD.dll`, SHA-256
  `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`;
* Linux 1.4.3 build 98 `m9x_drv.so`, SHA-256
  `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`.

## Linked GL objects keep explicit stage identity

The Windows link path at `0x10555ff1..0x10556356` records vertex and fragment
presence at program bytes `+0x61ac/+0x61ad`.  It chooses the attached vertex
object into `+0x5ba4` and fragment object into `+0x5ba8`.  Serialization places
the vertex blob pointer and byte length at `+0x5bac/+0x5bb4`, and the fragment
blob pointer and byte length at `+0x5bb0/+0x5bb8` [BIN].  These are distinct
compiler products, not entry offsets into the final native image.

Function `0x1055d920` transports either blob in internal opcode `0x59`, whose
allocated size is `0x20` bytes.  Its 0x1c-byte body is [BIN]:

| Body offset | Meaning |
| --- | --- |
| `+0x00` | converted linked-program handle, 8 bytes |
| `+0x08` | stage flag: 1 for `GL_VERTEX_SHADER` (`0x8b31`), 0 for fragment |
| `+0x0c` | zero |
| `+0x10` | converted stage-blob pointer/handle, 8 bytes |
| `+0x18` | stage-blob byte length |
| `+0x1c` | output field, initialized to zero; its address is returned |

The vertex branch reads `+0x5bac/+0x5bb4` and marks destination byte `+0x12c`;
the fragment branch reads `+0x5bb0/+0x5bb8` and marks `+0x12d`.  Calls at
`0x10556306` and `0x10556326` supply targets `0x8b31` and `0x8b30`,
respectively [BIN].

When both stages exist, the call at `0x10556352` passes the **fragment** opcode
`0x59` output, the 14-dword varying remap at program `+0x5bbc`, and its
high-water value at `+0x5bf4` to `0x1055deb0`.  That function emits opcode
`0x67`, size `0x18`, and marks destination byte `+0x12e` [BIN].  Thus `0x67`
is the cross-stage link record and its stage-object operand is specifically the
fragment install result [DER].  This fact does not identify an MxMD hardware
entry register.

The independently compiled Linux path confirms the association.  Link routine
`m9x4183` at `0x6be950` uses presence bytes `+0x6118/+0x6119`, vertex and
fragment blobs `+0x5ae8/+0x5af0`, lengths `+0x5af8/+0x5afc`, varying remap
`+0x5b00`, and high-water `+0x5b38` [BIN].  Function `m9x23579` at `0x6af060`
emits opcode `0x59`, size `0x20`; target `0x8b31` selects the vertex blob and
passes stage flag 1 to `0x5b6690`, while the other branch selects the fragment
blob and passes zero.  At `0x6bee2b..0x6bee44`, the value in `[rsp+8]` from the
fragment install is passed to `m9x13837` at `0x6aee50` along with the remap and
high-water.  That function emits opcode `0x67`, size `0x18` [BIN].

## Linux closes the private-call stage join

The Linux build contains the private wrapper and the backend in one image, so
the call following the `0x59` marker can be followed farther than its Windows
counterpart.  Wrapper `0x5b6690` tests the serialized stage flag directly.
Flag zero calls `0x3b8a50`; that path uses state object `device+0x2560` and
calls compiler/install routine `0x4360c0`.  Flag one calls `0x3b8d60`; that
path uses `device+0x2550` and calls `0x435b30` [BIN].  Joined to the
`GL_VERTEX_SHADER` test in `0x6af060`, this assigns the objects without relying
on names [DER]:

| stage flag | GL stage | backend state object | compiler/install routine | current handle |
| --- | --- | --- | --- | --- |
| 0 | fragment | `device+0x2560` | `0x4360c0` | object `+0x10e` |
| 1 | vertex | `device+0x2550` | `0x435b30` | object `+0x1f1` |

Both compiler/install wrappers receive the source blob pointer and byte length
from `0x59`, and both retain the supplied source byte length at compiled object
`+0x40`.  That field is source transport metadata; it is not emitted beside a
hardware entry control [BIN].

The `0x67` route is fragment-owned too.  Its body wrapper `0x5b6550` calls
`0x3b8c90`.  The latter loads `device+0x2560`, compares the `0x67` fragment
handle with current association `+0x11c`, and calls `0x431d70` with that handle,
the varying remap, and the high-water count (rejected above 15) [BIN].  Thus
`0x67` creates/updates cross-stage association data in the fragment backend;
it is not a second stage program upload [DER].

This route is not peculiar to one X-server build.  The unique target-`0x8b31`,
stage-dispatch, and `0x67` association signatures occur once in every one of
the 16 saved `m9x_drv.so` images and every saved 64-bit `libGL.so.2.0.0` image,
from X server 6.9 through 1.15.  In every copy, rel32 decoding reaches the same
`+0x2560` fragment and `+0x2550` vertex object split and the same fragment
`+0x11c` association field [BIN].

The Windows opcode numbers are software-record identifiers, not native shader
opcodes.  Allocator `MxMGLX!0x100030c0` groups records under an eight-byte
`{opcode,count}` header.  Flush `0x100031a0` passes the byte span to
`0x1002ea20`, resets the buffer, and writes `{0xffff,1}` at its new head
[BIN].  That `0xffff` terminates the private software record stream.  It never
appears in the native 8-byte shader image and therefore supplies no hardware
program stop rule [DER].

The exact producer signatures `push 0; push 0x20; push 0x59` and
`push 0; push 0x18; push 0x67` each occur once in `MxMGLX.dll` and do not occur
in `MxMD.dll` or `MxMDI.dll` [BIN].  This rules out a copied direct-call helper
as the missing Windows join.  It does not rule out an indirect escape/table
dispatcher, so it is negative evidence rather than a recovered consumer.

## What the two recovered hardware starts prove

MxMD function `0x13bf80` builds one concatenated, 8-byte-instruction image for
the fixed fragment-chain path.  It records the two chain starts as cumulative
byte offsets shifted right by three at state `+0xba8/+0xbac`, the aligned image
length at `+0xbb0`, and its pointer at `+0xbb4` [BIN].

Emitter `0xf74d0` has two program-bind forms [BIN]:

* External/resource form: index `69` is relocation-backed from state
  `+0xa0/+0xa4`; dirty bit `0x80` emits `6a <- +0xa8`, and dirty bit `0x100`
  emits `6d <- +0xbc`.
* Inline fixed-chain form: indices `68` and `60..67` upload `+0xbb4` in
  32-byte lines.  With `+0xba4 == 0`, it emits `69 <- 4` and packs
  `low16(+0xba8)` and `low16(+0xbac)` into the low and high halves of `6a`.
  With `+0xba4 != 0`, it emits the second start to `6d`, then `69 <- 4` and the
  first start to `6a`.

This proves two entry slots into a shared native image and shows that `+0xa8`
and `+0xbc` are the corresponding external-form controls [DER].  It does **not**
prove that `6a` is the vertex entry or that `6d` is the fragment entry.  The
inline image is selected by the fixed vertex/declaration-conversion builder;
its second slot can be a companion chain selected by `0x13b570`.  Packing both
starts into `6a` when the companion mode is inactive is also incompatible with
assigning a universal shader-stage name from the register number alone.

Linux independently contains the same emitter as `m9x11728` at `0x4b9340`,
with state layout shifted by `0x28`: native image pointer/length are
`+0xbe0/+0xbd8`, starts are `+0xbd0/+0xbd4`, and companion flag is `+0xbcc`.
It emits the same `68`, `60..67`, `69`, `6a`, and optional `6d` selectors
[BIN].  Its producer `m9x24483` at `0x44ba40` builds those starts from selected
fixed-chain fragments and cumulative byte sizes.  Neither function reads the
typed current handles at vertex object `+0x1f1` or fragment object `+0x10e`.
This cross-check therefore preserves, rather than closes, the final stage to
entry-register boundary [DER].

## Final Linux state ownership and dispatch

The object passed to `m9x11728` is no longer structurally anonymous.  Generic
program-substate initializer `m9x17453` at `0x4b8b80` initializes
external-resource identity fields `+0xa0/+0xa8`, primary entry control
`+0xb0`, companion entry control `+0xd0`, and the inline-program fields later
used by the emitter [BIN].  In temporary top-state construction at
`0x4b4478`, that initialized object is installed specifically at top-state
`+0xc8`; the other substates go to `+0xb8`, `+0xc0`, `+0xd0`, and `+0xd8`
[BIN].

The live hardware-state constructor gives the specialized form of the same
ownership.  `m9x3673` constructs `m9x18369` at live state `+0xbb0` and
`m9x31420` at live state `+0x17c0`.  Its owner pointers are stored at `+0xd0`
and `+0xe0`; the corresponding dispatch view is biased by eight bytes and sees
them as top-state `+0xc8` and `+0xd8` [BIN][DER].  Independent reset routine
`m9x11566` removes the need to infer that bias: it loads top-state `+0xc8` and
calls `m9x18369`, then loads top-state `+0xd8` and tail-calls `m9x31420`
[BIN].  `m9x18369` initializes a large specialized program state including
the emitter's `+0xa0/+0xa8/+0xb0/+0xd0` and inline image
`+0xbcc..+0xbe0`.  `m9x31420` initializes a separate `0xd0`-byte resource/link
state with three descriptor/pointer pairs at `+0x98/+0xa0`, `+0xa8/+0xb0`,
and `+0xb8/+0xc0` [BIN].

The complete final dispatch chain is [BIN]:

```text
m9x15197 (0x4b6c40)
  promote desired top_state+0xc8 dirty state
  switch context current top state at context+0x5f8
  -> m9x25088 (0x889a60)
       top_state+0xc8 -> m9x26001 (0x4ba3a0)
         -> m9x11728 (0x4b9340)
              resource 69, entry controls 6a/6d, or inline image
```

`m9x26001` has one direct owner, the `top_state+0xc8` call in `m9x25088`,
and `m9x11728` has one direct owner, the call at `m9x26001+0x579`.  Before a
state switch, `m9x15197` merges the program substate's current dirty word into
its pending word.  When byte `+1` has its high bit set it also promotes each
28-byte sparse record from `+0xe8` to its pending word at `+0xec`, through
offset `+0x7e8` [BIN].  This proves the lifetime and dirty-state route into the
emitter, but it does not identify the earlier writer that associates a
compiled native descriptor with `+0xa0/+0xa8/+0xb0/+0xd0` [DER].

## Compiler/cache vtables and the excluded resource-copy join

The compiler/cache path is live through indirect calls that a rel32-only
census misses.  Parent constructor `m9x1757` installs the vtable whose address
point is `0x121e990`.  Its slot `+0x08` is `m9x15760`, a getter returning
parent `+0x60`.  Factory `m9x8953` constructs an `m9x28041` child and stores it
at that field.  At `0x5b15d1..0x5b15dd`, the GL context calls the getter and
stores the returned child at context `+0x9ba8` [BIN].

The `m9x28041` child vtable address point is `0x121e7b0` [BIN]:

| slot | function | established role |
| ---: | --- | --- |
| `+0x00` | `m9x7986` | child destructor |
| `+0x08` | `m9x28572` | child callback |
| `+0x10` | `m9x3336` | child callback |
| `+0x18` | `m9x22637` | live update/compile entry |
| `+0x20` | `m9x1950` | live update/compile entry |
| `+0x28` | `m9x27762` | child callback |
| `+0x30` | `m9x30491` | child callback |
| `+0x38` | `m9x24167` | child callback |
| `+0x40` | `m9x10959` | returns child `+0x118` |

GL paths at `0x5a56d6` and `0x5a544e/0x5a55f8` load context `+0x9ba8` and
call vtable slots `+0x18` and `+0x20`, respectively.  The two exported entries
have no direct rel32 callers, but both call `m9x24062`; the vtable and context
ownership therefore prove they are live [BIN][DER].

This indirect path also disproves the most attractive proposed final join.
Device setup `m9x11513` maps child `+0x38` to device `+0x2588` and child
`+0x40` to device `+0x2590`.  At `0x4530c7..0x4530db`, `m9x24062` passes
child `+0x38` as `rdx`, child `+0x40` as `rcx`, and calls `m9x21983` with no
pointer adjustment [BIN].  `m9x21983` copies two compiled-native records from
`[compiled_pipeline+0x138]+0x2f1/+0x2f9` and `+0x301/+0x309` into destination
`+0x98/+0xa0` and `+0xb8/+0xc0` [BIN].

That destination is exactly the separate resource/link object.  During device
construction `m9x4335` requests `0xd0` bytes, initializes through `+0xc8`,
stores the allocation at device `+0x2590`, and wires it beside device
`+0x2588` through `m9x21443` [BIN].  The identical `m9x31420` initializer is
used for live state `+0x17c0` and top-state `+0xd8`.  Final dispatcher
`m9x25088` sends `+0xd8` to `m9x21922`, then independently sends `+0xc8`
through `m9x26001` to `m9x11728` [BIN].  The matching field numbers in these
two types do not establish aliasing.  The compiler-native copy is now assigned
to the resource/link path and excluded as the producer of the final program
bind [DER].  Constructor-relocation vtable recovery, the shared child callback,
the device-pointer setup, both compiled-resource copies, the `0xd0` allocation,
and the distinct `+0xd8/+0xc8` dispatches reproduce in all 16 saved
`m9x_drv.so` images and all 16 saved combined `libGL.so.2.0.0` images [BIN].

Windows gives a consistent private-boundary result.  The decoded import table
of the saved `MxMGLX.dll` names `DDRAW.dll` and ordinary system DLLs, but does
not import `MxMD.dll` or `MxMDI.dll` [BIN].  Together with the unique software
record producers already described, this rules out a statically named
cross-DSO compiler-to-MxMD call.  It leaves an escape, callback table, copied
object, or command-record consumer as the Windows ownership boundary [DER].

Two tempting alternate stage wrappers are excluded from the in-image live GL
route.  Fragment `m9x1996` at `0x3b8ae0` and vertex `m9x13428` at `0x3b8df0`
select builder mode 2 and optionally supply the persistent constant banks at
fragment state `+0xba` and vertex state `+0x91`.  Across all 16 saved 64-bit
`m9x_drv.so` images and all 16 combined `libGL.so.2.0.0` images, neither
wrapper has a direct call/jump, RIP-relative address materialization, or ELF
relocation owner [BIN].  They remain exported ABI entries, so an external
caller is possible, but the saved combined images provide no evidence that
the ordinary private GL stream calls them [DER].  The ordinary calls instead
reach the mode-1 wrappers at `0x3b8a50/0x3b8d60`.  Mode-2/BM state therefore
cannot supply the missing live programmable-stage join without runtime
evidence.

The often-cited state builder at `0x1397e0` does not close this gap.  Its call
to `0x1743d0` writes float/color state at record offsets `+0x70..+0xec`, and
`0x13ee10` consumes the record during draw/vertex state construction [BIN].
Static evidence does not make that 256-byte cache a fragment compiler result.

## Exact remaining boundary

The saved binaries establish this chain:

```text
GL linked program
  -> distinct vertex/fragment serialized blobs
  -> opcode 0x59(stage flag, blob, length)
  -> opcode 0x67(fragment-install result, varying map, high-water)
  -> typed vertex (+0x2550) / fragment (+0x2560) backend objects [Linux]
  -> compiler/cache child at context +0x9ba8
  -> compiled native pairs in separate top_state+0xd8 resource/link state
  -> [unrecovered writer of final top_state+0xc8 program state]
  -> native image resource or inline image
  -> entry controls 6a / 6d
```

Static code now joins `0x59/0x67` to typed backend objects and follows the live
compiler/cache vtable into its resource/link copy.  That copy is not the final
program state.  No static call, vtable, relocation, constructor, or recovered
object-copy edge joins either compiled stage/current handle to the program
substate external fields in MxMD state
`+0xa8/+0xbc` or `+0xba8/+0xbac` (Linux `+0xb0/+0xd0` or
`+0xbd0/+0xbd4`).  The native bind producer found in both operating systems is
the separate fixed-chain builder.  Therefore the final programmable
vertex/fragment-to-entry association remains a capture boundary.  A Mesa
driver can implement typed stage blobs, the known transport layout, and the
resource/link state, but cannot safely populate the final external descriptor
or select the final two hardware entries from this evidence alone.

The separate fixed-chain inline path can nevertheless serve as a clean,
bounded transport for a driver-owned native image: upload through
`68/60..67`, emit `69=4`, and use the two known starts in `6a/6d`.  This avoids
inventing a universal layout inside `6c`.  Arbitrary VS/FS use still requires
one inline stage-swap capture to assign those two starts; see
`parhelia-m9-shader-transport-static-closure.md` [DER].

The smallest decisive differential capture uses one fixed vertex shader and
two fragment shaders differing only in a constant output.  Disable the saved
driver's bus-master microcode options (`PsBmMicrocode=0`, `CeBmMicrocode=0`)
when possible, record the fragment `0x59` and following `0x67`, then capture the
native bytes and final `69/6a/6d` writes immediately before the first draw.  If
only image bytes change, repeat with a fixed fragment shader and two vertex
shaders.  If the external form remains active, dump the resource referenced by
the `69` relocation as well as `+0xa8/+0xbc`; an MMIO trace alone may contain
the entry values but omit the program image.  These two comparisons are enough
to associate stages without relying on names inferred from `Ps` or `Ce`.

Run `python3 tools/m9-program-stage-entry-evidence.py` to recheck the binary
hashes and the critical stage/opcode/entry joins above.  Run
`python3 tools/m9-entry-resource-boundary-evidence.py` to recheck the 32-image
alternate-wrapper ownership census, program-substate construction, dirty
promotion, compiler/cache vtables, object allocation/copy ownership, Windows
import boundary, and sole final dispatch chain.

## September 2026 live closure

The requested differential is now captured in
`captures/m9x-linux-glsl-ab-20260914` and analyzed in
`parhelia-m9-glsl-bind-capture.md`.  Both the vertex-only and fragment-only
A/B/A switches keep `69=0x11e42000`, `6a=0x25010000`, and `6b=0x1030`, while
`6c` selects `0x40c3a301`, `0x40c3a881`, then the original
`0x40c3a301`.  Allocation capture joins the high address to a 0x21000-byte
system resource at `0x40c3a000`; masking the retained low tag selects record
offsets `0x300` and `0x880`.  This closes the final live ownership edge as an
atomic linked-pair state record.  It does not expose the native bytes or the
two internal stage entries within that record; those remain resource-format
and compiler-coverage work rather than final-dispatch ownership work.
