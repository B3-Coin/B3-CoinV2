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
