# M9 programmable shader transport contract: static closure

This note reconciles the linked-record, stage-install, entry, termination, and
native-control-flow investigations into one driver-facing result. It uses only
the saved binaries and the existing saved A/B/A evidence. `[BIN]` is a direct
binary observation, `[CAP]` a saved-capture observation, and `[DER]` the narrow
conclusion supported by them.

The pinned inputs are:

| image | SHA-256 |
|---|---|
| Windows `MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |
| Windows `MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| Linux 1.4.3 build 98 `xserver/1_13_0/m9x_drv.so` | `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273` |

## Driver-facing result

The universal programmable contract is an **opaque linked pair**, selected by
one tagged write to index `6c` [BIN][DER]. It is not an exposed native-image
header. The reachable records are heterogeneous linked render-resource
objects, and record-relative native-looking regions move between workloads.
There is no statically supported universal `6c` field containing a native
image address, slot count, vertex start, fragment start, or terminal PC. The
external bind writes fixed-fragment-database resource `69`, numeric controls `6a/6b`, and one
linked-pair resource through `6c`; it emits no code count or end [BIN].

This negative result closes the transport schema. A driver may replay a
complete linked pair as one immutable object; it must not parse or manufacture
stage entries in a presumed fixed `6c` header. The September A/B/A corpus
independently changes either source stage while `69=0x11e42000`,
`6a=0x25010000`, and `6b=0x00001030` remain fixed. In both axes the sole
program-selection value is `6c`, changing
`0x40c3a301 -> 0x40c3a881 -> 0x40c3a301` [CAP]. After removing its low tag,
the two selections are offsets `0x300` and `0x880` in the same
`0x40c3a000`/`0x21000` system allocation [CAP]. Vertex and fragment selection
therefore changes atomically at this boundary; there is no separately visible
VS or FS entry assignment to tear [DER].

The fixed-chain inline form is a clean, bounded transport for a driver-owned
native image [DER]. It uploads 8-byte slots in 32-byte lines through selector
`68` and ports `60..67`, writes `69=4`, and supplies two 16-bit slot starts in
`6a` or splits the companion start to `6d`. The Windows builder records image
pointer/length at `+0xbb4/+0xbb0` and starts at `+0xba8/+0xbac`; Linux uses
`+0xbe0/+0xbd8` and `+0xbd0/+0xbd4` [BIN]. The upload is padded to four slots,
and the compiler-side slot vector rejects growth beyond 256 slots [BIN].

This is sufficient to implement native-image upload, bounds checking, padding,
and entry emission for that fixed chain without reverse engineering an
external record. It does not label the two starts as VS and FS. The producer is
the fixed vertex/declaration chain, its second start is a selected companion
chain, and the inactive-companion form packs both starts into `6a`. The
programmable A/B/A result above also leaves `6a` invariant and emits no `6d`.
Consequently `6a/6d` are replayable fixed-chain controls, not universal
programmable-stage registers [BIN][CAP][DER].

The native count is likewise representation-local. In the fixed-chain builder
the two entries are cumulative byte counts divided by eight and total bytes
are rounded to 32 before publication. In the compiler serializer every native
slot append is bounded at 256. Neither value is emitted as an execution bound
in the programmable external bind [BIN]. A clean driver therefore retains a
software slot count for validation and allocation, while the hardware-facing
programmable object remains the complete linked pair [DER].

The exact Windows offsets and corresponding PE file offsets are:

| evidence | VA | file offset | fields |
|---|---:|---:|---|
| fixed-chain builder | `0x13bf80` | `0x12bf80` | starts `+0xba8/+0xbac`, aligned bytes `+0xbb0`, pointer `+0xbb4` |
| program emitter | `0x0f74d0` | `0x0e74d0` | selectors `68`, `60..67`, `69`, `6a`, optional `6d` |
| resolver append | `0x1a2700` | `0x192700` | record boundary `+0x40`, entries at resolver `+0x6c/+0xb8` |
| ordinal lookup | `0x1a2e60` | `0x192e60` | 12-byte entry dword 1 or `0xf0000000` |
| type-3 resolver | `0x1dc690` | `0x1cc690` | typed operands `+0x14/+0x18/+0x1c` |
| resolve/serialize | `0x1dc7d0` | `0x1cc7d0` | resolve temporary, then call serializer |
| direct serializer | `0x1dc8b0` | `0x1cc8b0` | packed native slot(s), maximum 256 |

Linux independently places the fixed-chain fields at starts `+0xbd0/+0xbd4`,
aligned bytes `+0xbd8`, and pointer `+0xbe0`; emitter `0x4b9340` consumes them.

## Physical control-flow fixups close before transport

Windows control builders allocate 0x20-byte objects with vtable `0x1f50e0`.
The live virtual sequence is [BIN]:

