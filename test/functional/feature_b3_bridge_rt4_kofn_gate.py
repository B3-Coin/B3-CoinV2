#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""RT4: bridge V2 with the default K_OF_N bootstrap and the full burn gate.

REGTEST ONLY, the environment of RT4a (feature_b3_bridge_rt4a_round_trip.py):
four B3 regtest validators linked only to each other, one anvil on
127.0.0.1 (chain 31337, never forked), the synthetic sync-committee tool,
forge --offline with its output outside the source tree, fresh datadirs. The
bootstrap keys are the public deterministic test keys of the REGTEST ONLY
b3-bridge-regtest-certify tool (B3_BRIDGE_TOOLS), which never reads a
secret key and refuses every other chain domain and EVM chain id.

RT4a's round trip (node changes rev 11 sections 7-8; owner ruling R9 on
OI-12) with these differences:

3. DeployV2.s.sol deploys with B3V2_BOOTSTRAP_MODE = 0 (K_OF_N) and the
   bootstrap committee of `b3-bridge-regtest-certify bootstrap-set` (four
   test keys of weight 1, quorum 3). BOOTSTRAP_SET_HASH is the tool's,
   MAX_EPOCH_LAG is profile R's 72,000 s, the node's gate set expiry (an
   OI-3 harness check: B3 cannot pin the verifier code), and
   BRIDGE_ACTIVATION_HEIGHT is V.
4. initialize: B3's epoch-0 certificate at 266 supplies fb, Set'_0, the
   successor and leaf0Path. bootstrapDigestOf(fb) equals the tool's digest
   and the Python derivation of the 301-byte OI-12 message (chain domain,
   chain id 31337, this verifier, START_SET_HASH, fb). Each of these reverts
   in eth_call first: the tool's attestation for another verifier address
   and a digest-V2 certificate by the bootstrap keys (ProofRejected: OI-12
   replay protection), Set'_0's own certificate proof under the bootstrap
   header (ProofRejected) and the START_CERTIFICATE calldata
   (BadSigningSet). The tool refuses the B3 mainnet domain, chain id 1 and
   2 of 4 signers. The correct 3-of-4 attestation makes the verifier LIVE.
8. The burn gate. With fresh finality near the tip but no observation every
   node reports NOT_READY at row E1 and bridgewithdraw is refused with that
   reason. After a light-client update to the anvil head a VERIFIER_E record
   whose verdict byte is tampered is refused by submitbridgecarrier;
   contrib/b3bridge/b3_bridge_observe_v2.py then submits VERIFIER_E,
   RESERVE_E and TOKEN_FLAGS (decoded values equal anvil reads; latestCertId
   is B3's certId; observed code hashes equal anvil's, the OI-3 evidence);
   the same VERIFIER_E record again is refused as not newer and the observer
   refuses the stale plan. READY on every node, reserve minted = deposit;
   the burn passes and reserve burned = burn.
