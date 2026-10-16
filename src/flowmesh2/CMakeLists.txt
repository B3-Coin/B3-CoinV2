# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# This library is deliberately NOT linked by bitcoin_node, bitcoin_common,
# the wallet or Qt. Only the explicit default-OFF foundation test gate uses it.
add_library(flowmesh2_foundations STATIC mailbox.cpp read_slot.cpp)
target_link_libraries(flowmesh2_foundations PRIVATE core_interface bitcoin_util)
target_compile_options(flowmesh2_foundations PRIVATE
  $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wthread-safety;-Werror=thread-safety>
)

add_executable(test_flowmesh2_foundations foundation_tests.cpp read_slot_tests.cpp)
target_link_libraries(test_flowmesh2_foundations PRIVATE
  core_interface flowmesh2_foundations bitcoin_util bitcoin_consensus
)
target_compile_options(test_flowmesh2_foundations PRIVATE
  $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wthread-safety;-Werror=thread-safety>
)
add_test(NAME flowmesh2_foundations COMMAND test_flowmesh2_foundations)
set_tests_properties(flowmesh2_foundations PROPERTIES TIMEOUT 60 LABELS "flowmesh2;isolated;foundation")

# ---------------------------------------------------------------------------
# Rebirth outline (branch `rebirth`): see ARCHITECTURE.md and TODO.md.
# The C1 block above stays byte-identical. Everything below exists only with
# BUILD_FLOWMESH2=ON, which the top-level CMakeLists.txt accepts only together
# with BUILD_TESTS=ON and BUILD_FLOWMESH2_FOUNDATION_TESTS=ON. No target here is
# linked by bitcoin_node, bitcoin_common, b3coind, the wallet or Qt.
if(BUILD_FLOWMESH2)
  if(NOT TARGET Python3::Interpreter)
    message(FATAL_ERROR "BUILD_FLOWMESH2 requires a Python 3 interpreter for the flowmesh2 lint CTests")
  endif()

  # Linked PRIVATE by every fm2 target: lock annotations are errors on Clang,
  # and an ignored [[nodiscard]] Result is an error on every compiler.
  add_library(fm2_warnings INTERFACE)
  target_compile_options(fm2_warnings INTERFACE
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wthread-safety;-Werror=thread-safety>
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-Werror=unused-result>
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/we4834>
  )

  # core/: deterministic value types, codecs, tagged hashes, the provisional
  # profile, the error model and code registries, signed actions, slot
  # bodies, outcomes, facts, certificates, seat sets, the account leaf and
  # the shared nonce and fee spec functions, and the commitment v0 value
  # types (state_root). bitcoin_consensus provides hash.cpp, uint256.cpp and pubkey.cpp
  # (XOnlyPubKey::VerifySchnorr); bitcoin_crypto provides SHA-256 and BLS;
  # secp256k1 is the BIP340 verifier behind pubkey.cpp.
  add_library(fm2_types STATIC
    core/account_leaf.cpp
    core/action.cpp
    core/amount.cpp
    core/body.cpp
    core/certificate.cpp
    core/codes.cpp
    core/facts.cpp
    core/fee_math.cpp
    core/hashes.cpp
    core/ids.cpp
    core/market.cpp
    core/merkle.cpp
    core/nonce_rule.cpp
    core/outcome.cpp
    core/params.cpp
    core/result.cpp
    core/seat_set.cpp
    core/slot.cpp
    core/state_root.cpp
  )
  target_link_libraries(fm2_types
    PRIVATE
      core_interface
      fm2_warnings
      secp256k1
    PUBLIC
      bitcoin_consensus
      bitcoin_crypto
      bitcoin_util
  )

  # Application core (fm2_core). ledger/ (slice R5): the deterministic shared
  # spot ledger, its nonce windows and state commitment v0, in namespace
  # flowmesh2::ledger. engine/ (slice R6): the order book, the A2 sweep and
  # the residual-cross detection, in namespace flowmesh2::engine; engine.cpp
  # (slice R7): the canonical order, the validity predicate and the slot
  # phases. storage/ (slice R8): record framing, journals, strict commit,
  # durability tokens and the signer and commit logs, in namespace
  # flowmesh2::storage; crc32c frames the records. state/ (slice R9): the
  # core state, committed heads, slot candidates, the single-writer state
  # owner and decision replay, in namespace flowmesh2::state. Later slices
  # add the other application directories here. fm2_verify never links it
  # (guard rule 4), and it never links bitcoin_common (guard rule 4).
  add_library(fm2_core STATIC
    engine/book.cpp
    engine/engine.cpp
    engine/matcher.cpp
    engine/residual.cpp
    ledger/ledger.cpp
    ledger/nonce_window.cpp
    ledger/state_commitment.cpp
    state/candidate.cpp
    state/replay.cpp
    state/state_owner.cpp
    storage/commit_log.cpp
    storage/file_journal.cpp
    storage/journal.cpp
    storage/journal_commit.cpp
    storage/log_writer.cpp
    storage/memory_journal.cpp
    storage/record.cpp
    storage/seat_signer.cpp
    storage/signer_log.cpp
    storage/snapshot_store.cpp
  )
  target_link_libraries(fm2_core
    PRIVATE
      core_interface
      crc32c
      fm2_warnings
    PUBLIC
      fm2_types
  )

  # Boost.Test suite of the outline. Every case is listed in test/INVENTORY.txt.
  add_executable(test_flowmesh2
    test/main.cpp
    test/account_leaf_tests.cpp
    test/action_tests.cpp
    test/amount_tests.cpp
    test/authority_tests.cpp
    test/body_tests.cpp
    test/book_tests.cpp
    test/candidate_tests.cpp
    test/canonical_order_tests.cpp
    test/certificate_tests.cpp
    test/codec_tests.cpp
    test/codes_tests.cpp
    test/commit_log_tests.cpp
    test/commitment_tests.cpp
    test/durable_tests.cpp
    test/engine_property_tests.cpp
    test/engine_tests.cpp
    test/facts_tests.cpp
    test/fee_math_tests.cpp
    test/file_journal_tests.cpp
    test/hashes_tests.cpp
    test/ids_tests.cpp
    test/journal_tests.cpp
    test/ledger_property_tests.cpp
    test/ledger_tests.cpp
    test/market_tests.cpp
    test/matcher_reference_tests.cpp
    test/matcher_tests.cpp
    test/merkle_tests.cpp
    test/nonce_rule_tests.cpp
    test/nonce_window_tests.cpp
    test/outcome_tests.cpp
    test/params_tests.cpp
    test/record_tests.cpp
    test/replay_tests.cpp
    test/result_tests.cpp
    test/seat_set_tests.cpp
    test/seat_signer_tests.cpp
    test/signer_log_tests.cpp
    test/slot_tests.cpp
    test/state_owner_tests.cpp
    test/state_root_tests.cpp
    test/stub_inventory_tests.cpp
    test/validate_slot_tests.cpp
  )
  # bitcoin_common only for CKey (test/util/keys.h signs with test keys); the
  # fm2 libraries never link it (guard rule 4).
  target_link_libraries(test_flowmesh2 PRIVATE
    core_interface
    fm2_warnings
    fm2_core
    fm2_types
    bitcoin_common
    bitcoin_consensus
    bitcoin_util
    secp256k1
    univalue
    Boost::headers
  )
  # Tests that read tracked files (the profile JSON, the repository-wide tag
  # scan) find the source tree through FM2_SOURCE_ROOT; tests that write files
  # (the file journal, slice R8) use FM2_TEST_TMP_DIR.
  set(FM2_TEST_TMP ${CMAKE_BINARY_DIR}/fm2-test-tmp)
  target_compile_definitions(test_flowmesh2 PRIVATE
    FM2_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    FM2_TEST_TMP_DIR="${FM2_TEST_TMP}"
  )

  set(FM2_CONTRIB ${PROJECT_SOURCE_DIR}/contrib/flowmesh2)

  add_test(NAME flowmesh2_unit COMMAND test_flowmesh2 --log_level=test_suite)
  set_tests_properties(flowmesh2_unit PROPERTIES TIMEOUT 120 LABELS "flowmesh2;isolated;outline")

  add_test(NAME flowmesh2_inventory
    COMMAND ${Python3_EXECUTABLE} -I -B ${FM2_CONTRIB}/check_inventory.py
      --binary $<TARGET_FILE:test_flowmesh2>
      --inventory ${CMAKE_CURRENT_SOURCE_DIR}/test/INVENTORY.txt
      --workdir ${FM2_TEST_TMP}/inventory
      --selftest
  )
  set_tests_properties(flowmesh2_inventory PROPERTIES TIMEOUT 300 LABELS "flowmesh2;isolated;outline;lint")

  add_test(NAME flowmesh2_todo_lint
    COMMAND ${Python3_EXECUTABLE} -I -B ${FM2_CONTRIB}/check_todo.py
      --todo ${CMAKE_CURRENT_SOURCE_DIR}/TODO.md
      --workdir ${FM2_TEST_TMP}/todo_lint
      --selftest
  )
  set_tests_properties(flowmesh2_todo_lint PROPERTIES TIMEOUT 60 LABELS "flowmesh2;isolated;outline;lint")

  # -------------------------------------------------------------------------
  # Guards (slice R2): contrib/flowmesh2/check_guards.py, seven rules.
  if(NOT CMAKE_NM OR NOT CMAKE_AR)
    message(FATAL_ERROR "BUILD_FLOWMESH2 needs CMAKE_NM and CMAKE_AR for the flowmesh2 guard CTests")
  endif()

  add_test(NAME flowmesh2_guards
    COMMAND ${Python3_EXECUTABLE} -I -B ${FM2_CONTRIB}/check_guards.py
      --root ${PROJECT_SOURCE_DIR}
      --source
      --nm ${CMAKE_BINARY_DIR} --config $<CONFIG> --nm-tool ${CMAKE_NM}
  )
  set_tests_properties(flowmesh2_guards PROPERTIES TIMEOUT 300 LABELS "flowmesh2;isolated;outline;lint;guards")

  add_test(NAME flowmesh2_guards_selftest
    COMMAND ${Python3_EXECUTABLE} -I -B ${FM2_CONTRIB}/test_check_guards.py
      --root ${PROJECT_SOURCE_DIR}
      --workdir ${FM2_TEST_TMP}/guards_selftest
      --cxx ${CMAKE_CXX_COMPILER} --ar ${CMAKE_AR} --nm-tool ${CMAKE_NM}
  )
  set_tests_properties(flowmesh2_guards_selftest PROPERTIES TIMEOUT 300 LABELS "flowmesh2;isolated;outline;lint;guards")

  # Evidence tool (slice R2): contrib/flowmesh2/evidence.py run/verify.
  add_test(NAME flowmesh2_evidence_selftest
    COMMAND ${Python3_EXECUTABLE} -I -B ${FM2_CONTRIB}/test_evidence.py
      --workdir ${FM2_TEST_TMP}/evidence_selftest
  )
  set_tests_properties(flowmesh2_evidence_selftest PROPERTIES TIMEOUT 300 LABELS "flowmesh2;isolated;outline;lint;evidence")

  # Compile-fail controls (slice R2), Clang only: the lock control needs
  # Clang's thread-safety analysis. Each negative control is built twice, as
  # is (must fail with the diagnostic named in the file) and with
  # FM2_COMPILE_FAIL_FIXED (must compile), so the failure is exactly the
  # planted violation. The positive control uses the same flags and must
  # compile. All of them are EXCLUDE_FROM_ALL object libraries that only the
  # CTest builds; contrib/flowmesh2/check_negative_compile.py drives it.
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(fm2_compile_fail_targets fm2_cf_positive_control)
    add_library(fm2_cf_positive_control OBJECT EXCLUDE_FROM_ALL test/compile_fail/positive_control.cpp)
    foreach(fm2_case IN ITEMS tsa_missing_lock ignored_result forge_token sign_without_intent)
      add_library(fm2_cf_${fm2_case} OBJECT EXCLUDE_FROM_ALL test/compile_fail/${fm2_case}.cpp)
      add_library(fm2_cf_${fm2_case}_fixed OBJECT EXCLUDE_FROM_ALL test/compile_fail/${fm2_case}.cpp)
      target_compile_definitions(fm2_cf_${fm2_case}_fixed PRIVATE FM2_COMPILE_FAIL_FIXED)
      list(APPEND fm2_compile_fail_targets fm2_cf_${fm2_case} fm2_cf_${fm2_case}_fixed)
    endforeach()
    set(fm2_compile_fail_objects "")
    foreach(fm2_target IN LISTS fm2_compile_fail_targets)
      target_link_libraries(${fm2_target} PRIVATE core_interface fm2_warnings fm2_types)
      list(APPEND fm2_compile_fail_objects --object "${fm2_target}=$<TARGET_OBJECTS:${fm2_target}>")
    endforeach()

    add_test(NAME flowmesh2_negative_compile
      COMMAND ${Python3_EXECUTABLE} -I -B ${FM2_CONTRIB}/check_negative_compile.py
        --cmake ${CMAKE_COMMAND}
        --build-dir ${CMAKE_BINARY_DIR}
        --config $<CONFIG>
        --cases-dir ${CMAKE_CURRENT_SOURCE_DIR}/test/compile_fail
        --workdir ${FM2_TEST_TMP}/negative_compile
        ${fm2_compile_fail_objects}
    )
    # RUN_SERIAL: the test drives the build system of this build directory.
    set_tests_properties(flowmesh2_negative_compile PROPERTIES
      TIMEOUT 900 RUN_SERIAL TRUE LABELS "flowmesh2;isolated;outline;negative_compile"
    )
  endif()

  # Sanitizer canaries (slice R2), default OFF. One deliberately defective
  # program per sanitizer in SANITIZERS. Each must exit non-zero with its
  # sanitizer's report before any ctest result of the build is trusted; CI
  # (.github/workflows/flowmesh2.yml) and contrib/flowmesh2/evidence.py
  # (`run --expect nonzero`) run them first. Not CTests, so a canary can never
  # count as a passing test. Sources live outside src/flowmesh2/ because they
  # break the guard rules on purpose.
  option(FLOWMESH2_SANITIZER_CANARIES "Build the flowmesh2 sanitizer canaries (each must exit non-zero)." OFF)
  if(FLOWMESH2_SANITIZER_CANARIES)
    set(fm2_canaries "")
    if(SANITIZERS MATCHES "(^|,)address($|,)")
      list(APPEND fm2_canaries asan)
    endif()
    if(SANITIZERS MATCHES "(^|,)undefined($|,)")
      list(APPEND fm2_canaries ubsan)
    endif()
    if(SANITIZERS MATCHES "(^|,)thread($|,)")
      list(APPEND fm2_canaries tsan)
    endif()
    if(NOT fm2_canaries)
      message(FATAL_ERROR "FLOWMESH2_SANITIZER_CANARIES=ON needs SANITIZERS to contain address, undefined or thread (got '${SANITIZERS}')")
    endif()
    foreach(fm2_canary IN LISTS fm2_canaries)
      add_executable(fm2_canary_${fm2_canary} ${PROJECT_SOURCE_DIR}/contrib/flowmesh2/canaries/${fm2_canary}_canary.cpp)
      target_link_libraries(fm2_canary_${fm2_canary} PRIVATE core_interface)
    endforeach()
    message(STATUS "flowmesh2 sanitizer canaries: ${fm2_canaries}")
  endif()

  # -------------------------------------------------------------------------
  # KEEP THIS SECTION LAST: it enumerates every target of this directory.
  #
  # fm2_all builds every fm2 library and executable (check_guards.py --nm
  # requires each of them to exist). fm2_libs.manifest lists every target of
  # this directory (BUILDSYSTEM_TARGETS, so none can be left out) with its
  # type, file, objects and link closure; check_guards.py --nm reads it for
  # rules 3 and 4.
  #
  # fm2_link_closure() walks LINK_LIBRARIES and INTERFACE_LINK_LIBRARIES at
  # configure time. Every token of a generator expression that names a target
  # counts, so the closure over-approximates, which is the safe direction for
  # an exclusion check.
  function(fm2_link_closure target out_var)
    set(closure "")
    set(queue "${target}")
    while(queue)
      list(POP_FRONT queue current)
      get_target_property(aliased ${current} ALIASED_TARGET)
      if(aliased)
        set(current ${aliased})
      endif()
      if(current IN_LIST closure)
        continue()
      endif()
      list(APPEND closure ${current})
      get_target_property(imported ${current} IMPORTED)
      if(imported)
        set(props INTERFACE_LINK_LIBRARIES)
      else()
        set(props LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      endif()
      foreach(prop IN LISTS props)
        get_target_property(value ${current} ${prop})
        if(NOT value)
          continue()
        endif()
        string(REPLACE "::" "@@" text "${value}")
        string(REGEX REPLACE "[$<>:,;\"]" " " text "${text}")
        string(REPLACE "@@" "::" text "${text}")
        string(REGEX MATCHALL "[^ \t\n]+" tokens "${text}")
        foreach(token IN LISTS tokens)
          if(TARGET ${token})
            list(APPEND queue ${token})
          endif()
        endforeach()
      endforeach()
    endwhile()
    set(${out_var} "${closure}" PARENT_SCOPE)
  endfunction()

  get_property(fm2_dir_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  add_custom_target(fm2_all)
  set(fm2_manifest "# Generated by src/flowmesh2/CMakeLists.txt; read by contrib/flowmesh2/check_guards.py --nm.\n")
  string(APPEND fm2_manifest "format\tfm2-libs-manifest/1\n")
  foreach(tgt IN LISTS fm2_dir_targets)
    get_target_property(type ${tgt} TYPE)
    get_target_property(excluded ${tgt} EXCLUDE_FROM_ALL)
    if(excluded)
      set(in_all 0)
    else()
      set(in_all 1)
    endif()
    if(type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|EXECUTABLE)$")
      set(file "$<TARGET_FILE:${tgt}>")
      if(NOT excluded)
        add_dependencies(fm2_all ${tgt})
      endif()
    else()
      set(file "-")
    endif()
    string(APPEND fm2_manifest "target\t${tgt}\t${type}\t${in_all}\t${file}\n")
    if(type STREQUAL "STATIC_LIBRARY")
      string(APPEND fm2_manifest "objects\t${tgt}\t$<JOIN:$<TARGET_OBJECTS:${tgt}>,\t>\n")
    endif()
    if(NOT type STREQUAL "UTILITY")
      fm2_link_closure(${tgt} fm2_closure)
      list(JOIN fm2_closure "\t" fm2_closure_text)
      string(APPEND fm2_manifest "closure\t${tgt}\t${fm2_closure_text}\n")
    endif()
  endforeach()
  string(APPEND fm2_manifest "end\n")
  get_property(fm2_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(fm2_multi_config)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/fm2_libs-$<CONFIG>.manifest" CONTENT "${fm2_manifest}")
  else()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/fm2_libs.manifest" CONTENT "${fm2_manifest}")
  endif()
endif()
