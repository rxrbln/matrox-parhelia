# Final ordinary-output resolve state audit

The normal and genuine public constant-green output captures have the same
488-byte final lowered block, offsets `13e8..15d0`, SHA256
`b14c1cfd9697eb54ddd87e82286e1096b093d809daf2de7a0e43ac764c68dc19`.
This comparison found no omitted address callback or different rectangle
parameter in that block. It does not establish the hardware interpretation of
GPU-produced fragment records or a working image.

Actual compiler mode9 calls `fadf0` before its final trigger. `fadf0` lowers
fragment, AA, common, texture and miscellaneous shadows in order through
`fc0e0`, `fb920`, `f4f50`, `f6f20`, `fb260`. The final fragment writes are
F0/EF/D7 `e0000400/20007000/0`, index22 zero. The main logical extent remains
`136080`; its AD encoding is `136080 >> 5 = 9b04`. Native page-rounded capacity
must not replace this logical extent.

The actual six nonnull callbacks in this block join retained owners:

| Actual kind | Native owner | Role | Domain |
| --- | --- | --- | --- |
| 2 | 27 | Ordinary output | SYSTEM GPU virtual |
| 6 | 4 | AA companion | Local |
| 7 | 5 | HZ | Local |
| 2b | 22 | Device-selected pool, index24 | Local |
| 9 | 2 | Main store | Local |
| a | 3 | Auxiliary store | Local |

Each destination slot appears in the generated fixup inventory, with its actual
source kind, encoded payload, live owner and view offset. Eight later texture
and route callbacks, kinds `b..e` repeated twice, have actual pair `[0,0]` and
payload zero. They are source-produced null bindings, not addresses inferred
from numeric ranges. No active resource is substituted for those bindings.

Common-state `f5730` compares the program pair against the global null pair
`20d51c/20d520`. The observed final output takes the null-program branch:
`f58cf/f58d9` produce C2=4/C3=0, followed by four inline pairs
`ffff801c/ee9fc31f`. Those instructions, disabled texture bindings and their
descriptors are preserved in both complete native profiles. The dynamic
`f5898` timeline field is translated, as is completion producer `2557b` in
this block. Calling C2=4 an omitted program address would contradict its
actual literal producer. This audit does not decode the inline program's GPU
semantics.

EC is literal zero: `f8e45` clears EBX and `f8ea1` stores it. F3 and FC come
from fragment shadow+30/+34. Actual `abc70/abc7e` publish the caller rectangle;
`f8eb9/f8ed1` copy its origin and extent. These trigger fields read no pointer
or global. The bounded origin-zero 320x240 producer therefore preserves
`45fcf3ec/0/0/00f00140` through native relocation and clear-segment resizing.
The retained source call sequence dispatches at `ac0e3`, executes the lowerer,
then records resource deadlines at `ac139`. The temporary remains live at the
strict first GPU-input boundary and throughout the separately captured flush.
The original CPU-copy return and release are not supplied or claimed.

`tools/check-m9-faa-output-resolve-state.py` checks both profiles against the
pinned shipping instructions, actual CPU stores, typed callbacks, generated
owner/fixup tables, timeline sites and byte-exact input blob. It independently
walks this final raw indexed block, rather than interpreting the entire primary
as this grammar. Run the whole-flow manifest checker first, then:

```
PYTHONPATH=tools/faaemu /home/ai/tmp/faaemu/venv/bin/python \
  tools/check-m9-faa-output-resolve-state.py \
  captures/m9128-device-inputs-20261006/normal-faa-output-flush-input-manifest-101f8.json \
  captures/m9128-device-inputs-20261006/normal-faa-output-native-plan.json \
  captures/m9128-device-inputs-20261006/normal-faa-output-known-inputs.bin
```

The constant-green variant uses its corresponding `input-manifest`,
`native-plan` and `known-inputs` files. Both pass. Independent negative
checks reject a wrong live source owner, a null callback promoted to a nonnull
address, and a changed final rectangle.

Root's physical constant-green control retires but retains black output and
the same main/companion hashes as the preceding source-init/WC control. This
weakens interpolation as the sole explanation; it does not identify those
records as colored fragments. Remaining source boundaries include actual
hardware cold setup outside the captured CPU flow and the hardware consumption
of GPU-produced opaque save/pool data. No new register, counter, cache operation
or record decoder follows from this audit.
