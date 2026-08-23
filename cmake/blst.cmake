# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Vendored, pinned blst (src/blst, upstream v0.3.17 @ 54e6e556).
# Consensus dependency of crypto/bls (plan Commit 2, owner-approved); reached
# ONLY through that narrow wrapper — no other target includes blst.h.
#
# Portability contract (owner invariant, 2026-08-23): see the top-level
# CMakeLists.txt — the processor selects upstream's assembly path
# (x86_64/aarch64, C-compiler-assembled, __BLST_PORTABLE__) or upstream's
# pure-C path (everything else). Results are identical on both paths;
# consensus never depends on which one was compiled. B3_BLST_NO_ASM is set
# there (not a user option).

function(add_b3_blst)
  if(B3_BLST_NO_ASM)
    add_library(b3_blst STATIC EXCLUDE_FROM_ALL
      ${PROJECT_SOURCE_DIR}/src/blst/src/server.c
    )
    target_compile_definitions(b3_blst PRIVATE __BLST_NO_ASM__)
  else()
    add_library(b3_blst STATIC EXCLUDE_FROM_ALL
      ${PROJECT_SOURCE_DIR}/src/blst/src/server.c
      ${PROJECT_SOURCE_DIR}/src/blst/build/assembly.S
    )
    target_compile_definitions(b3_blst PRIVATE __BLST_PORTABLE__)
  endif()
  target_include_directories(b3_blst PUBLIC
    ${PROJECT_SOURCE_DIR}/src/blst/bindings
  )
  # Upstream code is compiled with its own conventions; do not apply the
  # project's warning set to it.
  target_compile_options(b3_blst PRIVATE -w -fno-builtin -fPIC)
  set_target_properties(b3_blst PROPERTIES C_VISIBILITY_PRESET hidden)
endfunction()
