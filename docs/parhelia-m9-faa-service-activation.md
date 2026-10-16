# M9128 service and partition activation

A bounded actual allocator/ordinary queue run pins the M9128 CPU producers before any GPU input. Original `24710` reads selected configuration records `+310=[8,32,24,32]`, `+314=[4,8,8]`. These satisfy the original valid-input branches; no fallback is required. It produces queue `+4c/+50/+54 = 00280800/1f3f2707/00070703`. A second actual invocation preserves that tuple. `23888` calls this producer before `2388f` calls requester activation; stores `23b50/23b5f/23b6f` publish the three words. Current native controls match this admitted tuple. An original-instruction CPU fixture also checks distinct valid inputs and the invalid-input fallback; those synthetic ABI records never enter the actual allocator or hardware input model.

The actual service prefix `344..354`, lanes `334..340`, and `330=1000` matches native. `2611d` invalidates exactly 20 slots `0..130` with `ffff`, so the suspected slot-count difference is absent. These findings narrow particular native M9148-derived choices for this M9128 configuration. They do not establish arbitrary-board fallback parity, complete Windows cold admission, or successful FAA resolve.

The full later frame also publishes twelve live resource descriptors at slots `0..b0` through `25d96/25dd2/25e0e/25e73`. Their producers consume live resource fields and ownership; this note does not infer a GPU prerequisite from their address values. CPU aperture/window use and cold hardware state are separate contracts. No native change follows from this checkpoint.

Check the retained source run and original CPU producer controls:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-service-activation.py \
  --capture captures/m9128-device-inputs-20261006/normal-faa-service-activation.json \
  --runtime-tools /home/ai/src/parhelia/xf86-video-parhelia/tools/faaemu \
  --native-dir /home/ai/src/parhelia/src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm/m9x
```

The capture retains exact source PCs/order, full producer hash, actual property input records, and actual M9128 PCI/resource input hashes. Its scope explicitly excludes completed miniport cold initialization, GPU execution and completion replies.
