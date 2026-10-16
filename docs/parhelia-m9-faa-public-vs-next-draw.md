# Retained public-VS graph: second DRAW and Lock source

The M9128 source controller resumes the returned first public-VS Unlock only
after the actual same-boot `0x1b0` retirement. The qualifying run used boot
`e10c5825-c2b9-4ede-b5be-10fda53c056d`, module SHA
`157614b9b0cc32336df2f58df3665fba7a99df470c4eb2eec9b9aa340c76a691`,
and helper SHA
`3642827037d6b89ea78957d606a05ef11998bdd6aeb8955b0cafac73dae01775`.
The helper released that test graph after the receipt. The emulator continues
the **returned Unlock CPU state before the independent finisher's destructive
stack**, modeling a future native run that retains its own graph. This does
not assert that the already released hardware graph can be used again.

The next public request keeps the compiled 328-byte vertex/fragment program,
its shader binding, and the six graph owners. Its caller sends only genuine
DP2 SETSTREAMSOURCE (stream 0, stride 32) and DRAW (triangle list, one
primitive). The three new clip-space vertices occupy a 96-byte eight-float
stream; the vendor writes it at retained owner 28 offset `[512,608)`. No
CREATEVS, SETCONST, SETVS, program recompile, or program upload occurs.
The public DRAW returns `EAX=1` with the existing context.

The source writes secondary owner 29 `[2688,5236)` and primary owner 27
`[8672,8732)` for DRAW. It posts one END at 8732. The immediately following
normal Lock writes primary `[8732,12304)`, posts END 12292 twice and END
12304 once, then reaches the first real CPU completion read at `4e1d1`,
desired `0x280`. That read is **not executed**. The native primary cursor has
the already proven +4 clear-alignment delta; corresponding live END offsets
are 8736, 12296 twice, and 12308. The source DRAW's last E3 token is
`0x251`, so its first END targets `0x250` from the real Unlock `0x1b0`.

The three `252b9` reads with desired `0x1e0`, `0x200`, and `0x230` occur
before the next END; the fourth asks for `0x260` after the DRAW END. Disassembly
of `25270` establishes that these reads encode GPU dependencies and do not
wait on the CPU. The controller uses the actual `0x1b1` observation. A
separate CPU-only branch control supplies hypothetical `0x251` at the fourth
read and produces identical packet bytes, END order, and final unread
`0x280` boundary. This control is not a hardware receipt.

[`plan.json`](../captures/m9-faa-graph-public-vs-20261007/next-draw-1b0/plan.json)
strips 42 secondary owner addresses, four primary secondary pointers, 13
completion tokens, three `43` GPU-dependency targets, three relative offsets,
six fixed SYSTEMVA fields, and two dimension words from the draw template.
The four program calls select retained entries 4, 11, 16, and 0. The six
`0x40004000` source immediates are valid only when that SYSTEMVA is mapped to
the live retained triangle backing; they are explicit admission fields, not
portable addresses. The live fragment input range is `[16,32)` in owner 28,
not the frozen source utility-red range `[0,32)`, so native must regenerate
the range/count at secondary 4908/4912/4916 from the accepted input layout.

The Lock suffix is byte-identical to the first public-VS Lock's 3572-byte
source after its typed address, timeline, extent and null fields are masked,
except the additional draw dependency word at source primary offset 12280
(`0x260` instead of `0x170`). Its encoder can be reused with that live
dependency and the new cursor/epoch. The source-qualified target remains
`0x280`; no Lock return, F021 view, or second image is claimed yet.

Run the host proof with
`python3 tests/m9-faa-graph-public-vs-20261007/check-next-draw1b0.py`.
It checks exact source reconstruction, a second set of live owner placements,
the live FS range, Lock suffix reuse, and nine refusal mutations. All
allocation-dependent fields must be derived from retained native owners and
caller state under the [capture/replay policy](../xf86-video-parhelia/docs/gpu-capture-replay-policy.md).