| vtable slot | VA | role |
|---:|---:|---|
| `+0x00` | `0x1dc790` | type-2/type-3 compatibility dispatch |
| `+0x04` | `0x1dc7d0` | clone, resolve, then serialize |
| `+0x08` | `0x1dc8b0` | direct native-slot serializer |

`0x1dc7d0` copies all fields into a temporary object, constructs the local
resolver data passed by its caller, invokes `0x1dc490` for type 2 or `0x1dc690`
for type 3, then calls `0x1dc8b0`. Thus a
`0xb4000000 + ctx[0x59c]` value is an intermediate
typed descriptor, not a native branch displacement [DER].

For type 3, `0x1dc690` walks eligible descriptors in object fields `+0x14` and
`+0x18` in encounter order. Eligibility is `high_nibble != 0xf` and
`descriptor & 0x0c000000 != 0`. Each eligible value is replaced by dword 1 of
the next 12-byte entry in a resolved table and enables serializer gate `2` or
`4`. An eligible `+0x1c` is replaced from the corresponding transformed local
value and enables gate `0x10` [BIN]. Lookup `0x1a2e60` uses six inline entries
at resolver `+0x6c`, then a dynamic tail at `+0xb8`; an out-of-range lookup
returns `0xf0000000` [BIN]. The serializer performs no arithmetic on the low
serial value. Physical destination selection has already become a resolved
descriptor before native packing [DER].

The producer side is now joined too. Resolver records are 0xc4 bytes and carry
an ordered sequence of 12-byte entries. `0x1a2700` sets record word `+0x40` to
the current terminal-entry ordinal on first use, writes its input descriptor
to that entry's dword 1, then appends
`{descriptor & 0x0000fff0, 0xf0000000, 0xffffffff}`. `0x1a2a10` performs the
parallel dword-1 update for the ordinal in record word `+0x30`; `0x1a28d0`
updates dword 0 [BIN]. These operations make the final sentinel explicit and
explain why lookup requires `ordinal + 1 < total` [DER].

Transform `0x1cef80` walks every non-sentinel source entry in ordinal order.
For a type-`0x0d` record it compares the loop ordinal with record word `+0x40`;
the matching entry's already-resolved dword 1 goes to `0x1a2700`, while other
entries go through descriptor conversion `0x1a30c0`. Compiler pass `0x1d1d40`
copies the resulting 0xc4-byte records in order at
`0x1d3b6b..0x1d3baa` and `0x1d3e01..0x1d3e40` [BIN]. The type-3 resolver then
consumes those physical descriptors by ordinal, as described above.

The mapping replacement producer is now exact too. Compiler pass `0x1d1d40`
keeps source descriptors and the output of physical converter `0x1df900` in
same-length dword arrays. Its later loops call constructor `0x1a2ae0` with
`{source[i], physical[i]}`, producing mapping dwords 0 and 1 before the
0x1c-byte entry is appended and sent to `0x1cef80` [BIN]. Type-`0x94` uses a
descending 16-bit selector allocator with bias `+0x20`; type `0xb4` uses four
selectors biased by 4; type `0xc4` uses 23 selectors biased by 8 [BIN]. The
saved Linux compiler independently has the paired arrays, mapping publication,
same formulas and modifier merge [BIN]. Exact sites and formulas are in
`parhelia-m9-linked-pair-target-placement-re.md`.

This closes the transport lifetime and physical placement rule for the pinned
typed fixups: graph serials and typed markers are compiler temporaries;
physical selectors are substituted before the native slot is packed, and the
selected linked pair is immutable while submitted [DER]. There is no late
branch relocation in the `68..6d` packet emitter. A new native compiler still
needs graph construction and scheduling rules for arbitrary forward branches,
backedges, CALL, BREAK, nesting and reconvergence beyond the pinned REP/LOOP
recipes. That is compiler implementation work, rather than a missing
transport field [DER].

## Exact serializer footprint

Direct serializer `0x1dc8b0` packs fields under the gate word at object `+0x10`
[BIN]:

| gate | source | native insertion |
|---:|---:|---:|
| `0x02` | `+0x14` | second output dword bits 20:15 |
| `0x04` | `+0x18` | first output dword low 6 bits |
| `0x10` | `+0x1c` | second output dword bits 13:9 |
| `0x08` / `0x41` | `+0x0c` | first output descriptor/control field |

It dispatches on `(object[+8] >> 22) & 31`. Classes 19 and 20 subtract four
from the first output dword; classes 19, 20, 21, 22, and 25 append the packed
slot plus three literal `{0,0}` slots and set output property bit 29. Classes
23 and 24 append only the packed slot. Every append checks `count + 1 <= 256`
[BIN]. The helper at `0x14c6a0` merely copies one two-dword slot and increments
the count, so those three slots are padding/footprint, not hidden target
patches [DER].

A clean compiler may reproduce the resolved native fields and footprints. It
must not expose the Windows intermediate `0xb4000000 + serial` marker as a
kernel ABI because this marker belongs to the compiler's pre-resolution object
graph rather than the native transport [DER].

