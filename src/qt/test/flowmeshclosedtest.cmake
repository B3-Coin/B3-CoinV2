# Packaging-only, opt-in macOS test entry. No installation/CTest UI launch.
if(APPLE AND ENABLE_WALLET)
  set(B3_FLOWMESH_CLOSED_TEST_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/flowmeshclosedtest-not-ready.json"
    CACHE FILEPATH "Reviewed embedded closed-test profile (default refuses before node/wallet startup)")
  add_executable(test_b3_flowmeshclosed-gui EXCLUDE_FROM_ALL
    flowmeshclosedtest_gui_main.cpp flowmeshclosedtest_policy.cpp ../../init/bitcoin-qt.cpp)
  set_source_files_properties("${B3_FLOWMESH_CLOSED_TEST_PROFILE}" PROPERTIES QT_RESOURCE_ALIAS "profile.json")
  qt_add_resources(test_b3_flowmeshclosed-gui closed_test_profile
    PREFIX "/closed-test" FILES "${B3_FLOWMESH_CLOSED_TEST_PROFILE}")
  target_link_libraries(test_b3_flowmeshclosed-gui core_interface bitcoinqt bitcoin_node Qt6::Network)
  import_plugins(test_b3_flowmeshclosed-gui)
  set_target_properties(test_b3_flowmeshclosed-gui PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "B3 FlowMesh CLOSED TEST"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.b3coin.flowmesh.closed-test.candidate04"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/flowmeshclosedtest-Info.plist.in")

  add_executable(test_b3_flowmeshclosed-policy EXCLUDE_FROM_ALL
    flowmeshclosedtest_policytests.cpp flowmeshclosedtest_policy.cpp)
  target_link_libraries(test_b3_flowmeshclosed-policy core_interface bitcoin_common bitcoin_util univalue Qt6::Core Qt6::Network Qt6::Test)
endif()
