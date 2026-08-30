# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(generate_setup_nsi output_exe)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  set(abs_top_builddir ${PROJECT_BINARY_DIR})
  set(CLIENT_URL ${PROJECT_HOMEPAGE_URL})
  set(CLIENT_TARNAME "b3coin")
  set(BITCOIN_WRAPPER_NAME "b3coin")
  set(BITCOIN_GUI_NAME "b3coin-qt")
  set(BITCOIN_DAEMON_NAME "b3coind")
  set(BITCOIN_CLI_NAME "b3coin-cli")
  set(BITCOIN_TX_NAME "b3coin-tx")
  set(BITCOIN_UTIL_NAME "b3coin-util")
  set(BITCOIN_WALLET_TOOL_NAME "b3coin-wallet")
  set(BITCOIN_TEST_NAME "test_bitcoin")
  set(EXEEXT ${CMAKE_EXECUTABLE_SUFFIX})
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WINDOWS_ARCH_TAG "win64")
    set(NSIS_ARCH_DEFINE "!define B3_INSTALL_WIN64")
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(WINDOWS_ARCH_TAG "win32")
    set(NSIS_ARCH_DEFINE "")
  else()
    message(FATAL_ERROR "Unsupported Windows pointer size: ${CMAKE_SIZEOF_VOID_P}")
  endif()
  set(WINDOWS_SETUP_BASENAME "b3coin-${WINDOWS_ARCH_TAG}-setup")
  configure_file(
    ${PROJECT_SOURCE_DIR}/share/setup.nsi.in
    ${PROJECT_BINARY_DIR}/${WINDOWS_SETUP_BASENAME}.nsi
    USE_SOURCE_PERMISSIONS @ONLY
  )
  set(WINDOWS_ARCH_TAG "${WINDOWS_ARCH_TAG}" PARENT_SCOPE)
  set(${output_exe} "${PROJECT_BINARY_DIR}/${WINDOWS_SETUP_BASENAME}.exe" PARENT_SCOPE)
endfunction()
