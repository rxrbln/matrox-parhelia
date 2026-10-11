# Derive GPU commands from live state

Standing user instruction, 2026-10-06: do not replay captured parameters and
memory addresses one for one. Apply this to every GPU family, emulator
capture, private diagnostic and application implementation in this project.

A capture proves the vendor's command structure, encoding and ordering for
its particular inputs. A native producer must derive commands from its own
live resources and state. Copying a complete captured command stream and
patching only the recorded relocation callbacks is insufficient.

- Resolve every address-bearing field to a live retained allocation, its
  address domain, view offset and encoding. This includes direct register
  writes, embedded page indices, packed address ranges and resource metadata
  outside ordinary relocation callbacks. Captured pointers, PFNs, physical
  addresses, GPU virtual addresses and allocation offsets are not native
  addresses. Use the hardware ownership and mapping contracts for the actual
  family and device.
- Recompute allocation-dependent parameters: dimensions, pitch, format and
  view descriptors, extents, page masks and counts, command lengths, range
  endpoints, restore pointers and timeline values. If alignment changes the
  number of packets or words, rebuild the segment and adjust the remaining
  stream and submission length.
- Retain a captured field as a constant only when source evidence proves
  its role as a stable opcode, encoding constant, program byte or immutable
  input for the admitted case. A bounded diagnostic may retain a fixed size
  or state only with explicit guards and documented scope. Do not present
  that diagnostic as support for arbitrary application state. Address-looking
  shader or state words are not addresses without a proved producer/consumer.
- Preserve the actual vendor initialization, state transitions and resource
  lifetime through real retirement. Copy only source-known input bytes;
  unknown bytes are not a license to invent initialization, mapping metadata
  or completion. Derive runtime inputs from the correct device or keep the
  unsupported case gated.
- Verify with distinct native placements and relevant alignment changes,
  using actual vendor instruction or source evidence as the reference.
  Prove bounds, domain joins, packet counts, suffix preservation and rejection
  before GPU submission. Hardware retirement and correct rendered pixels
  remain separate requirements.

The M9128 companion clear demonstrates why this rule matters. Its 151-page
extent produces 19 clear words at captured address `0x1021000`, but 20 at
native address `0x2f0e000`. The page indices are emitted directly by `b5e70`
outside the relocation callback. Retaining those captured words targets the
wrong pages; adding a constant address delta preserves the wrong masks and
count. The native producer must generate the actual page runs and update
the primary length. These addresses are explanatory historical examples,
not values to reuse in another implementation.

See [the source and helper proof](parhelia-m9-faa-companion-page-clear.md).