## Terminal rule

Both vendor compiler paths converge on terminal slot [BIN]:

```text
W0 = fffc7e1d
W1 = a2803f0f
```

It moves the stage-relative output selector `0x78` to destination selector
`0x7e`. Native transport sends neither a slot count nor an end PC. Static code
cannot distinguish whether selector `0x7e` itself terminates execution or the
whole final MOV form does. A driver can safely append the exact vendor terminal
slot to compiler output; claiming a general `END` encoding still requires the
single terminal mutation experiment described in
`parhelia-m9-shader-termination-boundary.md`.

For opaque programmable replay, termination is already part of the immutable
linked pair and needs no packet field [DER]. For driver-generated inline code,
the admitted terminal contract is deliberately narrower: append exactly
`fffc7e1d a2803f0f`, include it in the validated software slot count, pad the
upload to four slots, and do not submit any other proposed stop form. This
closes safe transport of known images while keeping general endpoint semantics
as an ISA-validation gate [DER].

## Association, allocation, and lifetime

GL stage identity is exact before the final bind. Private opcode `0x59` carries
flag 0 for fragment and 1 for vertex into distinct Linux backend objects at
device `+0x2560/+0x2550`; opcode `0x67` associates the varying map with the
fragment install result [BIN]. The compiler/cache vtable and native descriptor
copies terminate in separate top-state `+0xd8` resource/link state. Final
program emission uses top-state `+0xc8`. Constructor, vtable, copy, relocation,
and serializer audits find no edge joining the typed current handles to the
external program fields or assigning universal VS/FS names to `6a/6d` [BIN].

Inline upload copies the CPU native-image bytes into the ordered command stream
before the bind. The CPU compilation buffer can be released after command
construction, while the command BO and its restore/context state must remain
valid until the job fence retires [DER]. External `69/6c` resources and every
referenced backing object must stay pinned through that fence. Windows
`0xf9b70` attaches one submission value to both TaskMgr resource vectors before
submission; Linux `m9x954` attaches the returned submission value to referenced
backings before dropping their in-flight references [BIN]. Restore slots are
reused only after their availability/fence checks [BIN].

Program RAM must be treated as context state. The recovered Linux dirty-state
promotion and ordered A/B context switch re-emits program state through the
same queue, but no binary proves that inline program RAM survives reset or
another context's upload. Re-upload on context switch, reset generation, and
engine recovery is therefore the implementation-safe policy [DER].

The complete ownership rule is copy-on-write. Construct and finish the
instruction image, physical fixups, terminal, linked stage pair, and resource
vector before making the bind visible. Queue the data and its `6c` selection in
order; do not modify the selected object. Retire the command BO, linked record,
instruction backing, and referenced resources together at the submission
fence. A stage change builds or retrieves another complete pair and switches it
with one `6c` value [DER].

## Closed contract and remaining gates

The universal transport contract is closed [BIN][CAP][DER]:

1. programmable VS/FS state is one opaque, immutable linked pair selected by
   `6c`; it has no public per-stage entry or count fields;
2. stage changes publish a complete pair atomically, with all physical fixups
   resolved before native packing;
3. the fixed-chain inline form has two shared-image starts, a software byte
   length, 32-byte upload padding, and no hardware end/count write;
4. known driver-generated images end in the exact vendor terminal slot; and
5. instruction, command, linked-record, and referenced-resource lifetimes end
   only after the submission fence, with program RAM replayed after context or
   reset invalidation.

The remaining gates are outside this transport ABI [DER]:

1. recover or independently implement graph construction and scheduling for
   arbitrary forward branches, backedges, CALL, BREAK, nesting and
   reconvergence; physical allocation and final W0/W1 selector placement for
   the pinned `94/b4/c4` families are exact;
2. identify enough of the heterogeneous linked-record constructor to create a
   new programmable pair rather than replay a captured/vendor-produced one;
3. mutate the terminal destination and final operation independently before
   accepting stop forms other than `fffc7e1d a2803f0f`; and
4. establish allocator wrap/reuse and reset behavior on hardware before
   enabling long-running programmable workloads.

Until those are closed, use the programmable path only for complete known-good
linked pairs, and use inline upload only for bounded images whose two starts
are known by construction. Keep arbitrary native control flow behind a
development flag.

Run `python3 tools/m9-shader-transport-closure-evidence.py` from the repository
root. The checker uses Python's standard library, executes no vendor code, and
pins the hashes, GL stage flags, fixed-chain start/length publication, ordered
resolver producer/lookup, direct serializer, footprint table, zero-slot append
helper, and Linux inline bind. Run
`python3 tools/m9-linked-pair-target-evidence.py` for the independent
Linux/Windows paired mapping producer, physical-selector allocators and final
native selector fields. Run
`python3 tools/m9-glsl-ab-evidence.py --self-test` for the independent atomic
linked-pair A/B/A evidence.
