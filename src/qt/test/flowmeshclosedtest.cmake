# Packaging-only guarded entry. Never register a CTest GUI launch or install
# this executable as the normal wallet client. Keep the historic target name
# for existing offline fixtures and package the app with its REGTEST identity.
if(NOT (APPLE OR WIN32) OR NOT ENABLE_WALLET)
  return()
endif()

if(BUILD_FLOWMESH_REGTEST_CLIENT OR BUILD_GUI_TESTS)
  set(B3_FLOWMESH_CLOSED_TEST_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/flowmeshclosedtest-not-ready.json"
    CACHE FILEPATH "Reviewed embedded regtest profile (default refuses before node/wallet startup).")
  if(NOT EXISTS "${B3_FLOWMESH_CLOSED_TEST_PROFILE}" OR IS_DIRECTORY "${B3_FLOWMESH_CLOSED_TEST_PROFILE}")
    message(FATAL_ERROR "B3_FLOWMESH_CLOSED_TEST_PROFILE must name an existing reviewed JSON file")
  endif()
  add_executable(test_b3_flowmeshclosed-gui EXCLUDE_FROM_ALL
    flowmeshclosedtest_gui_main.cpp flowmeshclosedtest_policy.cpp ../../init/bitcoin-qt.cpp)
  if(BUILD_FLOWMESH_REGTEST_CLIENT)
    set_target_properties(test_b3_flowmeshclosed-gui PROPERTIES EXCLUDE_FROM_ALL FALSE)
  endif()
  set_source_files_properties("${B3_FLOWMESH_CLOSED_TEST_PROFILE}" PROPERTIES QT_RESOURCE_ALIAS "profile.json")
  qt_add_resources(test_b3_flowmeshclosed-gui closed_test_profile
    PREFIX "/closed-test" FILES "${B3_FLOWMESH_CLOSED_TEST_PROFILE}")
  target_link_libraries(test_b3_flowmeshclosed-gui core_interface bitcoinqt bitcoin_node Qt6::Network OpenSSL::Crypto)
  add_windows_application_manifest(test_b3_flowmeshclosed-gui)
  import_plugins(test_b3_flowmeshclosed-gui)
  if(APPLE)
    # Use the actual configured deployment target. A package built against a
    # newer SDK must not silently claim an unrelated fixed macOS minimum.
    set(B3_FLOWMESH_MINIMUM_SYSTEM_VERSION_PLIST "")
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      set(B3_FLOWMESH_MINIMUM_SYSTEM_VERSION_PLIST
        "<key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>")
    endif()
    set_target_properties(test_b3_flowmeshclosed-gui PROPERTIES
      MACOSX_BUNDLE TRUE
      MACOSX_BUNDLE_BUNDLE_NAME "B3 FlowMesh REGTEST TEST4"
      MACOSX_BUNDLE_GUI_IDENTIFIER "org.b3coin.flowmesh.regtest.test4"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}d4"
      MACOSX_BUNDLE_INFO_STRING "B3 FlowMesh REGTEST TEST4 ${CLIENT_VERSION_STRING}"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/flowmeshclosedtest-Info.plist.in")
  elseif(WIN32)
    target_link_libraries(test_b3_flowmeshclosed-gui advapi32)
    set_target_properties(test_b3_flowmeshclosed-gui PROPERTIES WIN32_EXECUTABLE TRUE)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/flowmesh-regtest-version.rc" CONTENT [=[
#include <windows.h>
1 ICON DISCARDABLE "@PROJECT_SOURCE_DIR@/src/qt/res/icons/bitcoin.ico"
VS_VERSION_INFO VERSIONINFO
FILEVERSION @CLIENT_VERSION_MAJOR@,@CLIENT_VERSION_MINOR@,@CLIENT_VERSION_BUILD@,4
PRODUCTVERSION @CLIENT_VERSION_MAJOR@,@CLIENT_VERSION_MINOR@,@CLIENT_VERSION_BUILD@,4
FILEFLAGSMASK VS_FFI_FILEFLAGSMASK
FILEFLAGS VS_FF_PRERELEASE
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904E4"
    BEGIN
      VALUE "CompanyName", "B3 FlowMesh project"
      VALUE "FileDescription", "B3 FlowMesh REGTEST TEST4"
      VALUE "FileVersion", "@CLIENT_VERSION_STRING@"
      VALUE "InternalName", "test_b3_flowmeshclosed-gui"
      VALUE "OriginalFilename", "B3FlowMeshRegtest.exe"
      VALUE "ProductName", "B3 FlowMesh REGTEST TEST4"
      VALUE "ProductVersion", "@CLIENT_VERSION_STRING@"
      VALUE "LegalCopyright", "Test tokens have no value. Futures are informational only."
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1252
  END
END
]=] @ONLY)
    add_windows_resources(test_b3_flowmeshclosed-gui "${CMAKE_CURRENT_BINARY_DIR}/flowmesh-regtest-version.rc")
  endif()
endif()

if(BUILD_FLOWMESH_REGTEST_POLICY_TESTS OR BUILD_GUI_TESTS)
  add_executable(test_b3_flowmeshclosed-policy EXCLUDE_FROM_ALL
    flowmeshclosedtest_policytests.cpp flowmeshclosedtest_policy.cpp)
  # The tests also run on a native Windows runner after cross-compilation;
  # their reviewed public fixture must travel inside the executable.
  set(flowmesh_policy_fixture "${PROJECT_SOURCE_DIR}/contrib/flowmesh-regtest/profile-vps-20260926.json")
  set_source_files_properties("${flowmesh_policy_fixture}" PROPERTIES QT_RESOURCE_ALIAS "profile.json")
  qt_add_resources(test_b3_flowmeshclosed-policy closed_test_policy_fixture
    PREFIX "/closed-test-test" FILES "${flowmesh_policy_fixture}")
  target_compile_definitions(test_b3_flowmeshclosed-policy PRIVATE
    FLOWMESH_CLOSED_TEST_SOURCE_PROFILE_PATH=":/closed-test-test/profile.json")
  target_link_libraries(test_b3_flowmeshclosed-policy
    core_interface bitcoin_common bitcoin_util univalue Qt6::Core Qt6::Network Qt6::Test OpenSSL::Crypto)
  if(WIN32)
    target_link_libraries(test_b3_flowmeshclosed-policy advapi32)
  endif()
  if(BUILD_FLOWMESH_REGTEST_POLICY_TESTS)
    set_target_properties(test_b3_flowmeshclosed-policy PROPERTIES EXCLUDE_FROM_ALL FALSE)
    add_test(NAME test_b3_flowmeshclosed-policy COMMAND test_b3_flowmeshclosed-policy)
  endif()
endif()
