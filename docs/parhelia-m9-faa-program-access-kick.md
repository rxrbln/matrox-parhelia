# Original C2 access kick before preprocessing

The source's local program access has a frontend kick independent of its CPU
aperture leaf copy. Actual `2cb40` is called with `[program,0,1]`; the nonzero
third argument takes `2cb81`. It calls the resource's two wait methods, each
with argument1, until each returns zero. After both return, `2cb9f..2cbaf`
loads the resource's live hardware register base and writes register314=1.
This write dominates all subsequent CPU-view selection/copy code. It is
unconditional **after those waits on this admitted caller path**, not proof
that arbitrary calls or waits can be bypassed.

The passive original180 observation joins that actual program object to its
resource `+198` local GPU base and `+1bc=1080` logical extent. Four typed
kind19/20 operands select the same program: C2/C3 at source30 offsets130/134
use program+32; offsets454/458 use program+0. Their actual object and source
relocations derive unique local owner26. It has240 known program bytes and
4224 logical bytes; a native page-rounded backing is8192 bytes. Kind25 is the
separate RS database and must not be included in program-owner derivation.
Captured object/GPU addresses are comparisons only, never native inputs.

The subsequent `59410` operation is different: its pretoken0 emits no31c
stores; actual `594a2` copies two existing GPU leaves into a CPU-aperture
child, then its selected post policy emits31c=f,0,c. Actual `2c840` calls
`27100` to release this CPU view and writes31c=0. Both depend on an active
CPU view. Native BAR upload does not use that alias. The current
`m9x_address_cpu_local_prepare` performs CPU mirror publication; it is not a
reason to install an unused alias or replay that31c sequence during native
program access. The source's alternate optimized-copy branch is explicitly
unqualified here.

A narrow complete180 diagnostic can isolate the independent kick: after a
**real** phase0/c0 retirement, derive the program owner from its typed C2
sites, validate its retained local extent/identity and unchanged known upload,
order prior CPU writes, then issue the existing named FRONTEND_KICK primitive
with value1 before extending the retained primary to120. It must not install
a CPU alias, advance a completion counter, emit31c tokens or change the source
shader/program. Any failed guard retains already-posted owner quarantine.
The original program wait deadline is0; the native c0 wait is stronger and
does not reproduce the original CPU scheduling. This is a causal diagnostic
candidate, not proof that the kick controls a particular GPU cache or fixes
FAA. The native code already uses the same primitive after output retirement.

Kernel6fe8ead implements this opt-in control. Its
[fresh-boot hardware test](../tests/m9-faa-program-kick-hardware-20261007/README.md)
confirms the kick after actualc0, but the final output remains black. Main,
companion and sampled-primary hashes exactly match the preceding no-kick run.
The added source transition alone is insufficient; CPU-alias31c operations
and the public resolve lifecycle remain separate questions.

`tools/check-m9-faa-program-access-kick.py` joins the unchanged full180
manifest/plan, all nine reproduced source-export hashes, the original phase
mapping observation, actual resource fields/program operands and shipping
instruction pins. It supplies no new GPU completion, fragment or pixels.

```sh
python3 tools/check-m9-faa-program-access-kick.py \
  --image xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll \
  --out /tmp/faa-program-access-kick.json
```
