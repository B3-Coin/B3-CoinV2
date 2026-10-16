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
