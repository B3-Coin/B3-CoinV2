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

  # core/: deterministic value types, codecs, tagged hashes, the error model
  # and code registries. bitcoin_consensus provides hash.cpp, uint256.cpp and
  # pubkey.cpp; bitcoin_crypto provides SHA-256.
  add_library(fm2_types STATIC
    core/amount.cpp
    core/codes.cpp
    core/hashes.cpp
    core/ids.cpp
    core/result.cpp
  )
  target_link_libraries(fm2_types
    PRIVATE
      core_interface
      fm2_warnings
    PUBLIC
      bitcoin_consensus
      bitcoin_crypto
      bitcoin_util
  )

  # Boost.Test suite of the outline. Every case is listed in test/INVENTORY.txt.
  add_executable(test_flowmesh2
    test/main.cpp
    test/amount_tests.cpp
    test/codec_tests.cpp
    test/codes_tests.cpp
    test/durable_tests.cpp
    test/hashes_tests.cpp
    test/ids_tests.cpp
    test/result_tests.cpp
    test/stub_inventory_tests.cpp
  )
  target_link_libraries(test_flowmesh2 PRIVATE
    core_interface
    fm2_warnings
    fm2_types
    bitcoin_consensus
    bitcoin_util
    Boost::headers
  )
  # Tests that read tracked files (the repository-wide tag scan) find the
  # source tree through this definition.
  target_compile_definitions(test_flowmesh2 PRIVATE FM2_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")

  set(FM2_TEST_TMP ${CMAKE_BINARY_DIR}/fm2-test-tmp)
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
    foreach(fm2_case IN ITEMS tsa_missing_lock ignored_result forge_token)
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
