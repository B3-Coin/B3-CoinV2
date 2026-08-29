#!/usr/bin/env python3
# Copyright (c) 2021-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test addrman functionality"""

import os

from test_framework.messages import ser_uint256, hash256, MAGIC_BYTES
from test_framework.netutil import ADDRMAN_NEW_BUCKET_COUNT, ADDRMAN_TRIED_BUCKET_COUNT, ADDRMAN_BUCKET_SIZE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

def serialize_addrman(
    *,
    format=1,
    lowest_compatible=4,
    net_magic="regtest",
    bucket_key=1,
    len_new=None,
    len_tried=None,
    mock_checksum=None,
):
    new = []
    tried = []
    INCOMPATIBILITY_BASE = 32
    r = MAGIC_BYTES[net_magic]
    r += format.to_bytes(1, "little")
    r += (INCOMPATIBILITY_BASE + lowest_compatible).to_bytes(1, "little")
    r += ser_uint256(bucket_key)
    r += (len_new or len(new)).to_bytes(4, "little", signed=True)
    r += (len_tried or len(tried)).to_bytes(4, "little", signed=True)
    ADDRMAN_NEW_BUCKET_COUNT = 1 << 10
    r += (ADDRMAN_NEW_BUCKET_COUNT ^ (1 << 30)).to_bytes(4, "little", signed=True)
    for _ in range(ADDRMAN_NEW_BUCKET_COUNT):
        r += (0).to_bytes(4, "little", signed=True)
    checksum = hash256(r)
    r += mock_checksum or checksum
    return r


def write_addrman(peers_dat, **kwargs):
    with open(peers_dat, "wb") as f:
        f.write(serialize_addrman(**kwargs))


class AddrmanTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        peers_dat = os.path.join(self.nodes[0].chain_path, "peers.dat")
        peers_bak = peers_dat + ".bak"

        def check_recovered_start(description):
            # B3 divergence from upstream: an unreadable peers.dat is the
            # expected state of every datadir carried over from the legacy
            # B3Coin client, so ANY deserialization failure backs the file
            # up and bootstraps a fresh addrman instead of refusing to
            # start (src/addrdb.cpp LoadAddrman).
            self.log.info(f"Check that {description} is backed up and recreated")
            if os.path.exists(peers_bak):
                os.remove(peers_bak)
            with self.nodes[0].assert_debug_log([
                    "Creating new peers.dat because the existing file could not be read",
            ]):
                self.start_node(0)
            assert_equal(self.nodes[0].getnodeaddresses(), [])
            assert_equal(os.path.exists(peers_dat + ".bak"), True)
            self.stop_node(0)

        self.log.info("Check that mocked addrman is valid")
        self.stop_node(0)
        write_addrman(peers_dat)
        with self.nodes[0].assert_debug_log(["Loaded 0 addresses from peers.dat"]):
            self.start_node(0, extra_args=["-checkaddrman=1"])
        assert_equal(self.nodes[0].getnodeaddresses(), [])
        self.stop_node(0)

        write_addrman(peers_dat, lowest_compatible=-32)
        check_recovered_start("addrman with negative lowest_compatible")

        self.log.info("Check that addrman from future is overwritten with new addrman")
        write_addrman(peers_dat, lowest_compatible=111)
        os.path.exists(peers_bak) and os.remove(peers_bak)
        with self.nodes[0].assert_debug_log([
                f'Creating new peers.dat because the file version was not compatible ("{peers_dat}"). Original backed up to peers.dat.bak',
        ]):
            self.start_node(0)
        assert_equal(self.nodes[0].getnodeaddresses(), [])
        assert_equal(os.path.exists(peers_bak), True)
        self.stop_node(0)

        with open(peers_dat, "wb") as f:
            f.write(serialize_addrman()[:-1])
        check_recovered_start("truncated addrman (EOF)")

        write_addrman(peers_dat, net_magic="signet")
        check_recovered_start("addrman with wrong network magic")

        write_addrman(peers_dat, mock_checksum=b"ab" * 32)
        check_recovered_start("addrman with checksum mismatch")

        max_len_tried = ADDRMAN_TRIED_BUCKET_COUNT * ADDRMAN_BUCKET_SIZE
        write_addrman(peers_dat, len_tried=-1)
        check_recovered_start("addrman with negative len_tried")

        write_addrman(peers_dat, len_tried=max_len_tried + 1)
        check_recovered_start("addrman with oversized len_tried")

        max_len_new = ADDRMAN_NEW_BUCKET_COUNT * ADDRMAN_BUCKET_SIZE
        write_addrman(peers_dat, len_new=-1)
        check_recovered_start("addrman with negative len_new")

        write_addrman(peers_dat, len_new=max_len_new + 1)
        check_recovered_start("addrman with oversized len_new")

        write_addrman(peers_dat, bucket_key=0)
        check_recovered_start("addrman failing its consistency check")

        self.log.info("Check that missing addrman is recreated")
        self.start_node(0)
        self.restart_node(0, clear_addrman=True)
        assert_equal(self.nodes[0].getnodeaddresses(), [])


if __name__ == "__main__":
    AddrmanTest(__file__).main()
