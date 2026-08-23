# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Vendored, pinned blst (src/blst, upstream v0.3.17 @ 54e6e556) —
# BENCHMARK-ONLY (owner ruling 2026-08-23). Built only when
# B3_FINALITY_BENCH=ON; never linked into node, wallet, tests or consensus.
# __BLST_PORTABLE__ keeps the arithmetic path independent of CPU features.
function(add_b3_blst)
  add_library(b3_blst STATIC EXCLUDE_FROM_ALL
    ${PROJECT_SOURCE_DIR}/src/blst/src/server.c
    ${PROJECT_SOURCE_DIR}/src/blst/build/assembly.S
  )
  target_include_directories(b3_blst PUBLIC
    ${PROJECT_SOURCE_DIR}/src/blst/bindings
  )
  target_compile_definitions(b3_blst PRIVATE __BLST_PORTABLE__)
  # Upstream code is compiled with its own conventions; do not apply the
  # project's warning set to it.
  target_compile_options(b3_blst PRIVATE -w -fno-builtin -fPIC)
  set_target_properties(b3_blst PROPERTIES C_VISIBILITY_PRESET hidden)
endfunction()
