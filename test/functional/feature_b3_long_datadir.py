#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Run a node whose data directory path is long.

Windows limits a path to 259 characters unless long paths are enabled. With
long paths, and on every other platform, a node in a 240-character data
directory must start, build its indexes, create, back up and reload a wallet,
send a payment, dump a UTXO snapshot and the mempool, and restart.

On Windows without long paths the node refuses, at startup and before it
creates anything, a network data directory longer than
MAX_WINDOWS_DATADIR_LENGTH (src/common/init.h), and it works in one exactly
that long. Without the check it failed deep inside LevelDB or the filesystem
library, from about 200 characters on.
"""

import ctypes
import os
import pathlib
import subprocess
import sys

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

LONG_DATADIR_LENGTH = 240
# src/common/init.h: WINDOWS_MAX_PATH_LENGTH - DATADIR_LONGEST_RELATIVE_PATH.
MAX_WINDOWS_DATADIR_LENGTH = 259 - 112


def windows_long_paths_enabled():
    """Python's own executable opts in to long paths, so this reports the
    system setting the node depends on."""
    try:
        return bool(ctypes.WinDLL("ntdll").RtlAreLongPathsEnabled())
    except (AttributeError, OSError):
        return False


def grow(base, length):
    """A path below `base` exactly `length` characters long, built from
    components of at most 40 characters."""
    path = str(base)
    index = 0
    assert len(path) + 2 <= length, f"temporary directory {path} is too long for this test"
    while len(path) < length:
        room = length - len(path) - 1
        component = (f"p{index:02d}" + "x" * 37)[:max(1, min(40, room))]
        # A one-character tail would leave no room; borrow from this component.
        if 0 < length - len(path) - 1 - len(component) < 2:
            component = component[:-2]
        path = os.path.join(path, component)
        index += 1
    assert_equal(len(path), length)
    return pathlib.Path(path)


class LongDataDirTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-txindex", "-blockfilterindex", "-coinstatsindex"]]
        self.limited = sys.platform == "win32" and not windows_long_paths_enabled()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def node_dir_length(self):
        if not self.limited:
            return LONG_DATADIR_LENGTH
        # The limit applies to the network directory, node0\regtest.
        return MAX_WINDOWS_DATADIR_LENGTH - len(os.sep + self.chain)

    def setup_chain(self):
        # Point tmpdir at a deeper directory so that tmpdir/node0 has the
        # length under test. The framework's own log stays in the original
        # tmpdir, which is restored for the clean-up at the end.
        self.original_tmpdir = self.options.tmpdir
        base = grow(self.options.tmpdir, self.node_dir_length() - len(os.sep + "node0"))
        os.makedirs(base)
        self.options.tmpdir = str(base)
        super().setup_chain()

    def run_test(self):
        node = self.nodes[0]
        self.log.info(f"Data directory: {len(str(node.datadir_path))} characters, "
                      f"network directory: {len(str(node.chain_path))} characters")
        if self.limited:
            assert_equal(len(str(node.chain_path)), MAX_WINDOWS_DATADIR_LENGTH)
            self.test_refused_one_character_over()
        else:
            assert_equal(len(str(node.datadir_path)), LONG_DATADIR_LENGTH)
        self.test_operations()
        self.options.tmpdir = self.original_tmpdir

    def test_refused_one_character_over(self):
        self.log.info("A network directory one character over the limit is refused before anything is created")
        datadir = grow(self.original_tmpdir, MAX_WINDOWS_DATADIR_LENGTH + 1 - len(os.sep + self.chain))
        os.makedirs(datadir)
        args = self.nodes[0].binaries.node_argv() + [f"-{self.chain}", f"-datadir={datadir}", "-server=0", "-listen=0",
                "-connect=0", "-dnsseed=0", "-fixedseeds=0", "-natpmp=0", "-printtoconsole=0"]
        try:
            result = subprocess.run(args, capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            raise AssertionError("the node started in a data directory over the limit")
        assert result.returncode != 0
        expected = f"is {MAX_WINDOWS_DATADIR_LENGTH + 1} characters long"
        assert expected in result.stderr, result.stderr
        assert_equal(os.listdir(datadir), [])

    def test_operations(self):
        node = self.nodes[0]
        self.log.info("Indexes, a wallet, a payment, a backup and a snapshot dump in the long directory")
        node.createwallet(wallet_name="w")
        wallet = node.get_wallet_rpc("w")
        address = wallet.getnewaddress()
        self.generatetoaddress(node, 120, address)
        assert_greater_than(wallet.getbalance(), 0)
        txid = wallet.sendtoaddress(wallet.getnewaddress(), 1)
        self.generatetoaddress(node, 1, address)
        self.wait_until(lambda: all(index["synced"] for index in node.getindexinfo().values()))
        assert_equal(sorted(node.getindexinfo()), ["basic block filter index", "coinstatsindex", "txindex"])
        assert_equal(node.getrawtransaction(txid, True)["txid"], txid)
        wallet.backupwallet(str(node.datadir_path / "wallet-backup.dat"))
        assert (node.datadir_path / "wallet-backup.dat").is_file()
        node.dumptxoutset(str(node.datadir_path / "utxo.dat"), "latest")
        assert (node.datadir_path / "utxo.dat").is_file()
        node.savemempool()

        self.log.info("Restart and reload the wallet")
        height = node.getblockcount()
        self.restart_node(0)
        assert_equal(node.getblockcount(), height)
        node.loadwallet("w")
        assert_equal(node.get_wallet_rpc("w").gettransaction(txid)["txid"], txid)
        self.wait_until(lambda: all(index["synced"] for index in node.getindexinfo().values()))
        assert node.verifychain(4, 0)


if __name__ == "__main__":
    LongDataDirTest(__file__).main()