13. Stale drill, after RT4a's release, relayer checks, pin guard and reindex
    (so the jumped clock leaves those steps untouched). With the gate open
    again (certificates relayed, light client at the head, new
    observations) a 1,000,000 burn is built without broadcasting it. B3 mock
    time jumps 5,000 s, past profile R's 4,800 s observation age, and one
    block is staked: every node reports NOT_READY at E1 ("VERIFIER_E
    observation is stale"), bridgewithdraw is refused and testmempoolaccept
    refuses the prebuilt burn with bridge-v2-burn-not-ready. anvil's clock
    jumps as far (B3 behind Ethereum, then even), the light client is
    updated and every subject observed again under mock-time production:
    READY, and testmempoolaccept accepts the prebuilt burn again.

Evidence (bridge_evidence.json): the K_OF_N initialize, the OI-12 negatives
and tool refusals, READY with observations, the E1 refusals and the stale
refusal and reopening.
"""

import json
import time

from feature_b3_bridge_rt4a_round_trip import (
    BRIDGE_V2_HEIGHT,
    BURN_FINALITY_LAG_MARGIN,
    BURN_RAW,
    ETH_RECIPIENT,
    RPC_VERIFY_REJECTED,
    B3BridgeRT4aRoundTripTest,
    error_selector,
    revert_data,
)
from test_framework.anvil import SRCDIR
from test_framework.b3bridge import (
    GATE_R_OBSERVATION_MAX_AGE,
    GATE_R_SET_EXPIRY,
    REGTEST_CERTIFY_TOOL,
    REGTEST_ETH_CHAIN_ID,
    VERIFIER_V2_BOOTSTRAP_DIGEST_SIGNATURE,
    ObserverRunError,
    RegtestCertifyError,
    bootstrap_attestation_digest_v2,
    bootstrap_attestation_message_v2,
    call_bytes32,
    call_uint,
    encode_verifier_v2_initialize,
    encode_verifier_v2_initialize_kofn,
    fb_v2_abi_words,
    hx,
    predict_create_address,
    rpc_refusal,
    run_observer_once,
    selector,
    tamper_observation_verdict,
    to_0x,
)
from test_framework.util import assert_equal, assert_greater_than

# The stale drill: the B3 clock jumps past profile R's observation age.
STALE_JUMP_SECONDS = 5_000
assert STALE_JUMP_SECONDS > GATE_R_OBSERVATION_MAX_AGE
DRILL_BURN_RAW = 1_000_000
BOOTSTRAP_SIGNERS = [0, 1, 2]
# A CREATE address of the same deployer after DeployV2's four contracts and
# the reserved forwarder-factory slot: "another verifier" for the replay case.
OTHER_VERIFIER_NONCE = 5


def bridge_v2_vectors():
    with open(SRCDIR / "contracts" / "test" / "vectors" / "bridge_v2_vectors.json", encoding="utf8") as f:
        return json.load(f)


class B3BridgeRT4KofNGateTest(B3BridgeRT4aRoundTripTest):
    BOOTSTRAP_MODE = 0  # K_OF_N, the Verifier V2 default

    def set_test_params(self):
        super().set_test_params()

    def skip_test_if_missing_module(self):
        super().skip_test_if_missing_module()
        self.skip_if_no_bridge_tools(("b3-bridge-ethcheck", "b3-bridge-regtest-beacon", REGTEST_CERTIFY_TOOL))

    # -- bootstrap committee (before any node starts) ---------------------------
    def setup_network(self):
        self.log.info("b3-bridge-regtest-certify: the K_OF_N bootstrap committee of public test keys")
        bootstrap_set = self.bootstrap_set = self.regtest_certify("bootstrap-set")
        header = bootstrap_set["signing_set"]
        assert_equal((header["epoch"], header["ruleset_version"], header["validator_count"],
                      header["total_weight"], header["quorum_weight"]), (0, 2, 4, 4, 3))
        env = bootstrap_set["env"]
        assert_equal(env["B3V2_BOOTSTRAP_MODE"], 0)
        assert_equal(hx(env["B3V2_BOOTSTRAP_SET_HASH"]), hx(header["set_hash"]))
        assert_equal((hx(env["B3V2_BOOTSTRAP_AGGREGATE_PUBKEY"]), hx(env["B3V2_BOOTSTRAP_MEMBERS_ROOT"])),
                     (hx(header["aggregate_pubkey"]), hx(header["members_root"])))
        assert_equal([(m["index"], m["weight"]) for m in bootstrap_set["members"]], [(i, 1) for i in range(4)])
        self.evidence.step("bootstrap committee from b3-bridge-regtest-certify", signing_set=header,
                           members=[m["pubkey"] for m in bootstrap_set["members"]], test_keys_only=True)
        super().setup_network()

    # -- 3. DeployV2 with K_OF_N -------------------------------------------------
    def bootstrap_deploy_env(self):
        return dict(self.bootstrap_set["env"])

    def deploy_v2_stack(self):
        super().deploy_v2_stack()
        anvil, verifier = self.anvil, self.verifier
        observed = {
            "BOOTSTRAP_MODE": call_uint(anvil, verifier, "BOOTSTRAP_MODE()"),
            "BOOTSTRAP_SET_HASH": to_0x(call_bytes32(anvil, verifier, "BOOTSTRAP_SET_HASH()")),
            "MAX_EPOCH_LAG": call_uint(anvil, verifier, "MAX_EPOCH_LAG()"),
            "BRIDGE_ACTIVATION_HEIGHT": call_uint(anvil, verifier, "BRIDGE_ACTIVATION_HEIGHT()"),
        }
        expected = {"BOOTSTRAP_MODE": 0, "BOOTSTRAP_SET_HASH": to_0x(hx(self.bootstrap_set["signing_set"]["set_hash"])),
                    "MAX_EPOCH_LAG": GATE_R_SET_EXPIRY, "BRIDGE_ACTIVATION_HEIGHT": BRIDGE_V2_HEIGHT}
        assert_equal(observed, expected)
        self.evidence.assertion("Verifier V2 deployed for K_OF_N with profile R windows (OI-3 harness check)",
                                expected=expected, actual=observed)

    # -- 4. K_OF_N initialize and the OI-12 negatives ----------------------------
    def initialize_transaction(self, cert, fb):
        self.log.info("(4) K_OF_N: the tool's OI-12-bound 3-of-4 attestation; replays and wrong proofs revert")
        anvil, verifier = self.anvil, self.verifier
        start_set, successor = cert["signing_set"], cert["successor_set"]
        leaf0_path = cert["history"]["leaf0_path"]
        start_set_hash = call_bytes32(anvil, verifier, "START_SET_HASH()")
        assert_equal(hx(start_set["set_hash"]), start_set_hash)
        assert_equal(hx(self.lineage["genesis_set_hash"]), start_set_hash)
        bootstrap_hash = hx(self.bootstrap_set["signing_set"]["set_hash"])
        request = {"chain_domain": to_0x(self.domain), "evm_chain_id": REGTEST_ETH_CHAIN_ID, "verifier": verifier,
                   "start_set_hash": to_0x(start_set_hash), "fb_encoded": to_0x(fb["encoded"])}
        attestation = self.regtest_certify("bootstrap", request)
        assert_equal((attestation["mode"], attestation["signers"], attestation["absent_indices"]),
                     ("K_OF_N", BOOTSTRAP_SIGNERS, [3]))
        assert_equal(hx(attestation["signing_set"]["set_hash"]), bootstrap_hash)
        message = bootstrap_attestation_message_v2(self.domain, REGTEST_ETH_CHAIN_ID, verifier, start_set_hash, fb)
        digest = bootstrap_attestation_digest_v2(self.domain, REGTEST_ETH_CHAIN_ID, verifier, start_set_hash, fb)
        view = anvil.call(verifier, selector(VERIFIER_V2_BOOTSTRAP_DIGEST_SIGNATURE) + fb_v2_abi_words(fb))
        assert_equal((hx(attestation["message"]), hx(attestation["digest"]), view), (message, digest, digest))

        def kofn(proof):
            return encode_verifier_v2_initialize_kofn(fb, attestation["signing_set"], start_set, successor,
                                                      leaf0_path, proof)

        other_verifier = predict_create_address(self.deployer, OTHER_VERIFIER_NONCE)
        replayed = self.regtest_certify("bootstrap", dict(request, verifier=other_verifier))
        assert hx(replayed["digest"]) != digest
        plain = self.regtest_certify("certify", {
            "chain_domain": to_0x(self.domain), "fb_encoded": to_0x(fb["encoded"]), "label": "bootstrap",
            "weights": [1, 1, 1, 1], "signers": BOOTSTRAP_SIGNERS, "expected_set_hash": to_0x(bootstrap_hash)})
        assert_equal(hx(plain["signing_set"]["set_hash"]), bootstrap_hash)
        assert_equal(hx(plain["cert_id"]), hx(cert["cert_id"]))
        negatives = {
            "attestation made for another verifier address": (kofn(replayed["proof"]), "ProofRejected"),
            "digest V2 signed by the bootstrap keys": (kofn(plain["proof"]), "ProofRejected"),
            "Set'_0's own certificate proof under the bootstrap header": (kofn(cert["proof"]), "ProofRejected"),
            "START_CERTIFICATE calldata": (encode_verifier_v2_initialize(cert), "BadSigningSet"),
        }
        reverted = {}
        for name, (calldata, error) in negatives.items():
            assert_equal(revert_data(anvil, verifier, calldata, self.relayer), error_selector(error))
            reverted[name] = error
        mainnet = next(v for v in bridge_v2_vectors()["bootstrap_attestation_v2"] if v["name"] == "mainnet")
        refusals = {}
        for name, change, expected in (
                ("B3 mainnet chain domain", {"chain_domain": mainnet["chain_domain"]},
                 "refusing the B3 mainnet chain domain"),
                ("Ethereum mainnet chain id", {"evm_chain_id": 1}, "refusing EVM chain id 1"),
                ("2 of 4 signers", {"signers": [0, 1]}, "needs at least 3 of the 4 bootstrap signers")):
            try:
                self.regtest_certify("bootstrap", dict(request, **change))
            except RegtestCertifyError as e:
                assert expected in e.message, e.message
                refusals[name] = e.message
            else:
                raise AssertionError(f"the tool signed with {name}")
        assert_equal(call_uint(anvil, verifier, "status()"), 0)
        self.evidence.assertion("OI-12: bootstrap attestations bound to this verifier, chain and start set",
                                expected={"digest": to_0x(digest), "reverts": {n: e for n, (_, e) in negatives.items()}},
                                actual={"bootstrapDigestOf": to_0x(view), "tool_digest": attestation["digest"],
                                        "reverts": reverted, "tool_refusals": refusals},
                                replay_verifier=other_verifier, replay_digest=replayed["digest"],
                                message=to_0x(message))
        return (kofn(attestation["proof"]), "B3FinalityVerifierV2.initialize (K_OF_N)",
                {"mode": "K_OF_N", "signers": attestation["signers"], "absent": attestation["absent_indices"],
                 "bootstrap_digest": to_0x(digest)})

    # -- 8. the gate's refusals, then the observations -------------------------
    def prepare_burn_gate(self):
        self.log.info("(8) Without kind 9 observations the full gate refuses every burn at row E1")
        n0, anvil = self.nodes[0], self.anvil
        self.shuttle_fresh_until_lag(BURN_FINALITY_LAG_MARGIN)
        self.relay_certificates("before the observations", require_new=False)
        info = self.v2_infos()
        gate = info["burn_gate"]
        assert_equal((gate["status"], gate["row"], gate["reason"]),
                     ("NOT_READY", "E1", "no VERIFIER_E observation"))
        assert "temporary" not in gate
        assert_equal(info["observations"], {})
        refusals = {"bridgewithdraw without observations": rpc_refusal(
            RPC_VERIFY_REJECTED, "bridge V2 burn gate is not ready: E1: no VERIFIER_E observation",
            n0.bridgewithdraw, BURN_RAW, ETH_RECIPIENT)}
        self.evidence.assertion("burn gate NOT_READY at E1 without observations, on every node",
                                expected="E1: no VERIFIER_E observation", actual=gate, refusals=refusals)

        self.log.info("Light client at the anvil head; a tampered verdict is refused, then all three subjects")
        light_client = self.update_light_client_to_head()
        dry = run_observer_once(n0, None, anvil.url, (1, 2, 3), dry_run=True)
        verifier_record = dry["observations"][0]
        assert_equal((verifier_record["name"], verifier_record["verdict"]), ("VERIFIER_E", 0))
        refusals = {"tampered VERIFIER_E verdict": rpc_refusal(
            RPC_VERIFY_REJECTED, "bridge observation verdict does not match B3's lineage",
            n0.submitbridgecarrier, tamper_observation_verdict(verifier_record["payload_hex"]))}
        output = self.submit_observations_v2(subjects=(1, 2, 3), label="kind 9 observations mined (before the burn)")
        # buildbridgeobservationv2 is deterministic for one tip and anchor.
        assert_equal([o["payload_hex"] for o in output["observations"]],
                     [o["payload_hex"] for o in dry["observations"]])
        self.check_observations(output, light_client)
        refusals["repeated VERIFIER_E anchor"] = rpc_refusal(
            RPC_VERIFY_REJECTED, "bridge observation anchor is not newer than the subject's latest observation",
            n0.submitbridgecarrier, verifier_record["payload_hex"])
        try:
            run_observer_once(n0, None, anvil.url, (1,), dry_run=True)
        except ObserverRunError as e:
            assert "the plan's anchor is not newer than its latest observation" in e.message, e.message
            refusals["observer with a used anchor"] = e.message
        else:
            raise AssertionError("the observer built a record for a used anchor")
        self.evidence.assertion("kind 9 refusals: tampered verdict, repeated anchor", actual=refusals,
                                payload_bytes={o["name"]: o["payload_bytes"] for o in output["observations"]})
        self.shuttle_fresh_until_lag(BURN_FINALITY_LAG_MARGIN)
        return self.check_gate_ready("after the observations")

    # -- 13. the stale drill ----------------------------------------------------
    def run_test(self):
        super().run_test()
        self.stale_drill()
        self.evidence_step("RT4 complete", self.anvil, fresh_votes=dict(self.fresh_vote_results),
                           relayed=len(self.relayed))

    def gate_on_every_node(self):
        info = self.v2_infos()
        return info, info["burn_gate"]

    def stale_drill(self):
        self.log.info("(13) Stale drill: the gate reopens on current observations")
        n0, anvil = self.nodes[0], self.anvil
        self.shuttle_fresh_until_lag(BURN_FINALITY_LAG_MARGIN)
        self.observe_ethereum_state("stale drill")
        self.shuttle_fresh_until_lag(BURN_FINALITY_LAG_MARGIN)
        before = self.check_gate_ready("stale drill, before the clock jump")
        prebuilt = n0.bridgewithdraw(DRILL_BURN_RAW, ETH_RECIPIENT, {"broadcast": False})
        assert_equal(prebuilt["broadcast"], False)
        assert prebuilt["txid"] not in n0.getrawmempool()
        # Keep its native fee inputs for the end of the drill: the carriers
        # below must not spend them. Its bUSD V2 input stays unlocked, so
        # the wallet can still try (and be refused) a burn of its own.
        bridged = {(utxo["txid"], utxo["vout"]) for utxo in self.wallet_asset(n0, self.v2info["asset_id"])["utxos"]}
        inputs = [(vin["txid"], vin["vout"]) for vin in n0.decoderawtransaction(prebuilt["hex"])["vin"]]
        assert any(outpoint in bridged for outpoint in inputs)
        native = [{"txid": txid, "vout": vout} for txid, vout in inputs if (txid, vout) not in bridged]
        assert native and n0.lockunspent(False, native)
        accepted = n0.testmempoolaccept([prebuilt["hex"]])[0]
        assert_equal(accepted["allowed"], True)

        self.log.info(f"B3 mock time jumps {STALE_JUMP_SECONDS} s: every observation is stale (row E1)")
        tip_time = n0.getblockheader(n0.getbestblockhash())["time"]
        oldest = min(record["t_obs"] for record in before["observations"].values())
        jump_to = max(int(time.time()), tip_time) + STALE_JUMP_SECONDS
        self.set_chain_time(jump_to)
        self.evidence.jump("b3 mocktime", jump_to - tip_time, before=tip_time, after=jump_to,
                           oldest_observation=oldest, max_age=GATE_R_OBSERVATION_MAX_AGE)
        assert_greater_than(jump_to - oldest, GATE_R_OBSERVATION_MAX_AGE)
        stale = {}
        for label in ("at the jump", "after a block staked at the jumped time"):
            if label != "at the jump":
                self.mine_pos_blocks_mocktime(1, allow_overshoot=True)
            info, gate = self.gate_on_every_node()
            assert_equal((gate["status"], gate["row"], gate["reason"]),
                         ("NOT_READY", "E1", "VERIFIER_E observation is stale"))
            assert_greater_than(gate["now_eff"] - info["observations"]["verifier_e"]["t_obs"],
                                GATE_R_OBSERVATION_MAX_AGE)
            refused = n0.testmempoolaccept([prebuilt["hex"]])[0]
            assert_equal((refused["allowed"], refused["reject-reason"]), (False, "bridge-v2-burn-not-ready"))
            stale[label] = {
                "gate": gate, "tip": n0.getblockcount(),
                "testmempoolaccept": refused,
                "bridgewithdraw": rpc_refusal(RPC_VERIFY_REJECTED,
                                              "bridge V2 burn gate is not ready: E1: VERIFIER_E observation is stale",
                                              n0.bridgewithdraw, DRILL_BURN_RAW, ETH_RECIPIENT),
            }
        self.evidence.assertion("stale observations close the gate (E1) in wallet and mempool, on every node",
                                expected="bridge-v2-burn-not-ready", actual=stale, prebuilt_txid=prebuilt["txid"])

        self.log.info("anvil's clock jumps as far; new observations under mock-time production reopen the gate")
        anvil_before = int(anvil.block()["timestamp"], 16)
        anvil.increase_time(STALE_JUMP_SECONDS)
        anvil.mine(1)
        anvil_after = int(anvil.block()["timestamp"], 16)
        self.evidence.jump("anvil", anvil_after - anvil_before, before=anvil_before, after=anvil_after)
        self.observe_ethereum_state("after the clock jumps")
        self.shuttle_fresh_until_lag(BURN_FINALITY_LAG_MARGIN)
        after = self.check_gate_ready("after the clock jumps")
        reaccepted = n0.testmempoolaccept([prebuilt["hex"]])[0]
        assert_equal(reaccepted["allowed"], True)
        self.evidence.assertion("the gate reopens on current observations; the prebuilt burn is acceptable again",
                                expected=True, actual=reaccepted, burn_gate=after["burn_gate"],
                                observations={key: record["t_obs"] for key, record in after["observations"].items()})


if __name__ == '__main__':
    B3BridgeRT4KofNGateTest(__file__).main()
