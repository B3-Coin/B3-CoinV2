#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Default-build negative control: experimental bootstrap RPCs must be absent.

Run directly against a build with BUILD_FLOWMESH_P2FV_REGTEST_GATE=OFF.
One generated, engine-off regtest node. No consensus/funding/trading workload.
"""
import json
from pathlib import Path

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_raises_rpc_error


class FlowMeshP2fvBuildGateTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-enableflowmeshvalidator=0"]]
        self.child = None
        self.result = {"complete": False, "build_option": "OFF"}

    def run_test(self):
        node = self.nodes[0]
        self.child = node.process
        for method in ("getflowmeshregtestbootstrap", "verifyflowmeshregtestbootstrap"):
            # Wrong parameters are deliberate: an accidentally registered RPC
            # must not count as absent merely because it rejects the request.
            assert_raises_rpc_error(-32601, "Method not found", getattr(node, method))
        self.result["complete"] = True

    def shutdown(self):
        if self.child is None and self.nodes:
            self.child = self.nodes[0].process
        code = super().shutdown()
        self.result["harness_exit_code"] = code
        self.result["child_exit_code"] = self.child.poll() if self.child else None
        if self.result["child_exit_code"] != 0:
            self.result["complete"] = False
            code = 1
        Path(self.options.tmpdir, "p2fv-build-gate.json").write_text(json.dumps(self.result, indent=2) + "\n")
        return code


if __name__ == "__main__":
    FlowMeshP2fvBuildGateTest(__file__).main()
