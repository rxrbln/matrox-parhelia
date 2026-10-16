# Q3 current-attribute cost witness

The actual fullscreen Q3 capture contains five steady gameplay profile periods before
its timedemo boundary. Each reports 360 zero-stride attribute fallbacks per 60
frames, or six draws/frame. The same periods contain about 55 jobs/frame,
118–120 draws/frame, 6.36–6.53 ms in emission and 5.67–5.78 ms in submit ioctls.
The driver reports averages for jobs/draws/records, but its native/fallback
counts are totals across the 60-frame period. Host scopes overlap and these
numbers do not isolate constant-attribute transforms or GPU execution time.
Later menu rows are excluded. Startup and the transition period with 392 refusals are excluded from this
recurring witness.

The separately retained Present observer shows one full GPU CopyArea per
presentation and no CPU detile/readback/upload/temp branch. Its first 360
copies average about 73 us in the copy host scope and 133 us in the flush host
scope. These are host timings, not the time the full-screen GPU copy takes.
[The full run](../tests/m9-present-fullscreen-20261007/README.md) remains the
physical baseline and includes its artifact/profile limits.

The separate Mesa candidate `cd82244` addresses this actual repeated refusal.
Mesa's original state tracker `st_setup_current` uploads current GL attributes
and gives them source stride zero; the user-pointer path has the same stride.
The packer now decodes one bounded live texel per draw and copies it into the
ordinary positive-stride source records. Hardware declarations, programs,
queued arena snapshots and completion ownership retain their existing paths.
The compiler-test gate independently compares mixed per-vertex/constant and
all-constant records, normalized/default components and live mutations; old
source refuses the new focused test. This improves GPU transform admission,
without changing ISA support or raising shader caps. A later existing refusal
may still keep an affected draw on the CPU.

The record/input references, 86 compiled shader references, five negative
admission references, complete DSO and batch gates pass. A public 1024x768
fixture with changed current colours in ordinary and indexed draws passes
0/1572864 wrong pixels on llvmpipe. Native hardware pixels and the performance
change are pending. The software oracle cannot establish native completion.

```sh
python3 tools/m9-q3-current-attribute-cost-check.py
```

`tests/m9-q3-current-attribute-cost-20261007/proof.json` retains exact profile
rows and the compressed capture SHA256. No captured GPU parameter or address
is used as a native input. Async display-start scheduling/old-owner release
remains separately open; this candidate advertises no asynchronous flip cap.
