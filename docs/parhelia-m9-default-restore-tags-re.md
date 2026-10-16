# Actual normal virtual restore-record tags

The normal default restore constructor really initializes its full0x181000
CPU-mapped pool. Every byte is sparse-valid in the retained host at physical
GPUa81000. Actual65430 copies the248-byte prefix from CPU source20f598 to
60a81000 through656a6. That prefix equals the Linux62-DWORD prefix exactly.
Actual65210 owner initialization selects record format1 and656c0; the
alternate65910 format2 builder is not the normal producer.

At actual65836/6584b/65892, descriptor+20 is ORed into three static pointer
fields: record DWORD9=work|tag, DWORD14=base|tag, DWORD19=(work+440)|tag.
The real descriptor tag is1. Comparing the entire known0x181000 pool to
`m9_restore_fill` rebased at the same captured base finds exactly3072
DWORD differences: these three fields in each of1024 records. Correcting
only these tags makes every known byte identical, including prefix, filler,
record headers, exception-record selection and all other endpoints.

The scoped patch `tools/native-faa-observation/virtual-restore-records.patch`
changes only those pointer fields for the private one-shot replay after the
locked drain, engine initialization and existing identity checks. All3072
original words are verified before any write; changed words are published
and read back. Ordinary Linux mode3 remains unchanged. No work shadow,
unknown allocation or completion counter is initialized by this conversion.

Regenerate the source comparison with vendor Python and
`tools/m9emu/m9_faa_restore_source.py`, canonical runtime/inventory/resources/DC
inputs, the current native `m9_requester.c` supplied as `--native-requester`,
and `--out` pointing at the retained compact JSON checkpoint. This runs the
real normal CPU frame and asserts whole-pool byte equality after exactly
those three tag conversions. The saved checkpoint includes actual copy and
record-producer events, known-span hash, selected raw records and producer
instruction pins. It does not assert GPU semantic equivalence or rendering.
