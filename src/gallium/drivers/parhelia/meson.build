# SPDX-License-Identifier: GPL-2.0-only
# Native Gallium driver and shared command builder.
libparhelia_batch = static_library(
  'parhelia_batch',
  files('parhelia_batch.c', 'm9_batch.c', 'm9_format.c', 'm9_state.c', 'parhelia_reference.c', 'reference/p3d_cs.c', 'reference/p3d_state.c',
        'reference/p3d_tile.c', 'reference/p3d_index.c', 'reference/p3d_clear.c',
        'reference/p3d_warp.c'),
  include_directories : [inc_include, inc_src, inc_gallium],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
libparhelia = static_library(
  'parhelia',
  files('parhelia_screen.c', 'm9_screen.c', 'parhelia_context.c', 'parhelia_fs_compile.c',
        'parhelia_vs_compile.c'),
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_libdrm, idep_mesautil, idep_nir],
  link_with : libparhelia_batch,
  gnu_symbol_visibility : 'hidden',
)
driver_parhelia = declare_dependency(
  compile_args : '-DGALLIUM_PARHELIA',
  link_with : [libparhelia, libparhelia_batch],
)
