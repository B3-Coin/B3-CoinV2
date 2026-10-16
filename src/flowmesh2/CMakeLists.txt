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

  # core/: deterministic value types, error model and code registries.
  add_library(fm2_types STATIC
    core/codes.cpp
    core/result.cpp
  )
  target_link_libraries(fm2_types
    PRIVATE
      core_interface
      fm2_warnings
    PUBLIC
      bitcoin_util
  )

  # Boost.Test suite of the outline. Every case is listed in test/INVENTORY.txt.
  add_executable(test_flowmesh2
    test/main.cpp
    test/codes_tests.cpp
    test/result_tests.cpp
    test/stub_inventory_tests.cpp
  )
  target_link_libraries(test_flowmesh2 PRIVATE
    core_interface
    fm2_warnings
    fm2_types
    bitcoin_util
    Boost::headers
  )

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
endif()
