# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# CTest flowmesh2_sim_cli_determinism (slice R14, ARCHITECTURE.md 14.4 P6):
# runs the simulator CLI (fm2-sim) twice with the same arguments and
# requires exit status 0 both times, a non-empty output and byte-identical
# stdout. The outputs stay in WORKDIR for inspection.
#
#   cmake -DSIM=<path to fm2-sim> -DWORKDIR=<dir> "-DSIM_ARGS=<a;b;...>" -P sim_determinism.cmake
#
# This file lives outside src/flowmesh2/ (which has exactly one CMake file)
# and names no CMake target (guard rule 4): the binary comes in as SIM.

cmake_minimum_required(VERSION 3.22)

foreach(var IN ITEMS SIM WORKDIR SIM_ARGS)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "sim_determinism: ${var} is not set")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

foreach(run IN ITEMS 1 2)
  execute_process(
    COMMAND "${SIM}" ${SIM_ARGS}
    OUTPUT_FILE "${WORKDIR}/run${run}.json"
    ERROR_VARIABLE stderr_text
    RESULT_VARIABLE status
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "sim_determinism: run ${run} exited with '${status}': ${stderr_text}")
  endif()
endforeach()

file(SIZE "${WORKDIR}/run1.json" size1)
file(SIZE "${WORKDIR}/run2.json" size2)
if(size1 EQUAL 0)
  message(FATAL_ERROR "sim_determinism: the first run printed nothing")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${WORKDIR}/run1.json" "${WORKDIR}/run2.json"
  RESULT_VARIABLE differ
)
if(NOT differ EQUAL 0)
  message(FATAL_ERROR "sim_determinism: the two runs differ (${size1} and ${size2} bytes)")
endif()
file(SHA256 "${WORKDIR}/run1.json" digest)
message(STATUS "sim_determinism: two runs, ${size1} bytes each, identical; SHA-256 ${digest}")
