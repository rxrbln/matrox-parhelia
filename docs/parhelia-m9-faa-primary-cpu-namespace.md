# Sampled-primary CPU address namespace

The captured `{GPU|1,f0220140,00010100}` descriptor requires a direct CPU
root mapping in addition to the compact GPU leaf and the mapping of that
leaf's storage. Those entries have different indices in the same root child.

The genuine M9128 allocator publishes these distinct tables:

| Table | Physical base | Published registers |
| --- | --- | --- |
| Primary compact leaves | `401000` | `132c/1330` |
| Root child | `901000` | `0360=901000,0364=980000` |
| Additional child | `981000` | `0f64=981000,0f68=a80000` |

The actual CPU mapper `26830 -> 597a0 -> 59410` copies 80 primary entries
into the **0360 root child**, using the surface address's page index for both
source and destination. This destination is not the additional `0f64` child.
For the sampled surface, index `ed1` begins with `ed1008,ed2008,ed3008`.
`594a0/594a2` read and write the real words, and the observed destination after
the copy equals all 80 source words. The copy is idempotent in this fixture.

A passive guest-memory observer establishes why those entries already match:
actual initializer store `5b6c2` executes 65536 DWORD writes across root CPU
alias `60901000..60941000`. The first words are `8,1008,2008`; the last is
`0ffff008`. The actual loop count is 65536. Instructions `5b6c0..5b6df`
preserve the low 12 flag bits and advance the physical page by `1000`.
These are vendor CPU instruction outputs, not values supplied by an OS or GPU
model. This finite constructor initializes direct identities over 256 MiB.

At `594b6`, the mapper passes packed token `30000c8f` to `59af0`, whose actual
`59b82` stores emit `31c=f,0,c` before the aperture descriptor is published.
The earlier public map synchronization writes `314=1` at `2cba8`. No `318`
write occurs in these allocation/map outputs. The descriptor's low selector
is also conditional: `25cf4` first strips it; actual property getter results
at `25d14/25d22` are `101b8`, whose bits `8` and `20` select `25d26` to
restore the layout's page offset and selector into the host backing address.
The alternate `59aa0` software-translation branch is not taken here.

The native identity checker proves a compact leaf such as
`primary_origin + (GPU >> 12)*4` and its storage page's root. It does not prove
`root_base + (GPU >> 12)*4`. The native constructor previously initialized
root identities only for the root allocation itself. A surface above that
allocation can therefore pass the GPU identity checker while its direct CPU
root is zero. The scoped private fix copies the verified leaf into each
direct root after checking the complete extent, existing entries and actual
root register pair, then emits the captured `f,0,c` sequence. It refuses
conflicting mappings and stays within the observed `40000`-byte child.

`tools/check-m9-faa-primary-namespace.py` checks the genuine constructor
stores, source instructions, distinct register publications, full copy,
conditional selector and flush in
`docs/traces/m9-vendor-faa-primary-namespace-20261006.json`.

This proves the CPU mapper's additional namespace join. Whether GPU FAA
fetches require the same direct identities, or a separate indexed bank,
requires its own address-domain proof. CPU mapping repair and real GPU
retirement do not establish a rendered FAA image.
