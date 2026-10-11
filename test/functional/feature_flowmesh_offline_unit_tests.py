#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FlowMesh offline harness unit tests

Runs the daemon-free unittest modules that check the FlowMesh qualification
harnesses' own recording, analysis and cleanup logic, so they cannot drift
silently from the harnesses they exercise. No node, socket or build output is
used, and the opt-in campaigns themselves are not run.
"""

import sys
import unittest

from test_framework.test_framework import TEST_EXIT_PASSED, TEST_EXIT_FAILED

FLOWMESH_OFFLINE_MODULES = [
    "flowmesh_performance_fault_metrics_test",
    "flowmesh_performance_metrics_test",
    "flowmesh_performance_worker_analyze_test",
    "flowmesh_public_consistency_analyze_test",
    "flowmesh_public_trace_test",
    "flowmesh_relay_read_failure_test",
    # Opt-in campaigns that carry their own offline unittest classes.
    "feature_flowmesh_delivery_review",
    "feature_flowmesh_speed",
]


def run_unit_tests():
    suite = unittest.TestSuite()
    for module in FLOWMESH_OFFLINE_MODULES:
        suite.addTest(unittest.TestLoader().loadTestsFromName(module))
    result = unittest.TextTestRunner(stream=sys.stdout, verbosity=1).run(suite)
    if not result.wasSuccessful():
        sys.exit(TEST_EXIT_FAILED)
    sys.exit(TEST_EXIT_PASSED)


if __name__ == "__main__":
    run_unit_tests()
