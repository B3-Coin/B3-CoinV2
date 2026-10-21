#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Crash reports.

Each kind of fatal failure leaves exactly one report in
<datadir>/crash_reports holding the reason, the crashing thread and the call
stack, but no path from this machine. The next start announces the report
once, and -crashreports=0 turns the reporter off.
"""

import platform
import re
import signal

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal


class CrashReportTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def setup_network(self):
        self.add_nodes(self.num_nodes)

    def reports(self):
        directory = self.nodes[0].chain_path / "crash_reports"
        return sorted(directory.glob("crash-*.txt")) if directory.exists() else []

    def crash(self, args, sig, stderr):
        node = self.nodes[0]
        before = self.reports()
        node.start(extra_args=args)
        node.wait_until_stopped(expected_ret_code=-sig, expected_stderr=re.compile(re.escape(stderr)))
        return [path for path in self.reports() if path not in before]

    def run_test(self):
        if platform.system() == "Windows":
            raise SkipTest("POSIX signal exit codes")
        node = self.nodes[0]

        self.log.info("A run that does not crash leaves no report directory")
        self.start_node(0)
        self.stop_node(0)
        assert not (node.chain_path / "crash_reports").exists()

        cases = [
            ("segv", signal.SIGSEGV, "", ["reason: fatal signal", "signal: 11 (SIGSEGV)", "fault_address: 0x0", "thread: init"]),
            ("abort", signal.SIGABRT, "", ["reason: abort", "signal: 6 (SIGABRT)", "thread: init"]),
            ("check", signal.SIGABRT, "Assertion `null_pointer' failed",
             ["reason: abort", "thread: init", "message: ", "Assertion `null_pointer' failed."]),
            ("throw", signal.SIGABRT, "uncaught exception: crash report test exception",
             ["reason: abort", "thread: crashtest", "message: uncaught exception: crash report test exception"]),
        ]
        for kind, sig, stderr, expected in cases:
            self.log.info(f"A {kind} crash leaves one report with its reason")
            new = self.crash([f"-crashreporttest={kind}"], sig, stderr)
            assert_equal(len(new), 1)
            text = new[0].read_text()
            for line in ["B3 crash report", "version: ", "os: ", "started: ", "uptime_seconds: ", "frames:\n  #0 "] + expected:
                assert line in text, f"{line!r} missing from {kind} report:\n{text}"
            assert str(node.datadir_path) not in text
            assert re.search(r"#\d+ b3coind\+0x[0-9a-f]+\n", text), text

        self.log.info("The next start announces the newest report, once")
        newest = self.reports()[-1].name
        with node.assert_debug_log(["The previous run ended in a crash; its report is", newest]):
            self.start_node(0)
        with node.assert_debug_log(expected_msgs=[], unexpected_msgs=["The previous run ended in a crash"]):
            self.restart_node(0)
        self.stop_node(0)

        self.log.info("-crashreports=0 writes no report")
        assert_equal(self.crash(["-crashreports=0", "-crashreporttest=abort"], signal.SIGABRT, ""), [])


if __name__ == '__main__':
    CrashReportTest(__file__).main()
