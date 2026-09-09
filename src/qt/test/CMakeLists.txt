# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

set(CMAKE_AUTOMOC_MOC_OPTIONS "-p${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_bitcoin-qt
  apptests.cpp
  b3shelltests.cpp
  b3splashtests.cpp
  b3tradetests.cpp
  optiontests.cpp
  rpcnestedtests.cpp
  test_main.cpp
  uritests.cpp
  util.cpp
  ../../init/bitcoin-qt.cpp
)

add_windows_application_manifest(test_bitcoin-qt)

target_link_libraries(test_bitcoin-qt
  core_interface
  bitcoinqt
  test_util
  bitcoin_node
  Boost::headers
  Qt6::Test
)

import_plugins(test_bitcoin-qt)

if(ENABLE_WALLET)
  target_sources(test_bitcoin-qt
    PRIVATE
      addressbooktests.cpp
      b3assettests.cpp
      b3dashboardtests.cpp
      b3hardeningtests.cpp
      b3stakesettingstests.cpp
      wallettests.cpp
      ../../wallet/test/wallet_test_fixture.cpp
  )

  # Focused, headless form regression: no node/wallet startup or full GUI suite.
  add_executable(test_b3_sendcoinsentry-qt b3sendcoinsentrytests.cpp)
  target_link_libraries(test_b3_sendcoinsentry-qt
    core_interface
    bitcoinqt
    bitcoin_node
    Qt6::Test
  )
  import_plugins(test_b3_sendcoinsentry-qt)
  add_test(NAME test_b3_sendcoinsentry-qt COMMAND test_b3_sendcoinsentry-qt)
  set_tests_properties(test_b3_sendcoinsentry-qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=minimal"
  )

  # Pure amount/transaction-review checks: no wallet, RPC, node or GUI startup.
  add_executable(test_b3_flowmeshtrading-qt b3flowmeshtradingtests.cpp)
  target_link_libraries(test_b3_flowmeshtrading-qt
    core_interface bitcoinqt bitcoin_node univalue Qt6::Test
  )
  import_plugins(test_b3_flowmeshtrading-qt)
  add_test(NAME test_b3_flowmeshtrading-qt COMMAND test_b3_flowmeshtrading-qt)

  add_executable(test_b3_flowmeshtradingpanel-qt b3flowmeshtradingpaneltests.cpp)
  target_link_libraries(test_b3_flowmeshtradingpanel-qt
    core_interface bitcoinqt test_util bitcoin_node Boost::headers Qt6::Test
  )
  import_plugins(test_b3_flowmeshtradingpanel-qt)
  add_test(NAME test_b3_flowmeshtradingpanel-qt COMMAND test_b3_flowmeshtradingpanel-qt)
  set_tests_properties(test_b3_flowmeshtradingpanel-qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=minimal"
  )

  # Synthetic certified snapshots only: exact units, curves, empty/stale UI;
  # no node, wallet, sockets, signing or mainnet state.
  add_executable(test_b3_flowmeshworkspace-qt b3flowmeshworkspacetests.cpp)
  target_link_libraries(test_b3_flowmeshworkspace-qt
    core_interface bitcoinqt bitcoin_node univalue Qt6::Test
  )
  import_plugins(test_b3_flowmeshworkspace-qt)
  add_test(NAME test_b3_flowmeshworkspace-qt COMMAND test_b3_flowmeshworkspace-qt)
  set_tests_properties(test_b3_flowmeshworkspace-qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=minimal"
  )

  add_executable(test_b3_flowmeshop-qt b3flowmeshoptests.cpp)
  target_link_libraries(test_b3_flowmeshop-qt
    core_interface bitcoinqt bitcoin_node univalue Qt6::Test
  )
  import_plugins(test_b3_flowmeshop-qt)
  add_test(NAME test_b3_flowmeshop-qt COMMAND test_b3_flowmeshop-qt)

  add_executable(test_b3_assettransfer-qt b3assettransfertests.cpp)
  target_link_libraries(test_b3_assettransfer-qt
    core_interface bitcoinqt bitcoin_node univalue Qt6::Test
  )
  import_plugins(test_b3_assettransfer-qt)
  add_test(NAME test_b3_assettransfer-qt COMMAND test_b3_assettransfer-qt)

  # Isolated FN-panel lifecycle and relock checks, with no mining/network.
  add_executable(test_b3_flowmeshpanel-qt b3flowmeshpaneltests.cpp)
  target_link_libraries(test_b3_flowmeshpanel-qt
    core_interface bitcoinqt test_util bitcoin_node Boost::headers Qt6::Test
  )
  import_plugins(test_b3_flowmeshpanel-qt)
  add_test(NAME test_b3_flowmeshpanel-qt COMMAND test_b3_flowmeshpanel-qt)
  set_tests_properties(test_b3_flowmeshpanel-qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=minimal"
  )

  # Isolated encrypted-wallet staking unlock checks, with no mining/network.
  add_executable(test_b3_stakingunlock-qt b3stakingunlocktests.cpp)
  target_link_libraries(test_b3_stakingunlock-qt
    core_interface
    bitcoinqt
    test_util
    bitcoin_node
    Boost::headers
    Qt6::Test
  )
  import_plugins(test_b3_stakingunlock-qt)
  add_test(NAME test_b3_stakingunlock-qt COMMAND test_b3_stakingunlock-qt)
  set_tests_properties(test_b3_stakingunlock-qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=minimal"
  )

  # Fast view-only regression/gallery: no wallet startup, mining or networking.
  add_executable(test_b3_uipreview-qt
    b3uipreviewtests.cpp b3shelltests.cpp b3dashboardtests.cpp b3assettests.cpp
    b3stakesettingstests.cpp b3tradetests.cpp b3hardeningtests.cpp
  )
  target_link_libraries(test_b3_uipreview-qt
    core_interface bitcoinqt test_util bitcoin_node Boost::headers Qt6::Test
  )
  import_plugins(test_b3_uipreview-qt)
  add_test(NAME test_b3_uipreview-qt COMMAND test_b3_uipreview-qt)
  set_tests_properties(test_b3_uipreview-qt PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal")

  add_executable(test_b3_themerendering-qt b3themerenderingtests.cpp)
  set_property(TARGET test_b3_themerendering-qt PROPERTY AUTOUIC_SEARCH_PATHS
    "${CMAKE_CURRENT_SOURCE_DIR}/../forms"
  )
  target_link_libraries(test_b3_themerendering-qt
    core_interface bitcoinqt bitcoin_node Qt6::Test
  )
  import_plugins(test_b3_themerendering-qt)
  add_test(NAME test_b3_themerendering-qt COMMAND test_b3_themerendering-qt)
  set_tests_properties(test_b3_themerendering-qt PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal")
endif()

add_test(NAME test_bitcoin-qt
  COMMAND test_bitcoin-qt
)
if(WIN32 AND VCPKG_TARGET_TRIPLET)
  set(plugin_path "$<SHELL_PATH:$<PATH:GET_PARENT_PATH,$<PATH:GET_PARENT_PATH,$<TARGET_PROPERTY:Qt6::QWindowsIntegrationPlugin,LOCATION_$<CONFIG>>>>>")
  set_property(TEST test_bitcoin-qt APPEND PROPERTY
    ENVIRONMENT_MODIFICATION
      # On Windows, vcpkg configures Qt with `-opengl dynamic`, which makes
      # the "minimal" platform plugin unusable due to internal Qt bugs.
      QT_QPA_PLATFORM=set:windows
      QT_PLUGIN_PATH=set:${plugin_path}
  )
  if(TARGET test_b3_sendcoinsentry-qt)
    set_property(TEST test_b3_sendcoinsentry-qt APPEND PROPERTY
      ENVIRONMENT_MODIFICATION
        QT_QPA_PLATFORM=set:windows
        QT_PLUGIN_PATH=set:${plugin_path}
    )
  endif()
  if(TARGET test_b3_stakingunlock-qt)
    set_property(TEST test_b3_stakingunlock-qt APPEND PROPERTY
      ENVIRONMENT_MODIFICATION
        QT_QPA_PLATFORM=set:windows
        QT_PLUGIN_PATH=set:${plugin_path}
    )
  endif()
  if(TARGET test_b3_flowmeshpanel-qt)
    set_property(TEST test_b3_flowmeshpanel-qt APPEND PROPERTY
      ENVIRONMENT_MODIFICATION
        QT_QPA_PLATFORM=set:windows
        QT_PLUGIN_PATH=set:${plugin_path}
    )
  endif()
  if(TARGET test_b3_uipreview-qt)
    set_property(TEST test_b3_uipreview-qt APPEND PROPERTY
      ENVIRONMENT_MODIFICATION
        QT_QPA_PLATFORM=set:windows
        QT_PLUGIN_PATH=set:${plugin_path}
    )
  endif()
  if(TARGET test_b3_themerendering-qt)
    set_property(TEST test_b3_themerendering-qt APPEND PROPERTY
      ENVIRONMENT_MODIFICATION
        QT_QPA_PLATFORM=set:windows
        QT_PLUGIN_PATH=set:${plugin_path}
    )
  endif()
  unset(plugin_path)
endif()

install_binary_component(test_bitcoin-qt INTERNAL)
