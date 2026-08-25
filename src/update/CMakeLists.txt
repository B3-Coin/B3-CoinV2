# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.

# B3 Hive update system core: strictly isolated from consensus, wallet,
# FlowMesh and P2P (doc/design/b3-hive-update-system.md).
add_library(b3_update STATIC EXCLUDE_FROM_ALL
  downloader.cpp
  manifest.cpp
)
target_link_libraries(b3_update
  PRIVATE
    core_interface
    bitcoin_common
    bitcoin_crypto
    bitcoin_util
    secp256k1
)
