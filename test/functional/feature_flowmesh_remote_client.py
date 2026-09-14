#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Ordinary engine-off client against the existing four-validator FMN2 fixture.

All wallets, TLS keys, actions and B3 funds are isolated regtest fixtures. The
fifth client is not an FN, does not stake, and never synchronizes the optional
execution store. This daemon test is not a claim that Qt was exercised.
"""

import argparse
import hashlib
import json
import time
from decimal import Decimal
from pathlib import Path

from feature_flowmesh_independent import FlowMeshIndependentTest
from feature_flowmesh_release import B3_ARGS, TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.flowmesh_client_tls import FlowMeshTLSFaultRelay, create_test_pki
from test_framework.flowmesh_qt_review import FlowMeshQtReviewController
from test_framework.flowmesh_validity import assert_flowmesh_block_parity, assert_flowmesh_mempool_parity
from test_framework.test_node import TestNode
from test_framework.util import assert_equal, assert_raises_rpc_error, get_datadir_path, initialize_datadir, p2p_port, rpc_port


class FlowMeshRemoteClientTest(FlowMeshIndependentTest):
    def add_options(self, parser):
        super().add_options(parser)
        parser.add_argument("--check-default-off", action="store_true",
                            help="After explicit engine-off qualification, also require startup without the enable flag to be off")

        def bounded_hold(value):
            seconds = int(value)
            if not 0 <= seconds <= 900:
                raise argparse.ArgumentTypeError("Qt review hold must be between 0 and 900 seconds")
            return seconds

        parser.add_argument("--qt-review-hold-seconds", type=bounded_hold, default=0,
                            help="After automated success stop only client4 and retain the isolated operators/proxies for bounded manual Qt review (0-900, default0)")

    def set_test_params(self):
        super().set_test_params()
        # Stay within the framework's twelve-node port range. RPC slots0-4
        # are in use; these HTTPS-only listeners occupy otherwise unused ones.
        self.api_ports = [rpc_port(8), rpc_port(9)]
        self.client_api_ports = [rpc_port(10), rpc_port(11)]
        self.client = None
        self.client_args = []
        self.client_clock = None
        self.tls_relays = []
        self.api_args = [[] for _ in range(4)]
        self.client_results = []
        self.adversarial_results = []
        self.compatibility = []
        for args in self.extra_args:
            args += ["-enableflowmeshvalidator=1", "-debug=bench"]

    def run_test(self):
        super().run_test()

    def setup_network(self):
        self.pki = create_test_pki(Path(self.options.tmpdir, "flowmesh-client-tls"))
        for index, port in enumerate(self.api_ports):
            self.api_args[index] = ["-flowmeshapi=1", "-flowmeshapibind=127.0.0.1",
                                    f"-flowmeshapiport={port}",
                                    f"-flowmeshapicert={self.pki['certificate']}",
                                    f"-flowmeshapikey={self.pki['key']}"]
            self.extra_args[index] += self.api_args[index]
        super().setup_network()

    def restart_node(self, i, extra_args=None, clear_addrman=False, *, expected_stderr=""):
        if extra_args is not None:
            extra_args = [arg for arg in extra_args if not arg.startswith("-enableflowmeshvalidator=")]
            extra_args += ["-enableflowmeshvalidator=1", "-debug=bench", *self.api_args[i]]
        super().restart_node(i, extra_args, clear_addrman, expected_stderr=expected_stderr)

    def set_chain_time(self, timestamp):
        super().set_chain_time(timestamp)
        self.sync_client_time()

    def sync_client_time(self):
        if self.client is not None and self.client.running and self.client_clock != self.mock_time:
            self.client.setmocktime(self.mock_time)
            self.client_clock = self.mock_time

    def pump_b3(self):
        super().pump_b3()
        self.sync_client_time()

    def start_ordinary_client(self):
        for port, upstream in zip(self.client_api_ports, self.api_ports):
            self.tls_relays.append(FlowMeshTLSFaultRelay(port, upstream, self.pki))
        initialize_datadir(self.options.tmpdir, 4, self.chain, self.disable_autoconnect)
        self.client_args = [*B3_ARGS, "-enableflowmeshvalidator=0", "-debug=bench",
                            f"-port={p2p_port(12)}", f"-bind=127.0.0.1:{p2p_port(12)}",
                            f"-flowmeshendpointca={self.pki['ca']}",
                            *[f"-flowmeshendpoint={relay.url}" for relay in self.tls_relays]]
        # Framework add_nodes(1) creates index0, not the next index. Construct
        # this ordinary node explicitly and keep operator-only loops unchanged.
        self.client = TestNode(4, get_datadir_path(self.options.tmpdir, 4), chain=self.chain,
                               rpchost=None, timewait=self.rpc_timeout,
                               timeout_factor=self.options.timeout_factor, binaries=self.get_binaries(),
                               coverage_dir=self.options.coveragedir, cwd=self.options.tmpdir,
                               extra_args=self.client_args, uses_wallet=True)
        self.client.start()
        self.client.wait_for_rpc_connection()
        self.client.createwallet(wallet_name=self.default_wallet_name, load_on_startup=True)
        self.sync_client_time()
        self.client.addnode(f"127.0.0.1:{p2p_port(0)}", "onetry")
        self.wait_client_b3_sync()
        self.assert_engine_off()

    def wait_client_b3_sync(self):
        self.wait_until(lambda: self.client.getbestblockhash() == self.nodes[0].getbestblockhash(),
                        timeout=120, check_interval=.05)

    def assert_engine_off(self):
        info = self.client.getflowmeshclientinfo()
        assert_equal(info["backend"], "remote")
        assert_equal(info["engine_enabled"], False)
        assert_equal(len(info["endpoints"]), 2)
        validator = self.client.getflowmeshvalidatorinfo()
        assert_equal(validator["service_available"], False)
        assert_equal(validator["armed"], False)
        assert_equal(validator["wallet_key_count"], 0)
        # New, isolated datadir: there is no pre-existing journal to preserve.
        # Remote receipt/cursor storage is permitted, optional engine/network
        # storage is not. No production datadir is inspected by this assertion.
        assert not (self.client.chain_path / "flowmesh" / "network").exists()
        if self.last_market:
            assert not (self.client.chain_path / "flowmesh" / self.last_market).exists()
        self.compatibility.append({"engine_off": True, "B3_tip": self.client.getbestblockhash(),
                                   "height": self.client.getblockcount(), "validator": validator})

    def publish_client_transaction(self, transaction):
        # The client has already broadcast its prepared exact bytes. Mock-time
        # fixtures explicitly relay them so wallet INV trickle is not measured.
        for node in self.nodes:
            if transaction["txid"] not in node.getrawmempool():
                assert_equal(node.sendrawtransaction(transaction["hex"]), transaction["txid"])
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_client_b3_sync()

    def publish_parity_transaction(self, transaction, record_type):
        """First type8/type9 also passes authenticated block validation on all5.

        This helper, not sendrawtransaction, publishes the positive-control
        transaction in one exact block. Repeated checkpoint publications keep
        their original transaction checks without repeating the block fixture.
        """
        self.end_b3_workload()
        self.sync_blocks(timeout=120)
        self.wait_client_b3_sync()
        parity = assert_flowmesh_mempool_parity(self.nodes, self.client, transaction["hex"], record_type)
        row = {"prepared_txid": transaction["txid"], "decisions": parity["valid_decisions"], "validation": parity}
        self.compatibility.append(row)
        if not any(item.get("block_validation", {}).get("record_type") == record_type for item in self.compatibility):
            self.log.info("Independent actual-block validation parity for FlowMesh type%d", record_type)
            row["block_validation"] = assert_flowmesh_block_parity(
                self.nodes, self.client, transaction["hex"], record_type,
                fixture_root=self.options.tmpdir, advance_time=self.set_chain_time)
            self.wait_until(lambda: all(node.getconnectioncount() > 0 for node in [*self.nodes, self.client]), timeout=30)
            self.wait_client_b3_sync()
            for node in [*self.nodes, self.client]:
                node.syncwithvalidationinterfacequeue()
        else:
            assert_equal(self.client.sendrawtransaction(transaction["hex"]), transaction["txid"])
            self.publish_client_transaction(transaction)
        return row

    def prepare_and_publish_deposit(self, node, market_id, base_asset, deposit_asset, amount):
        """Check the asset-wallet option parser, then publish one exact deposit.

        flowmeshdeposit uses ParseAssetRpcOptions/get_bool, unlike the
        checkpoint/vault RPCs' custom ParseBroadcastOption error contract.
        Invalid calls must still fail before creating an account or changing
        wallet transactions, input locks or the mempool.
        """
        node.syncwithvalidationinterfacequeue()
        txcount = node.getwalletinfo()["txcount"]
        mempool = set(node.getrawmempool())
        locked_coins = node.listlockunspent()
        account_before = node.getflowmeshbalance(market_id).get("account")

        def unchanged():
            node.syncwithvalidationinterfacequeue()
            assert_equal(node.getwalletinfo()["txcount"], txcount)
            assert_equal(set(node.getrawmempool()), mempool)
            assert_equal(node.listlockunspent(), locked_coins)

        invalid_options = [
            (-8, "Unknown option 'unexpected'", {"broadcast": False, "unexpected": True}),
            (-3, "Wrong type passed", False),
        ]
        for value, kind in ((None, "null"), ("false", "string"), (0, "number"), ([], "array"), ({}, "object")):
            invalid_options.append((-3, f"JSON value of type {kind} is not of expected type bool", {"broadcast": value}))
        for code, message, options in invalid_options:
            assert_raises_rpc_error(code, message, node.flowmeshdeposit, base_asset, deposit_asset, amount, options)
            unchanged()
        assert_equal(node.getflowmeshbalance(market_id).get("account"), account_before)

        prepared = node.flowmeshdeposit(base_asset, deposit_asset, amount, {"broadcast": False})
        assert_equal(prepared["broadcast"], False)
        assert_equal(prepared["market_id"], market_id)
        assert_equal(prepared["amount"], amount)
        assert_equal(prepared["deposit_txid"], prepared["txid"])
        unchanged()
        assert_raises_rpc_error(-5, "Invalid or non-wallet transaction id", node.gettransaction, prepared["txid"])
        decoded = node.decoderawtransaction(prepared["hex"])
        assert_equal(decoded["txid"], prepared["txid"])
        assert_equal(decoded["ptxid"], prepared["ptxid"])
        assert_equal(node.sendrawtransaction(prepared["hex"]), prepared["txid"])
        node.syncwithvalidationinterfacequeue()
        assert prepared["txid"] in node.getrawmempool()
        assert_equal(node.getrawtransaction(prepared["txid"]), prepared["hex"])
        assert_equal(node.gettransaction(prepared["txid"])["hex"], prepared["hex"])
        return prepared

    def observe_action(self, market_id, method, params, label, *, expect_sequence=None):
        started = time.monotonic()
        attempts = []
        response = None
        known_pre_admission = {"FlowMesh service is reconciling the B3 tip",
                               "FlowMesh market is paused (at least four active seats are required)"}

        def submit():
            nonlocal response
            self.pump_b3()
            before = time.monotonic()
            try:
                response = getattr(self.client, method)(*params)
                attempts.append({"before_ms": (before - started) * 1000,
                                 "after_ms": (time.monotonic() - started) * 1000,
                                 "receipt_state": response["receipt_state"]})
                return True
            except JSONRPCException as error:
                # Only known explicit rejection before admission can re-enter
                # wallet signing. Unknown outcomes below use retry by ActionId.
                if error.error.get("code") != -1 or error.error.get("message") not in known_pre_admission:
                    raise
                attempts.append({"before_ms": (before - started) * 1000,
                                 "after_ms": (time.monotonic() - started) * 1000,
                                 "explicit_pre_admission_rejection": error.error["message"]})
                return False

        self.wait_until(submit, timeout=30, check_interval=.1)
        response_observed_ms = (time.monotonic() - started) * 1000
        assert (response["receipt_state"] in {"queued", "admitted", "unknown", "certified_inclusion"} or
                (response["receipt_state"] == "rejected" and response["reason"] in known_pre_admission)), response
        action_id = response["action_id"]
        assert len(action_id) == 64
        if expect_sequence is not None:
            assert_equal(response["sequence"], expect_sequence)
        next_retry = time.monotonic() + 1
        observed = {}

        def certified():
            nonlocal next_retry
            self.pump_b3()
            status = self.client.getflowmeshactionstatus(market_id, action_id)
            assert_equal(status["action_id"], action_id)
            state = status["receipt_state"]
            assert state in {"queued", "admitted", "rejected", "unknown", "certified_inclusion"}, status
            retryable_rejection = state == "rejected" and status["reason"] in known_pre_admission
            if state == "rejected" and not retryable_rejection:
                raise AssertionError(f"Isolated test action was definitely rejected: {status}")
            if state == "admitted" and "admitted_observed_ms" not in observed:
                observed["admitted_observed_ms"] = (time.monotonic() - started) * 1000
            if (state == "unknown" or retryable_rejection) and time.monotonic() >= next_retry:
                retry = self.client.retryflowmeshaction(market_id, action_id)
                assert_equal(retry["action_id"], action_id)
                attempts.append({"same_signed_action_retry_ms": (time.monotonic() - started) * 1000,
                                 "receipt_state": retry["receipt_state"]})
                next_retry = time.monotonic() + 1
            if state != "certified_inclusion":
                return False
            assert_equal(status["certificate_verified"], True)
            assert_equal(status["outcome_verified"], False)
            observed["certified_inclusion_observed_ms"] = (time.monotonic() - started) * 1000
            observed["status"] = status
            return True

        self.wait_until(certified, timeout=90, check_interval=.1)
        sample = {"label": label, "action_id": action_id, "initial_response": response,
                  "initial_submission_monotonic_us": round(started * 1_000_000),
                  "initial_response_observed_ms": response_observed_ms,
                  "attempts": attempts, **observed,
                  "operator_delivery_observations": self.collect_delivery_trace(market_id)}
        self.client_results.append(sample)
        self.log.info("FLOWMESH_REMOTE_ACTION %s", json.dumps(sample, sort_keys=True, default=str))
        return sample

    def client_balance(self, market_id):
        data = self.client.getflowmeshmarketdata(market_id, {"limit": 1, "curve_limit": 2})
        assert_equal(data["market_id"], market_id)
        verification = data["verification"]
        assert_equal(verification["source"], "remote_endpoint")
        assert_equal(verification["certificate_verified"], True)
        assert_equal(verification["account_state_verified"], True)
        assert_equal(verification["execution_result_verified"], False)
        assert_equal(verification["freshest_network_head_proven"], False)
        assert_equal(verification["history_endpoint_reported"], True)
        account = dict(data["account"])
        account["b3_available"] = Decimal(account["b3_available_atoms"]) / Decimal(1_000_000_000)
        account["b3_reserved"] = Decimal(account["b3_reserved_atoms"]) / Decimal(1_000_000_000)
        return account

    def wait_client_balance(self, market_id, predicate):
        def ready():
            self.pump_b3()
            return predicate(self.client_balance(market_id))
        self.wait_until(ready, timeout=90, check_interval=.1)
        return self.client_balance(market_id)

    def wait_vault_operation(self, market_id, account_id, kind, asset):
        deadline = time.monotonic() + 90
        published = 0
        while time.monotonic() < deadline:
            operations = self.client.listflowmeshvaultoperations(market_id)
            matches = [operation for operation in operations if operation["account_id"] == account_id and
                       operation["kind"] == kind and operation["asset"] == asset]
            if matches:
                assert_equal(len(matches), 1)
                return matches[0]
            if self.market_status(self.nodes[0], market_id)["checkpoint_pending"]:
                assert published < 16, "Remote vault checkpoint bound exhausted"
                checkpoint = self.client.createflowmeshcheckpoint(market_id, {"broadcast": False})
                assert_equal(checkpoint["broadcast"], False)
                row = self.publish_parity_transaction(checkpoint, 8)
                row["engine_off_prepared_checkpoint"] = checkpoint["txid"]
                published += 1
                self.wait_client_b3_sync()
            else:
                self.pump_b3()
                time.sleep(.1)
        raise AssertionError("Expected connected remote vault operation did not appear")

    def adversarial_reads(self, market_id):
        """Bounded hostile endpoint replies; no new action or engine process."""
        expected = self.client_balance(market_id)
        pending = self.client.getflowmeshclientinfo()["pending_actions"]

        def submissions():
            return sum(row["method"] == "submit" for relay in self.tls_relays
                       for row in relay.snapshot()["requests"])

        initial_submissions = submissions()
        fields = ("account_id", "next_sequence", "base_available", "base_reserved",
                  "b3_available_atoms", "b3_reserved_atoms")
        cases = ("wrong_domain", "wrong_market", "missing_state", "missing_certificate",
                 "tampered_state", "tampered_certificate", "regressing_head_claim",
                 "malformed_json", "oversized_reply", "false_account_row", "unavailable")
        for case in cases:
            seen = []

            def mutate(method, body):
                if method not in {"updates", "snapshot"}:
                    return body
                if case == "malformed_json":
                    seen.append(method)
                    return b'{"ok":'
                if case == "oversized_reply":
                    seen.append(method)
                    # One byte above the production 24 MiB client limit,
                    # still below the test relay's own 32 MiB response cap.
                    return b" " * (24 * 1024 * 1024 + 1)
                reply = json.loads(body)
                if not reply.get("ok"):
                    return body
                result = reply["result"]
                if case in {"wrong_domain", "wrong_market"}:
                    result["status"]["domain" if case == "wrong_domain" else "market_id"] = "a5" * 32
                    seen.append(method)
                elif case == "regressing_head_claim":
                    assert result["status"]["next_microblock_sequence"] > 0
                    result["status"]["next_microblock_sequence"] = 0
                    seen.append(method)
                else:
                    if method == "updates":
                        # Require a full proof fetch, even for an unchanged
                        # durable head. This does not rewrite signed bytes.
                        result["gap"] = True
                    if method == "snapshot":
                        seen.append(method)
                        if case in {"missing_state", "missing_certificate"}:
                            result.pop("state_bytes" if case == "missing_state" else "certified_payload")
                        elif case in {"tampered_state", "tampered_certificate"}:
                            key = "state_bytes" if case == "tampered_state" else "certified_payload"
                            raw = bytearray.fromhex(result[key])
                            assert raw
                            raw[-1] ^= 1
                            result[key] = raw.hex()
                        elif case == "false_account_row":
                            row = result["reported_data"]["account"]
                            row["base_available"] = expected["base_available"] + 999_999
                            row["b3_available_atoms"] = expected["b3_available_atoms"] + 999_999
                            row["next_sequence"] = expected["next_sequence"] + 100
                return json.dumps(reply, separators=(",", ":")).encode()

            started = time.monotonic()
            for relay in self.tls_relays:
                relay.configure(unavailable=case == "unavailable", reply_mutation=mutate)
            try:
                try:
                    data = self.client.getflowmeshmarketdata(market_id, {"limit": 1, "curve_limit": 2})
                except JSONRPCException as error:
                    assert case != "false_account_row", error.error
                    assert_equal(error.error["code"], -1)
                    assert "budget" not in error.error["message"].lower(), error.error
                    outcome = {"refused": True, "reason": error.error["message"]}
                else:
                    assert_equal(case, "false_account_row")
                    assert_equal(data["verification"]["account_state_verified"], True)
                    assert_equal(data["verification"]["history_endpoint_reported"], True)
                    for field in fields:
                        assert_equal(data["account"][field], expected[field])
                    outcome = {"false_row_ignored": True}
                assert case == "unavailable" or seen, f"Fault {case} did not reach the requested path"
                assert_equal(submissions(), initial_submissions)
                assert_equal(self.client.getflowmeshclientinfo()["pending_actions"], pending)
                self.adversarial_results.append({"case": case, "elapsed_ms": (time.monotonic() - started) * 1000,
                                                 "mutated_methods": seen, **outcome})
            finally:
                for relay in self.tls_relays:
                    relay.configure()
            recovered = self.client_balance(market_id)
            for field in fields:
                assert_equal(recovered[field], expected[field])
            # Keep deliberate hostile-read checks below the unchanged public
            # per-IP budget; do not misclassify unrelated rate refusal as a
            # successful tampered-proof rejection.
            time.sleep(max(0, 1.05 - (time.monotonic() - started)))
        assert all(relay.snapshot()["records_dropped"] == 0 for relay in self.tls_relays)

    def unchanged_head_cursor_scopes(self, market_id):
        """A gap cannot be satisfied merely by a concurrently advancing head."""
        baseline = self.client.getflowmeshbalance(market_id)
        head_fields = ("last_microblock_hash", "state_root", "next_microblock_sequence")
        head = {field: baseline[field] for field in head_fields}
        info = self.client.getflowmeshclientinfo()
        initial_submissions = sum(row["method"] == "submit" for relay in self.tls_relays
                                  for row in relay.snapshot()["requests"])
        active = next(relay for relay in self.tls_relays if relay.url == info["active_endpoint"])
        other = next(relay for relay in self.tls_relays if relay is not active)
        active.configure(unavailable=True)
        try:
            switched = self.client.getflowmeshbalance(market_id)
            after = self.client.getflowmeshclientinfo()
            assert_equal({field: switched[field] for field in head_fields}, head)
            assert_equal(switched["account"], baseline["account"])
            assert_equal(after["active_endpoint"], other.url)
            assert after["event_gaps"] > info["event_gaps"]
            self.adversarial_results.append({"case": "same_head_endpoint_instance_gap", "head": head,
                                             "from_endpoint": active.url, "to_endpoint": other.url,
                                             "gap_delta": after["event_gaps"] - info["event_gaps"]})
        finally:
            active.configure()

        self.client.createwallet(wallet_name="cursor_scope_only", load_on_startup=False)
        selected = self.client.get_wallet_rpc(self.default_wallet_name)
        accountless = self.client.get_wallet_rpc("cursor_scope_only")
        try:
            before_scope = selected.getflowmeshclientinfo()["event_gaps"]
            empty = accountless.getflowmeshbalance(market_id)
            assert "account" not in empty
            assert_equal({field: empty[field] for field in head_fields}, head)
            assert_equal(empty["verification"]["certificate_verified"], True)
            empty_gaps = selected.getflowmeshclientinfo()["event_gaps"]
            assert empty_gaps > before_scope
            restored = selected.getflowmeshbalance(market_id)
            restored_gaps = selected.getflowmeshclientinfo()["event_gaps"]
            assert restored_gaps > empty_gaps
            assert_equal({field: restored[field] for field in head_fields}, head)
            assert_equal(restored["account"], baseline["account"])
            self.adversarial_results.append({"case": "same_head_account_scope_gap", "head": head,
                                             "accountless_gap_delta": empty_gaps - before_scope,
                                             "restored_account_gap_delta": restored_gaps - empty_gaps,
                                             "accountless_read_created_account": False})
        finally:
            self.client.unloadwallet("cursor_scope_only")
        final_submissions = sum(row["method"] == "submit" for relay in self.tls_relays
                                for row in relay.snapshot()["requests"])
        assert_equal(final_submissions, initial_submissions)

    def retained_certified_restart(self, market_id, action_id):
        """A durable no-replay marker survives an ordinary client restart."""
        resolved = self.client.getflowmeshactionstatus(market_id, action_id)
        assert_equal(resolved["receipt_state"], "certified_inclusion")
        assert_equal(resolved["certificate_verified"], True)
        # Receipt cards discover original local instructions, not new remote
        # events. Enumeration must not query endpoints or mutate the outbox.
        def saved_view(rpc, market=None):
            before_counts = [len(relay.snapshot()["requests"]) for relay in self.tls_relays]
            view = rpc.listflowmeshactions(*([] if market is None else [market]))
            assert_equal(view["source"], "local-retained-outbox")
            assert len(view["actions"]) <= 512
            assert_equal(len({(row["market_id"], row["action_id"]) for row in view["actions"]}), len(view["actions"]))
            for row in view["actions"]:
                assert_equal(row["account_id"], view["account_id"])
                assert_equal(row["receipt"]["action_id"], row["action_id"])
                if market is not None:
                    assert_equal(row["market_id"], market)
            assert_equal([len(relay.snapshot()["requests"]) for relay in self.tls_relays], before_counts)
            return view

        before_saved = saved_view(self.client, market_id)
        retained_before = {row["action_id"]: row for row in before_saved["actions"]}
        original = retained_before[action_id]
        assert_equal(original["previously_certified"], True)
        transmitted = [row for relay in self.tls_relays for row in relay.snapshot()["requests"]
                       if row["method"] == "submit" and row["action_id"] == action_id]
        assert transmitted
        exact = bytes.fromhex(transmitted[0]["action_hex"])
        assert_equal(original["signed_bytes_sha256"], hashlib.sha256(exact).hexdigest())
        assert_equal(original["signed_bytes_size"], len(exact))
        self.client.createwallet(wallet_name="unrelated_client_wallet", load_on_startup=False)
        unrelated = self.client.get_wallet_rpc("unrelated_client_wallet")
        unrelated_saved = saved_view(unrelated)
        assert_equal(unrelated_saved["actions"], [])
        assert "account_id" not in unrelated_saved
        for method in (unrelated.getflowmeshactionstatus, unrelated.retryflowmeshaction):
            assert_raises_rpc_error(-4, "This wallet has no FlowMesh account", method, market_id, action_id)
        self.client.unloadwallet("unrelated_client_wallet")
        submissions_before = sum(row["method"] == "submit" for relay in self.tls_relays
                                 for row in relay.snapshot()["requests"])
        unknown_replies = []

        def no_current_receipt(method, body):
            if method != "action":
                return body
            reply = json.loads(body)
            if reply.get("ok"):
                receipt = reply["result"]
                if receipt["action_id"] == action_id:
                    receipt.update(receipt_state="unknown", accepted=False, certificate_verified=False,
                                   outcome_verified=False, reason="Synthetic endpoint receipt-ring expiry")
                    for key in ("certified_payload", "microblock_hash", "microblock_sequence"):
                        receipt.pop(key, None)
                    unknown_replies.append(action_id)
            return json.dumps(reply, separators=(",", ":")).encode()

        for relay in self.tls_relays:
            relay.configure(reply_mutation=no_current_receipt)
        try:
            self.client.stop_node()
            restart_args = list(self.client_args)
            if self.options.check_default_off:
                restart_args = [arg for arg in restart_args if not arg.startswith("-enableflowmeshvalidator=")]
            self.client.start(extra_args=restart_args)
            self.client.wait_for_rpc_connection()
            self.client_clock = None
            self.sync_client_time()
            self.client.addnode(f"127.0.0.1:{p2p_port(0)}", "onetry")
            self.wait_client_b3_sync()
            # No fresh remote evidence has been requested for these cards.
            # Historical no-resubmit remains true, current verification false.
            restored_saved = saved_view(self.client, market_id)
            retained_restored = {row["action_id"]: row for row in restored_saved["actions"]}
            assert_equal(set(retained_restored), set(retained_before))
            identity_fields = ("market_id", "domain", "execution_config_id", "account_id", "action_id",
                               "action_type", "sequence", "signed_bytes_sha256", "signed_bytes_size",
                               "initial_submission_ms", "may_have_been_sent", "previously_certified")
            for identity, row in retained_before.items():
                for field in identity_fields:
                    assert_equal(retained_restored[identity].get(field), row.get(field))
            assert_equal(retained_restored[action_id]["previously_certified"], True)
            assert_equal(retained_restored[action_id]["receipt"]["certificate_verified"], False)
            assert_equal(retained_restored[action_id]["receipt"]["receipt_state"], "unknown")
            status = self.client.getflowmeshactionstatus(market_id, action_id)
            assert_equal(status["action_id"], action_id)
            assert_equal(status["receipt_state"], "unknown")
            assert_equal(status["certificate_verified"], False)
            retry = self.client.retryflowmeshaction(market_id, action_id)
            assert_equal(retry["action_id"], action_id)
            assert_equal(retry["receipt_state"], "unknown")
            assert_equal(retry["certificate_verified"], False)
            assert "not be resubmitted" in retry["reason"], retry
            assert unknown_replies
        finally:
            for relay in self.tls_relays:
                relay.configure()
        recovered = self.client.getflowmeshactionstatus(market_id, action_id)
        assert_equal(recovered["receipt_state"], "certified_inclusion")
        assert_equal(recovered["certificate_verified"], True)
        recovered_saved = {row["action_id"]: row for row in saved_view(self.client, market_id)["actions"]}
        assert_equal(recovered_saved[action_id]["signed_bytes_sha256"], original["signed_bytes_sha256"])
        assert_equal(recovered_saved[action_id]["sequence"], original["sequence"])
        assert_equal(recovered_saved[action_id]["previously_certified"], True)
        assert_equal(recovered_saved[action_id]["receipt"]["certificate_verified"], True)
        # Head high-water and exact-action replay protection are durable; the
        # event cursor is not. First state recovery must declare that lost
        # continuity rather than treating a fresh snapshot as event replay.
        resumed = self.client.getflowmeshmarketdata(market_id, {"limit": 1, "curve_limit": 2})
        assert_equal(resumed["verification"]["certificate_verified"], True)
        assert_equal(resumed["verification"]["account_state_verified"], True)
        assert_equal(resumed["verification"]["event_gap"], True)
        resumed_gaps = self.client.getflowmeshclientinfo()["event_gaps"]
        assert resumed_gaps > 0
        submissions_after = sum(row["method"] == "submit" for relay in self.tls_relays
                                for row in relay.snapshot()["requests"])
        assert_equal(submissions_after, submissions_before)
        self.assert_engine_off()
        self.adversarial_results.append({"case": "certified_restart_unknown_never_resubmits",
                                         "action_id": action_id, "submit_delta": 0,
                                         "saved_card_enumeration_local_only": True,
                                         "saved_card_wrong_wallet_empty": True,
                                         "saved_original_sha256": original["signed_bytes_sha256"],
                                         "saved_original_sequence": original["sequence"],
                                         "saved_identities_unchanged_across_restart": len(retained_before),
                                         "unknown_replies": len(unknown_replies), "fresh_proof_recovered": True,
                                         "restart_snapshot_event_gap": True, "resumed_event_gaps": resumed_gaps})

    def exercise_remote_client(self, market_id):
        n0 = self.nodes[0]
        market = self.market_status(n0, market_id)
        asset = market["base_asset_id"]
        self.start_ordinary_client()
        self.wait_until(lambda: any(row["market_id"] == market_id for row in self.client.listflowmeshmarkets()), timeout=60)
        for _ in range(2):
            accountless = self.client.getflowmeshbalance(market_id)
            assert "account" not in accountless
            assert_equal(accountless["verification"]["certificate_verified"], True)
            assert_equal(accountless["verification"]["source"], "remote_endpoint")
        # No account or FN key was imported from any operator wallet.
        funding_address = self.client.getnewaddress()
        funding_txid = n0.sendtoaddress(funding_address, Decimal("3"))
        self.synchronize_mempools()
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_client_b3_sync()
        assert self.client.gettransaction(funding_txid)["confirmations"] >= 1
        deposit = self.prepare_and_publish_deposit(self.client, market_id, asset, "B3", Decimal("1"))
        self.publish_client_transaction(deposit)
        self.mine_pos_blocks(30, allow_overshoot=True)
        self.wait_client_b3_sync()
        recovery = self.observe_action(market_id, "submitflowmeshdeposit",
                                       (market_id, deposit["deposit_txid"], deposit["deposit_vout"]), "remote_deposit")
        recovery["measurement_class"] = "post_reindex_recovery_not_normal_baseline"
        account = self.wait_client_balance(market_id, lambda row: row["b3_available"] == Decimal("1"))
        assert_equal(account["base_available"], 0)
        assert_equal(self.client.getflowmeshbalance(market_id)["account"]["account_id"], account["account_id"])
        sweep = self.wait_vault_operation(market_id, account["account_id"], "deposit-sweep", "B3")
        swept = self.prepare_and_publish(self.client, self.client.createflowmeshvaulttx, sweep["effect_id"], None)
        self.publish_client_transaction(swept)
        self.adversarial_reads(market_id)
        self.unchanged_head_cursor_scopes(market_id)

        self.begin_b3_workload(range(4))
        height_before = n0.getblockcount()
        before = self.client_balance(market_id)
        info = self.client.getflowmeshclientinfo()
        active = next(relay for relay in self.tls_relays if relay.url == info["active_endpoint"])
        other = next(relay for relay in self.tls_relays if relay is not active)
        gaps_before = info["event_gaps"]
        active.configure(drop_submit_once=True)
        synthetic_rejections = []

        def reject_after_ambiguous_delivery(method, body):
            if method != "submit":
                return body
            reply = json.loads(body)
            if reply.get("ok") and not synthetic_rejections:
                receipt = reply["result"]
                receipt.update(receipt_state="rejected", accepted=False, certificate_verified=False,
                               outcome_verified=False, reason="Synthetic endpoint rejection after an earlier ambiguous delivery")
                receipt.pop("microblock_hash", None)
                receipt.pop("microblock_sequence", None)
                synthetic_rejections.append(receipt["action_id"])
            return json.dumps(reply, separators=(",", ":")).encode()

        other.configure(reply_mutation=reject_after_ambiguous_delivery)
        bid = self.observe_action(market_id, "submitflowmeshorder",
                                  (market_id, "bid", TRADE_PRICE, 1, before["next_sequence"]),
                                  "ambiguous_admission_failover", expect_sequence=before["next_sequence"])
        bid_id = bid["action_id"]
        initial = [row for row in active.snapshot()["requests"] if row["action_id"] == bid_id and row["response_dropped"]]
        assert_equal(len(initial), 1)
        assert_equal(synthetic_rejections, [bid_id])
        assert_equal(bid["initial_response"]["receipt_state"], "unknown")
        other.configure()
        # A verified certificate resolves uncertainty without another submit.
        # If resolution instead needed a retry while unknown, every submitted
        # instruction at the replacement endpoint must be byte-for-byte exact.
        resolved = self.client.getflowmeshactionstatus(market_id, bid_id)
        assert_equal(resolved["action_id"], bid_id)
        assert_equal(resolved["receipt_state"], "certified_inclusion")
        assert_equal(resolved["certificate_verified"], True)
        forwarded = [row for row in other.snapshot()["requests"]
                     if row["method"] == "submit" and row["action_id"] == bid_id and row["forwarded"]]
        assert all(row["action_hex"] == initial[0]["action_hex"] for row in forwarded)
        bid["failover_resolution"] = "exact_saved_action_retry" if forwarded else "verified_status_only"
        self.wait_client_balance(market_id, lambda row: row["next_sequence"] == before["next_sequence"] + 1)
        self.wait_until(lambda: self.client.getflowmeshclientinfo()["active_endpoint"] == other.url, timeout=30)
        self.wait_until(lambda: self.client.getflowmeshclientinfo()["event_gaps"] > gaps_before, timeout=30)

        seller_before = n0.getflowmeshbalance(market_id)["account"]
        self.submit_observed(market_id, 0, "ask", price=TRADE_PRICE, quantity=1,
                             label="remote_client_matched_trade")
        filled = self.wait_client_balance(market_id, lambda row: row["base_available"] == 1 and row["b3_reserved"] == 0)
        assert_equal(filled["b3_available"], Decimal("0.9"))
        seller_after = n0.getflowmeshbalance(market_id)["account"]
        assert_equal(seller_after["base_available"], seller_before["base_available"] - 1)
        assert_equal(seller_after["b3_available"] - seller_before["b3_available"], Decimal("0.09999"))
        self.wait_for_new_b3_block(height_before)

        # A separate no-fault normal sample, not the first post-reindex action
        # or the ambiguous-admission failover sample. Observe one common head
        # before submission, retain the submission clock across pre-admission
        # retries, and require ordinary B3 advancement during this phase.
        for relay in self.tls_relays:
            relay.configure()
        common = {}

        def common_certified_head():
            self.pump_b3()
            rows = [self.market_status(node, market_id) for node in self.nodes]
            if any(row is None or row["paused"] for row in rows):
                return False
            heads = [(row["next_microblock_sequence"], row["last_microblock_hash"]) for row in rows]
            if len(set(heads)) != 1:
                return False
            common.update(sequence=heads[0][0], hash=heads[0][1], B3_height=n0.getblockcount())
            return True

        self.wait_until(common_certified_head, timeout=90, check_interval=.1)
        opened = self.observe_action(market_id, "submitflowmeshorder",
                                     (market_id, "bid", TRADE_PRICE // 2, 1, filled["next_sequence"]),
                                     "remote_standing_bid", expect_sequence=filled["next_sequence"])
        opened.update(measurement_class="normal_common_head_no_injected_faults", common_head_before=common)
        self.wait_for_new_b3_block(common["B3_height"])
        opened["B3_height_after"] = n0.getblockcount()
        standing = self.wait_client_balance(market_id, lambda row: row["b3_reserved"] == Decimal("0.05"))
        self.observe_action(market_id, "cancelflowmeshorder",
                            (market_id, "bid", standing["next_sequence"]), "remote_cancel",
                            expect_sequence=standing["next_sequence"])
        cancelled = self.wait_client_balance(market_id, lambda row: row["b3_reserved"] == 0)
        assert_equal(cancelled["base_available"], 1)
        assert_equal(cancelled["b3_available"], Decimal("0.9"))
        assert_equal(cancelled["next_sequence"], filled["next_sequence"] + 2)
        assert opened["action_id"] != bid_id
        active.configure()
        self.end_b3_workload()

        destination = self.client.getnewaddress()
        self.observe_action(market_id, "requestflowmeshwithdrawal",
                            (market_id, asset, 1, destination, cancelled["next_sequence"]),
                            "remote_withdrawal", expect_sequence=cancelled["next_sequence"])
        self.wait_client_balance(market_id, lambda row: row["base_available"] == 0)
        operation = self.wait_vault_operation(market_id, cancelled["account_id"], "withdrawal", asset)
        payout = self.client.createflowmeshvaulttx(operation["effect_id"], destination, {"broadcast": False})
        assert_equal(payout["broadcast"], False)
        # The same exact valid type-9 transaction is accepted by enabled and
        # engine-off B3 validators before its single broadcast.
        row = self.publish_parity_transaction(payout, 9)
        row["valid_withdrawal_txid"] = payout["txid"]
        self.wait_until(lambda: self.wallet_asset(self.client, asset)["confirmed"] == 1, timeout=60)
        self.assert_engine_off()

        self.retained_certified_restart(market_id, bid_id)
        self.wait_for_independent_mesh()

    def qt_review_hold(self, market_id):
        seconds = self.options.qt_review_hold_seconds
        if not seconds:
            return
        # Preserve all evidence and the stopped client wallet even if a
        # manually launched Qt process outlives the bounded hold by mistake.
        self.options.nocleanup = True
        self.client.stop_node()
        complete = Path(self.options.tmpdir, "qt-review-complete")
        assert not complete.exists(), "Qt completion marker predates the isolated review window"
        artifact = Path(self.options.tmpdir, "qt-review.json")
        # createwallet(load_on_startup=True) already persists this wallet in
        # the generated node settings. A CLI -wallet appends a duplicate and
        # raises a modal startup warning; isolated Qt preferences do not
        # replace these node settings. Refuse an unexpected autoload list.
        settings = json.loads((self.client.chain_path / "settings.json").read_text(encoding="utf-8"))
        assert_equal(settings.get("wallet"), [self.default_wallet_name])
        startup_args = [f"-datadir={self.client.datadir_path}", "-regtest", "-server=1",
                        f"-mocktime={self.mock_time}",
                        f"-rpcport={rpc_port(4)}", f"-connect=127.0.0.1:{p2p_port(0)}", *self.client_args]
        review = {"automated_checks_succeeded": True, "Qt_path_exercised": False,
                  "client_stopped": True, "client_datadir": str(self.client.datadir_path),
                  "client_startup_args": startup_args, "market_id": market_id,
                  "operator_api_endpoints": [f"https://127.0.0.1:{port}" for port in self.api_ports],
                  "client_proxy_endpoints": [relay.url for relay in self.tls_relays],
                  "tls_ca": str(self.pki["ca"]), "mocktime": self.mock_time,
                  "client_B3_port": p2p_port(12), "client_RPC_port": rpc_port(4),
                  "hold_seconds": seconds, "completion_marker": str(complete),
                  "Qt_preferences_isolation_required": True,
                  "warning": "No Qt process is launched by this harness. Independently isolate Qt preferences before launch; stop that Qt process before writing the completion marker. Operators and proxies stop after this bounded window. No Qt qualification is inferred."}
        def mine_review_blocks(count):
            before = self.nodes[0].getblockcount()
            self.mine_pos_blocks(count)
            return [self.nodes[0].getblockhash(height) for height in range(before + 1, before + count + 1)]

        controller = FlowMeshQtReviewController(self.options.tmpdir, self.tls_relays, mine_blocks=mine_review_blocks)
        try:
            market = self.market_status(self.nodes[0], market_id)
            review.update(local_controls=controller.manifest(), base_asset_id=market["base_asset_id"],
                          trade_price_atoms=TRADE_PRICE,
                          operator_wallet_names=[self.default_wallet_name] * len(self.nodes),
                          operator_account_ids=[node.getflowmeshbalance(market_id).get("account", {}).get("account_id") for node in self.nodes])
            artifact.write_text(json.dumps(review, sort_keys=True, indent=2) + "\n", encoding="utf-8")
            self.log.info("FLOWMESH_QT_REVIEW_READY %s", artifact)
            deadline = time.monotonic() + seconds
            while not complete.exists() and time.monotonic() < deadline:
                controller.poll_once()
                time.sleep(min(.5, max(0, deadline - time.monotonic())))
            self.log.info("FLOWMESH_QT_REVIEW_HOLD_ENDED marker=%s; Qt remains unqualified by this harness", complete.exists())
        finally:
            controller.close()

    def qualification_workload(self, market_id):
        self.last_market = market_id
        success = False
        started = time.monotonic()
        try:
            self.exercise_remote_client(market_id)
            success = True
        finally:
            self.end_b3_workload()
            report = {"success": success, "elapsed_ms": (time.monotonic() - started) * 1000,
                      "actions": self.client_results, "compatibility": self.compatibility,
                      "adversarial_replies": self.adversarial_results,
                      "tls_relays": [relay.snapshot() for relay in self.tls_relays],
                      "ordinary_client_datadir": str(self.client.datadir_path) if self.client else None,
                      "tls_ca": str(self.pki["ca"]), "Qt_path_exercised": False,
                      "operator_api_endpoints": [f"https://127.0.0.1:{port}" for port in self.api_ports],
                      "node_debug_logs": [str(node.debug_log_path) for node in self.nodes] +
                                         ([str(self.client.debug_log_path)] if self.client else []),
                      "default_off_qualified": success and self.options.check_default_off,
                      "invalid_transaction_parity_qualified": success and
                          {row["validation"]["record_type"] for row in self.compatibility if "validation" in row} == {8, 9},
                      "WAN_qualified": False, "invalid_checkpoint_withdrawal_block_parity_qualified": success and
                          {row["block_validation"]["record_type"] for row in self.compatibility
                           if row.get("block_validation", {}).get("block_parity_qualified")} == {8, 9},
                      "measurement": "Initial submission includes retries; admission and verified inclusion observations are distinct. No independently replayed per-action outcome is inferred."}
            Path(self.options.tmpdir, "flowmesh-remote-client-qualification.json").write_text(
                json.dumps(report, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
            self.log.info("FLOWMESH_REMOTE_CLIENT_RESULT %s", json.dumps(report, sort_keys=True, default=str))
        if success:
            self.qt_review_hold(market_id)

    def shutdown(self):
        try:
            try:
                if self.client is not None and self.client.running:
                    self.client.stop_node()
            finally:
                for relay in self.tls_relays:
                    relay.stop()
        finally:
            # Always run the original operator/proxy cleanup, even if a client
            # cleanup error occurred; never turn that error into a passing run.
            exit_code = super().shutdown()
        return exit_code


if __name__ == "__main__":
    FlowMeshRemoteClientTest(__file__).main()
