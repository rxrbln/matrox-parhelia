# M9128 second returned-DRAW tail and independent finish

The actual `0x1e0` target receipt qualifies the original second public DRAW
stack, which returns `EAX=1`. The original M9128 producer then writes 692
bytes at secondary owner 29 `[2040,2732)` and a 12-byte primary indirect
packet at owner 27 `[8668,8680)`, posting `COMMAND_END` at source 8680.
The independent genuine queue finish writes a 16-byte secondary footer
`[2732,2748)` and a second 12-byte primary indirect packet
`[8680,8692)`, posts END at source 8692, and stops before reading the
common-buffer counter for desired `0x210`. Both primary ENDs use the same
retained graph and command-base namespace. The native producer must use its
live cursor; the current graph has a four-byte dynamic clear delta, giving
native endpoints 8684 and 8696.

The compact source plan is
`captures/m9-faa-next-frame-tail-20261007/native-plan.json`; its stripped
732-byte blob contains primary 24 bytes then secondary 708 bytes. It has 17
typed secondary address encodings, four direct primary endpoints, two
320x240 descriptor words, and three timeline values at prefix+`0x10`,
`0x20`, `0x30`. Every address-bearing and timeline word is zeroed in the
blob and must be generated from the retained 31 live owners and actual
transport point. The 708 secondary bytes match the earlier returned-DRAW
opcode structure only after both independent source traces have had all
address, size, and timeline fields stripped. The changed triangle's 60-byte
geometry was supplied to the prior `0x1e0` producer; this tail reuses the
same retained owner 28 and shader state.

This is source-known input for a bounded 320x240 red control. No actual
`0x210` retirement, second normal Lock, changed image, or pixel correctness
is claimed. The native encoder has distinct placement/timeline and atomic
refusal tests. Hardware must separately prove the two-post order and final
retirement before continuing the original normal Lock.


Native rendering continuation is now hardware verified. Kernel WIPf14553e
(canonical791ad1b) adds private ioctl0x14 NEXT_FINISH from exact1e0. Firmware
SHA4fc27c24... is authenticated before allocation/encoding; retained owner
bindings and SYSTEM PTEs are rechecked. All six write fences precede the two
ordered ENDs; failures after a post quarantine the fence and graph owners.
Actual-function ASAN/UBSAN tests cover19 stale/PTE refusals and before/after
failures at both posts. Encoder17 placement/epoch controls and16 refusals pass.

Fresh M9128 bootab573610-efea-490c-8e4a-0ff77fe0fe1f completed the entire
INITb0 → firstDRAW/Lock170 → view → Unlock190 → secondprefix1e0 → second
renderingfinish210. raw00=211, submitted=completed210,next220, generation0,
queue0,wedged0, busmaster1 immediately and two seconds later. Actual native
primary8696B/posts14 preserve the dynamic clear delta. ModuleSHA d068e3ab...,
helperSHA0422bcd1.... Evidence is under tests/m9-faa-next-frame-tail-20261007/
hardware-20261007 and check-hardware.py verifies eight altered-receipt refusals.
The saved raw image is still the FIRST Lock image; second Lock and changed
second pixels require the next source continuation and separate hardware proof.
