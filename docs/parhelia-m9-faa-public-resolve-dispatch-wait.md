# Actual public resolve dispatch before pending cleanup

The genuine public shared-primary/new-depth flow has a distinct physical
receipt in `tests/m9-faa-public-flush-hardware-20261007/`: M9128 boot
`ebedad89-d818-4f6b-bdf9-58fbce68370d`, kernel `e316f3f`, module SHA256
`f31736b671cb0615410fc1488cfa34e0d325d5a1615b2bfd3e439f1948193058`.
Its exact source profile is `public-shared-depth-flush-wait`, 8792 primary
bytes, 34 retained owners, 50 native fixups and 30 timeline producers. It
retires `1a0` with actual channel-zero `1a1/0`; the other three channels remain
`10/0`. No other same-numbered in-place receipt is substituted.

[The controller](../tools/m9emu/m9_faa_public_flush_receipt_continuation.py)
reconstructs the original stopped public request and checks the entire signed
source prefix before restoring the original `aa750` stack. It supplies only
that actual four-byte `1a1` channel-zero read at `252b9`, requested `190`.
Opaque BAR records, unknown mapped MDL bytes, pixels and later deadlines remain
guarded. The actual `aa750` return PC is derived from its observed caller stack
and checked as `a749c`; the controller would stop there before replacement
draw execution, rather than insert a helper call.

The `190` wait precedes the actual resolve render dispatch. With the real
receipt, the original path completes its format preparation at `fa865`, enters
`f8e36`, and returns to the `aa750` resource-use publications at `aae3b`,
`aae4e`, and `aae66`. Those calls each publish deadline `1c0` for the retained
main, companion and primary. This establishes CPU emission and returned render
dispatch, not GPU image completion. The subsequent conditional cleanup enters
`a5ae0/a7ce0` on the retained sentinel and stops at another original `252b9`
read requiring `1c0`. `aa750` has not returned and the replacement triangle has
not run. The actual `1a1` receipt is not sufficient for that newer deadline.

The [whole exporter](../tools/m9emu/m9_faa_public_resolve_capture.py) retains
the complete independent `public-shared-depth-resolve-wait` prefix, not a
spliced rectangle or footer. It proves every earlier consumed LOCAL image and
SYSTEM span, including the previous 8792-byte posted primary, is byte-exact.
The original continuation appends 620 primary bytes and makes 20 typed
callbacks; zero opaque GPU bytes and zero pixels are supplied. No endpoint
MMIO post occurs in that continuation before the newer wait. The genuine
independent `23b80` finish then appends its own 12-byte footer and requests
`1d0`, stopping before the real `23bc3` counter load.

The resulting source primary is exactly 9424 bytes, with 107 actual typed
callbacks, 34 chronological timeline producers and six direct secondary
endpoints. It retains the same sampled destination graph and has no ordinary
SYSTEM output owner. Its cleanup/public caller and CPU copy remain pending;
the marker distinguishes returned mode-nine dispatch from returned `aa750`.
The final mode-nine primary AB/AC words are actual literal zeros. They are
preserved; the earlier triangle's source-derived inactive secondary mask
normalization does not authorize rewriting these different producer words.

Native generation must preserve the four already qualified boundaries
`c0/120/140/1a0` and add the source-produced final `1d0`, with live clear rebasing
and real owner/domain encoding. The separate strict checker and actual-C
placement proof qualify that native plan. A new actual `1d0` receipt is needed
before continuing original cleanup. This source checkpoint establishes the
genuine public resolve launch which was absent from the earlier public `1a0`
prefix; it makes no rendered-pixel or completed public-copy claim.
