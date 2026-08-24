# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(AddWindowsResources)

configure_file(${PROJECT_SOURCE_DIR}/cmake/bitcoin-build-config.h.in bitcoin-build-config.h USE_SOURCE_PERMISSIONS @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

#=============================
# Subprojects
#=============================
# Subprojects include subdirectories that do or could have tests
# and/or benchmark binaries, such as all subtrees and univalue.
# These need to be included before CMAKE_*_OUTPUT_DIRECTORY variables
# are set, so output locations of subproject tests and libraries are
# not overridden.
include(../cmake/crc32c.cmake)
include(../cmake/leveldb.cmake)
include(../cmake/minisketch.cmake)
add_subdirectory(univalue)
if (ENABLE_IPC AND NOT WITH_EXTERNAL_LIBMULTIPROCESS)
  include(../cmake/libmultiprocess.cmake)
  add_libmultiprocess(ipc/libmultiprocess)
endif()
include(../cmake/secp256k1.cmake)
add_secp256k1(secp256k1)

# Set top-level target output locations.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()

add_custom_target(generate_build_info
  BYPRODUCTS ${PROJECT_BINARY_DIR}/src/bitcoin-build-info.h
  COMMAND ${CMAKE_COMMAND} -DBUILD_INFO_HEADER_PATH=${PROJECT_BINARY_DIR}/src/bitcoin-build-info.h -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/script/GenerateBuildInfo.cmake
  COMMENT "Generating bitcoin-build-info.h"
  VERBATIM
)
add_library(bitcoin_clientversion STATIC
  clientversion.cpp
)
target_link_libraries(bitcoin_clientversion
  PRIVATE
    core_interface
)
add_dependencies(bitcoin_clientversion generate_build_info)

# Pinned blst (src/blst, upstream v0.3.17) — consensus dependency of
# crypto/bls (plan Commit 2). Portable build; narrow wrapper only.
include(../cmake/blst.cmake)
add_b3_blst()
add_subdirectory(crypto)
add_subdirectory(util)
if(ENABLE_IPC)
  add_subdirectory(ipc)
endif()

add_library(bitcoin_consensus STATIC EXCLUDE_FROM_ALL
  arith_uint256.cpp
  consensus/merkle.cpp
  consensus/tx_check.cpp
  hash.cpp
  legacy/pos.cpp
  legacy/primitives.cpp
  legacy/scrypt.cpp
  primitives/block.cpp
  primitives/transaction.cpp
  pubkey.cpp
  script/interpreter.cpp
  script/script.cpp
  script/script_error.cpp
  uint256.cpp
)
target_link_libraries(bitcoin_consensus
  PRIVATE
    core_interface
    bitcoin_crypto
    secp256k1
)

if(WITH_ZMQ)
  add_subdirectory(zmq)
endif()

# Home for common functionality shared by different executables and libraries.
# Similar to `bitcoin_util` library, but higher-level.
add_library(bitcoin_common STATIC EXCLUDE_FROM_ALL
  addresstype.cpp
  base58.cpp
  bech32.cpp
  chain.cpp
  chainparams.cpp
  chainparamsbase.cpp
  coins.cpp
  common/args.cpp
  common/bloom.cpp
  common/config.cpp
  common/init.cpp
  common/interfaces.cpp
  common/messages.cpp
  common/netif.cpp
  common/pcp.cpp
  common/run_command.cpp
  common/settings.cpp
  common/signmessage.cpp
  common/system.cpp
  common/url.cpp
  compressor.cpp
  core_io.cpp
  deploymentinfo.cpp
  external_signer.cpp
  init/common.cpp
  kernel/chainparams.cpp
  legacy/checkpoints.cpp
  key.cpp
  key_io.cpp
  merkleblock.cpp
  musig.cpp
  net_permissions.cpp
  net_types.cpp
  netaddress.cpp
  netbase.cpp
  outputtype.cpp
  policy/feerate.cpp
  policy/policy.cpp
  pow.cpp
  protocol.cpp
  psbt.cpp
  rpc/rawtransaction_util.cpp
  rpc/request.cpp
  rpc/util.cpp
  scheduler.cpp
  script/descriptor.cpp
  script/miniscript.cpp
  script/parsing.cpp
  script/sign.cpp
  script/signingprovider.cpp
  script/solver.cpp
)
target_link_libraries(bitcoin_common
  PRIVATE
    core_interface
    bitcoin_consensus
    bitcoin_util
    univalue
    secp256k1
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
    $<$<PLATFORM_ID:Windows>:ws2_32>
)

include(InstallBinaryComponent)

if(ENABLE_WALLET)
  add_subdirectory(wallet)

  if(BUILD_WALLET_TOOL)
    add_executable(b3coin-wallet
      bitcoin-wallet.cpp
      init/bitcoin-wallet.cpp
      wallet/wallettool.cpp
    )
    add_windows_resources(b3coin-wallet bitcoin-wallet-res.rc)
    add_windows_application_manifest(b3coin-wallet)
    target_link_libraries(b3coin-wallet
      core_interface
      bitcoin_wallet
      bitcoin_common
      bitcoin_util
      Boost::headers
    )
    install_binary_component(b3coin-wallet HAS_MANPAGE)
  endif()
endif()


# P2P and RPC server functionality used by `bitcoind` and `bitcoin-qt` executables.
add_library(bitcoin_node STATIC EXCLUDE_FROM_ALL
  addrdb.cpp
  addrman.cpp
  banman.cpp
  bip324.cpp
  blockencodings.cpp
  blockfilter.cpp
  consensus/tx_verify.cpp
  dbwrapper.cpp
  deploymentstatus.cpp
  flatfile.cpp
  headerssync.cpp
  httprpc.cpp
  httpserver.cpp
  i2p.cpp
  index/base.cpp
  index/blockfilterindex.cpp
  index/coinstatsindex.cpp
  index/txindex.cpp
  index/txospenderindex.cpp
  init.cpp
  kernel/chain.cpp
  kernel/checks.cpp
  kernel/coinstats.cpp
  kernel/context.cpp
  kernel/cs_main.cpp
  kernel/disconnected_transactions.cpp
  legacy/consensus.cpp
  legacy/replay.cpp
  kernel/mempool_removal_reason.cpp
  mapport.cpp
  net.cpp
  net_processing.cpp
  netgroup.cpp
  node/abort.cpp
  node/blockmanager_args.cpp
  node/blockstorage.cpp
  node/caches.cpp
  node/chainstate.cpp
  node/chainstatemanager_args.cpp
  node/coin.cpp
  node/coins_view_args.cpp
  node/connection_types.cpp
  node/context.cpp
  node/database_args.cpp
  node/eviction.cpp
  node/interface_ui.cpp
  node/interfaces.cpp
  node/kernel_notifications.cpp
  node/legacy_orphanage.cpp
  node/mempool_args.cpp
  node/mempool_persist.cpp
  node/mempool_persist_args.cpp
  node/miner.cpp
  node/mini_miner.cpp
  node/minisketchwrapper.cpp
  node/peerman_args.cpp
  node/psbt.cpp
  node/timeoffsets.cpp
  node/transaction.cpp
  node/txdownloadman_impl.cpp
  node/txorphanage.cpp
  node/txreconciliation.cpp
  node/flowmesh_anchor.cpp
  node/flowmesh_store.cpp
  node/fn_pod.cpp
  node/legacy_fn_issuance.cpp
  node/finality_binding_index.cpp
  node/finality_pin.cpp
  node/finality_signature.cpp
  node/finality_tracker.cpp
  node/validator_set.cpp
  node/stake_registry.cpp
  node/stake_tracker.cpp
  node/staking.cpp
  node/utxo_commitment.cpp
  node/utxo_equivalence_check.cpp
  node/utxo_rows.cpp
  node/utxo_snapshot.cpp
  node/warnings.cpp
  noui.cpp
  policy/ephemeral_policy.cpp
  policy/fees/block_policy_estimator.cpp
  policy/fees/block_policy_estimator_args.cpp
  policy/packages.cpp
  policy/rbf.cpp
  policy/settings.cpp
  policy/truc_policy.cpp
  private_broadcast.cpp
  rest.cpp
  rpc/blockchain.cpp
  rpc/external_signer.cpp
  rpc/fees.cpp
  rpc/mempool.cpp
  rpc/mining.cpp
  rpc/net.cpp
  rpc/node.cpp
  rpc/output_script.cpp
  rpc/rawtransaction.cpp
  rpc/server.cpp
  rpc/server_util.cpp
  rpc/signmessage.cpp
  rpc/txoutproof.cpp
  script/sigcache.cpp
  signet.cpp
  torcontrol.cpp
  txdb.cpp
  txgraph.cpp
  txmempool.cpp
  txrequest.cpp
  validation.cpp
  validationinterface.cpp
  versionbits.cpp
  $<$<TARGET_EXISTS:bitcoin_wallet>:wallet/init.cpp>
  $<$<NOT:$<TARGET_EXISTS:bitcoin_wallet>>:dummywallet.cpp>
)
target_link_libraries(bitcoin_node
  PRIVATE
    core_interface
    bitcoin_common
    bitcoin_util
    $<TARGET_NAME_IF_EXISTS:bitcoin_zmq>
    leveldb
    minisketch
    univalue
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:libevent::core>
    $<TARGET_NAME_IF_EXISTS:libevent::extra>
    $<TARGET_NAME_IF_EXISTS:libevent::pthreads>
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
)
if(WITH_EMBEDDED_ASMAP)
  target_compile_definitions(bitcoin_node PRIVATE ENABLE_EMBEDDED_ASMAP=1)
  include(TargetDataSources)
  target_raw_data_sources(bitcoin_node NAMESPACE node::data
    node/data/ip_asn.dat
  )
endif()

# Bitcoin wrapper executable that can call other executables.
if(BUILD_BITCOIN_BIN)
  add_executable(b3coin bitcoin.cpp)
  add_windows_resources(b3coin bitcoin-res.rc)
  add_windows_application_manifest(b3coin)
  target_link_libraries(b3coin core_interface bitcoin_common bitcoin_util)
  install_binary_component(b3coin HAS_MANPAGE)
endif()

# Bitcoin Core bitcoind.
if(BUILD_DAEMON)
  add_executable(b3coind
    bitcoind.cpp
    init/bitcoind.cpp
  )
  add_windows_resources(b3coind bitcoind-res.rc)
  add_windows_application_manifest(b3coind)
  target_link_libraries(b3coind
    core_interface
    bitcoin_node
    $<TARGET_NAME_IF_EXISTS:bitcoin_wallet>
  )
  install_binary_component(b3coind HAS_MANPAGE)
endif()

# Standalone operator diagnostic: verify a trusted replay of the legacy chain
# against a fully-validated chainstate (U == U'). Outside consensus and node
# startup by construction.
add_executable(b3coin-utxo-verify
  bitcoin-utxo-verify.cpp
)
add_windows_application_manifest(b3coin-utxo-verify)
target_link_libraries(b3coin-utxo-verify
  PRIVATE
    core_interface
    bitcoin_node
)
install_binary_component(b3coin-utxo-verify INTERNAL)

if(ENABLE_IPC AND BUILD_DAEMON)
  add_executable(b3coin-node
    bitcoind.cpp
    init/bitcoin-node.cpp
  )
  target_link_libraries(b3coin-node
    core_interface
    bitcoin_node
    bitcoin_ipc
    $<TARGET_NAME_IF_EXISTS:bitcoin_wallet>
  )
  install_binary_component(b3coin-node INTERNAL)
endif()


add_library(bitcoin_cli STATIC EXCLUDE_FROM_ALL
  compat/stdin.cpp
  rpc/client.cpp
)
target_link_libraries(bitcoin_cli
  PUBLIC
    core_interface
    univalue
)


# Bitcoin Core RPC client
if(BUILD_CLI)
  add_executable(b3coin-cli bitcoin-cli.cpp)
  add_windows_resources(b3coin-cli bitcoin-cli-res.rc)
  add_windows_application_manifest(b3coin-cli)
  target_link_libraries(b3coin-cli
    core_interface
    bitcoin_cli
    bitcoin_common
    bitcoin_util
    libevent::core
    libevent::extra
  )
  install_binary_component(b3coin-cli HAS_MANPAGE)
endif()


if(BUILD_TX)
  add_executable(b3coin-tx bitcoin-tx.cpp)
  add_windows_resources(b3coin-tx bitcoin-tx-res.rc)
  add_windows_application_manifest(b3coin-tx)
  target_link_libraries(b3coin-tx
    core_interface
    bitcoin_common
    bitcoin_util
    univalue
  )
  install_binary_component(b3coin-tx HAS_MANPAGE)
endif()


if(BUILD_UTIL)
  add_executable(b3coin-util bitcoin-util.cpp)
  add_windows_resources(b3coin-util bitcoin-util-res.rc)
  add_windows_application_manifest(b3coin-util)
  target_link_libraries(b3coin-util
    core_interface
    bitcoin_common
    bitcoin_util
  )
  install_binary_component(b3coin-util HAS_MANPAGE)
endif()


if(BUILD_GUI)
  add_subdirectory(qt)
endif()


if(BUILD_KERNEL_LIB)
  add_subdirectory(kernel)
  if (BUILD_KERNEL_TEST)
    add_subdirectory(test/kernel)
  endif()
  if(BUILD_UTIL_CHAINSTATE)
    add_executable(b3coin-chainstate
      bitcoin-chainstate.cpp
    )
    add_windows_application_manifest(b3coin-chainstate)
    target_link_libraries(b3coin-chainstate
      PRIVATE
        core_interface
        bitcoinkernel
    )
    install_binary_component(b3coin-chainstate INTERNAL)
  endif()
endif()


add_subdirectory(test/util)
if(BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(B3_FINALITY_BENCH)
  # Timing harness over the same vendored blst the consensus wrapper uses.
  add_executable(b3-finality-bench bench/b3_finality_bench.cpp)
  target_link_libraries(b3-finality-bench
    PRIVATE
      core_interface
      b3_blst
      bitcoin_consensus
      bitcoin_common
      bitcoin_util
      bitcoin_crypto
      secp256k1
  )
endif()

if(B3_BRIDGE_TOOLS)
  # Relayer dry-run harness over the same header-only bridge verification
  # stack the tests use. Tooling only; never linked into the node.
  add_executable(b3-bridge-ethcheck bridge/ethcheck.cpp)
  target_link_libraries(b3-bridge-ethcheck
    PRIVATE
      core_interface
      b3_blst
      bitcoin_consensus
      bitcoin_common
      bitcoin_util
      bitcoin_crypto
      secp256k1
      univalue
  )
endif()

if(BUILD_TESTS)
  add_subdirectory(test)
endif()

if(BUILD_FUZZ_BINARY)
  add_subdirectory(test/fuzz)
endif()
