#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""RT4c: a finality conflict closes bridge V2 for good (node changes rev 11 §8.5).

REGTEST ONLY, the environment of RT4a (feature_b3_bridge_rt4a_round_trip.py):
four B3 regtest validators linked only to each other, one anvil on
127.0.0.1 (chain 31337, never forked), the synthetic sync-committee tool,
forge --offline with its output outside the source tree, fresh datadirs. The
validators bind the public deterministic test keys of label rt4c-committee
from the REGTEST ONLY b3-bridge-regtest-certify tool (importfinalitykey
before bindfinalitykey), so the tool can sign as B3's own fresh committees.
The tool refuses every chain domain but the -b3modernregtest one and never
reads a key from a node. Fresh votes gossip over the P2P message finsigv2 as
in RT4a; no phase partitions the validators, so no fresh vote is submitted
by RPC.

1. Bring-up with the test keys: getfinalityset's members are the tool's
   keys, in order; their weights are the committee weights the tool signs
   with.
2. RT4a steps 1-10 (START_CERTIFICATE). The deposit is made only after the
   relayer guard (contrib/b3bridge/b3_bridge_deposit_guard_v2.py) reports
   OPEN on every node and getbridgedepositrecipient returns
   recipient_v1(address). Leaf L1 is then under an accepted root but not
   released.
3. The gate is READY (fresh finality within the C3 margin, new kind 9
   observations); a 1,000,000 burn is prebuilt without broadcasting it.
4. Frozen: B3's fresh epoch e and Set'_{e-1}, Set'_e, Set'_{e+1}; Verifier
   V2's latest checkpoint k at H_k with B3's block hash, and its history
   root (re-derived in Python).
5. kind 13 negatives refused by submitbridgecarrier: B3's own epoch-e
   statement re-signed by Set'_e ("equals B3's canonical statement"), a
   Set'_{e-1} statement ("not signed by the lineage's current or next
   committee") and a rogue committee ("does not verify").
6. The conflicting certificate: index k, height H_k, another block hash,
   epoch e+1, signed by Set'_{e+1} (the tool's committee equals B3's set
   field for field); its kind 13 record equals the Python encoding and is
   mined at c.
7. Every node: the sticky conflict (kind13, c, the evidence certId, k,
   e+1), the gate NOT_READY at C4, deposits CLOSED_FOR_GOOD. bridgewithdraw,
   the prebuilt burn (bridge-v2-burn-not-ready), getbridgedepositrecipient
   and the relayer guard are refused; a second kind 13 is refused
   ("already set").
8. Carried certificates for checkpoints below c keep bridgeFlags 0, those at
   or above c carry bit 0 (byte 184 = 0x01, certId = keccak(encoded)).
   Relayed, Verifier V2 closes deposits for good: depositsClosed,
   depositsOpen() = 0, depositGateOpen() = 0, deposit reverts
   DepositClosed() and DepositsClosedForGood is emitted once; a later
   certificate leaves deposits closed.
9. reportConflict, earlier-index form, with the same certificate: the
   canonical block hash is NoConflict; eth_call, then the transaction
   (ConflictReported, form 3), halts Verifier V2 (status 2) and
   submitCertificate reverts NotLive().
10. New observations show the verifier halted: burns stay refused (C1); new
    B3 certificates (bit 0) cannot reach the halted verifier.
11. L1 is still released, exactly once (C-1).
12. n3 restarts (replay) and n0 reindexes: getbridgev2info (burn_gate.now_eff
    aside) and the fresh lineage agree on every node; the conflict is kept.

Evidence: bridge_evidence.json.
"""

import time

from feature_b3_bridge_rt4a_round_trip import (
    BRIDGE_V2_HEIGHT,
    BRING_UP_HEIGHT,
    BURN_FINALITY_LAG_MARGIN,
    DEPOSIT_RAW,
    ETH_RECIPIENT,
    FRESH_ACTIVATION,
    FRESH_GENESIS,
    INIT_CHECKPOINT,
    LINEAGE_VIEW_KEYS,
    MAX_DEPOSIT_RAW,
    RPC_VERIFY_REJECTED,
    B3BridgeRT4aRoundTripTest,
    error_selector,
    raw_block_hash,
    revert_data,
)
from test_framework.b3bridge import (
    CHECKPOINT_INTERVAL,
    ETH_OBSERVATION_KEYS,
    FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED,
    REGTEST_CERTIFY_TOOL,
    REGTEST_ETH_CHAIN_ID,
    VALIDATOR_COUNT,
    VERIFIER_V2_CONFLICT_EARLIER_INDEX,
    VERIFIER_V2_REPORT_CONFLICT_SIGNATURE,
    VERIFIER_V2_STATUS_HALTED_CONFLICT,
    VERIFIER_V2_STATUS_LIVE,
    DepositGuardError,
    abi_call,
    call_bytes32,
    call_uint,
    enc_address,
    enc_uint,
    encode_certificate_payload_v2,
    encode_finality_conflict_record,
    encode_finalized_block_v2,
    encode_report_conflict,
    finality_digest_v2,
    fold_withdrawal_path,
    gate_view,
    history_leaf_v2,
    hx,
    keccak256,
    recipient_v1,
    rpc_refusal,
    run_deposit_guard,
    selector,
    to_0x,
    withdrawal_root,
    withdrawal_root_and_path,
)
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)

COMMITTEE_LABEL = "rt4c-committee"
ROGUE_LABEL = "rt4c-rogue"
DEPOSIT_RECEIVE_LABEL = "b3-busd-bridge-receive"
DRILL_BURN_RAW = 1_000_000
# The conflicting statement names another block at Verifier V2's checkpoint.
OTHER_BRANCH_HASH = keccak256(b"B3-REGTEST-RT4C/other-branch")
SECOND_BRANCH_HASH = keccak256(b"B3-REGTEST-RT4C/other-branch-2")
# Neither B3 nor reportConflict checks the evidence's successor; non-zero.
EVIDENCE_SUCCESSOR = keccak256(b"B3-REGTEST-RT4C/successor")

C4_CONFLICT_REASON = "sticky conflict: finality conflict evidence"
C1_HALTED_REASON = "verifier is halted by a conflict"
CONFLICT_REPORTED_EVENT = "ConflictReported(bytes32,uint64,uint8,address)"
DEPOSITS_CLOSED_EVENT = "DepositsClosedForGood(bytes32,uint64)"


class B3BridgeRT4cConflictTest(B3BridgeRT4aRoundTripTest):
    BOOTSTRAP_MODE = 1  # START_CERTIFICATE, as RT4a

    def set_test_params(self):
        # Required: the framework's metaclass refuses a subclass that does not
        # define set_test_params itself. RT4a's parameters are RT4c's.
        super().set_test_params()

    def skip_test_if_missing_module(self):
        super().skip_test_if_missing_module()
        self.skip_if_no_bridge_tools(("b3-bridge-ethcheck", "b3-bridge-regtest-beacon", REGTEST_CERTIFY_TOOL))

    def run_test(self):
        self.withdrawal_leaves = []  # (b3 height, leaf V2) in id order
        self.relayed = []
        self.conflict_height = None

        self.log.info(f"(1) Corridor and stakes bound to the {COMMITTEE_LABEL} test keys; live V1 finality")
        self.bring_up_validators(BRING_UP_HEIGHT, finality_key_label=COMMITTEE_LABEL)
        self.check_committee()
        self.check_lineage()
        self.v2info = self.check_v2_info_before_activation()

        self.log.info("(2) RT4a: fresh checkpoint 266, DeployV2, initialize, past V, deposit, mint, burn, root")
        self.resume()
        self.shuttle_until(lambda: all(self.fresh_finalized_height(node) >= INIT_CHECKPOINT
                                       for node in self.bridge_validators()))
        self.freeze()
        self.evidence_step("fresh checkpoint 266 finalized by gossip", self.anvil,
                           finsigv2_bytes_received=self.assert_fresh_gossip(self.bridge_validators()))
        self.deploy_v2_stack()
        self.initialize_verifier()
        self.resume()
        self.shuttle_until(lambda: all(self.fresh_finalized_height(node) >= BRIDGE_V2_HEIGHT
                                       for node in self.bridge_validators()))
        self.freeze()
        self.check_carried_roots()
        self.relay_certificates("past V")
        self.check_deposits_open()
        deposit = self.make_deposit()
        plan = self.build_plan(deposit)
        self.relay_carriers(plan, deposit)
        self.claim_mint(plan, deposit)
        self.burn()
        covering = self.relay_withdrawal_root()
        proof = self.check_withdrawal_proof(covering)
        assert_equal(call_uint(self.anvil, self.vault, "released(uint64)", enc_uint(0)), 0)
        self.evidence_step("leaf L1 under an accepted root, not released", self.anvil,
                           covering=covering, leaf=to_0x(self.leaf), root=to_0x(self.root))

        prebuilt = self.prebuild_burn()
        frozen = self.read_conflict_inputs()
        self.refuse_bad_evidence(frozen)
        conflict = self.submit_conflict(frozen)
        self.check_closed_for_good(frozen, conflict, prebuilt)
        self.relay_flagged_certificates(conflict)
        self.report_conflict(frozen, conflict)
        self.burns_stay_refused(conflict, prebuilt)
        self.release(proof)
        gossip = self.assert_fresh_gossip(self.bridge_validators())
        self.check_replay_and_reindex(conflict)
        self.evidence_step("RT4c complete", self.anvil, fresh_votes=dict(self.fresh_vote_results),
                           fresh_vote_transport=self.fresh_vote_transport, finsigv2_bytes_received=gossip,
                           relayed=len(self.relayed), conflict_height=conflict["height"])

    # -- 1. the test committee ---------------------------------------------------
    def check_committee(self):
        self.log.info("B3's finality set is the tool's test committee, member for member")
        self.freeze()
        members = self.finality_test_keys["members"]
        assert_equal(self.finality_test_keys["label"], COMMITTEE_LABEL)
        # The public half only: no secret leaves the tool here.
        tool_keys = self.regtest_certify("keys", args=("--label", COMMITTEE_LABEL,
                                                       "--count", str(VALIDATOR_COUNT)))["keys"]
        assert all("blssecret" not in key for key in tool_keys)
        sets = [node.getfinalityset() for node in self.bridge_validators()]
        for info in sets[1:]:
            assert_equal(info, sets[0])
        listed = sets[0]["members"]
        assert_equal(sets[0]["validator_count"], VALIDATOR_COUNT)
        assert_equal([m["index"] for m in listed], list(range(VALIDATOR_COUNT)))
        assert_equal([hx(m["bls_pubkey"]) for m in listed], [hx(key["pubkey"]) for key in tool_keys])
        assert_equal([m["validator_key"] for m in listed], [m["validator_key"] for m in members])
        # Members are ordered by ascending validator_key bytes.
        assert_equal([m["validator_key"] for m in listed],
                     sorted((m["validator_key"] for m in listed), key=bytes.fromhex))
        self.committee_weights = [m["weight"] for m in listed]
        assert all(weight > 0 for weight in self.committee_weights)
        assert_equal(sum(self.committee_weights), sets[0]["total_weight"])
        self.evidence.assertion("B3's finality set is the tool's test committee",
                                expected=[key["pubkey"] for key in tool_keys],
                                actual=[m["bls_pubkey"] for m in listed], label=COMMITTEE_LABEL,
                                weights=self.committee_weights, set_hash=sets[0]["set_hash"],
                                members=members, test_keys_only=True)

    # -- 2. RT4a with the deposit instructions -----------------------------------
    def deploy_v2_stack(self):
        super().deploy_v2_stack()
        verifier_ids = self.bridge_method_ids("B3FinalityVerifierV2.sol", "B3FinalityVerifierV2")
        assert_equal(verifier_ids[VERIFIER_V2_REPORT_CONFLICT_SIGNATURE],
                     selector(VERIFIER_V2_REPORT_CONFLICT_SIGNATURE).hex())
        events = {entry["name"]: f"{entry['name']}({','.join(i['type'] for i in entry['inputs'])})"
                  for entry in self.bridge_artifact("B3FinalityVerifierV2.sol", "B3FinalityVerifierV2")["abi"]
                  if entry["type"] == "event"}
        assert_equal((events["ConflictReported"], events["DepositsClosedForGood"]),
                     (CONFLICT_REPORTED_EVENT, DEPOSITS_CLOSED_EVENT))

    def deposit_recipient(self):
        """Route (a): the relayer guard reports OPEN on every node, then the
        wallet's getbridgedepositrecipient names the RECIPIENT_V1 to deposit to."""
        n0 = self.nodes[0]
        guards = {}
        for node in self.bridge_nodes():
            guards[node.index] = run_deposit_guard(node)
            assert_equal(guards[node.index], {"deposits": "OPEN", "next_height": node.getblockcount() + 1})
        instructions = n0.getbridgedepositrecipient()
        address = instructions["address"]
        assert_equal(n0.getaddressinfo(address)["labels"], [DEPOSIT_RECEIVE_LABEL])
        assert_equal(n0.getbridgedepositrecipient(address), instructions)
        assert_equal(hx(instructions["recipient_v1"]), recipient_v1(address))
        assert_equal((hx(instructions["vault"]), hx(instructions["token"])), (hx(self.vault), hx(self.token)))
        assert_equal({key: instructions[key] for key in ("origin_chain_id", "asset_id", "max_deposit_raw",
                                                         "deposit_signature")},
                     {"origin_chain_id": REGTEST_ETH_CHAIN_ID, "asset_id": self.v2info["asset_id"],
                      "max_deposit_raw": MAX_DEPOSIT_RAW, "deposit_signature": "deposit(uint256,bytes32)"})
        self.evidence.assertion("deposit guard OPEN; getbridgedepositrecipient returns recipient_v1(address)",
                                expected=to_0x(recipient_v1(address)), actual=instructions["recipient_v1"],
                                instructions=instructions, guards=guards)
        return address

    # -- 3. READY and a prebuilt burn -------------------------------------------
    def fresh_lineage_settled(self, max_lag):
        """On every validator: the tip at most max_lag above the fresh finalized
        checkpoint, that checkpoint signed in the current fresh epoch, and no
        rotation at the next block (judges of the next block see epoch e)."""
        for node in self.bridge_validators():
            lineage = node.getfinalitylineage()
            finalized = lineage.get("finalized")
            if not lineage.get("bootstrapped") or not finalized or lineage.get("lineage_broken"):
                return False
            if lineage["tip_height"] - finalized["height"] > max_lag or finalized["epoch"] != lineage["epoch"]:
                return False
            epoch_blocks = lineage["break_height"] - lineage["epoch_starts"][-1] - lineage["max_epoch_extension"]
            if lineage["tip_height"] + 1 >= lineage["epoch_starts"][-1] + epoch_blocks:
                return False
        return True

    def settle_fresh_lineage(self, max_lag, timeout=600):
        """Stake while fresh votes gossip until fresh_lineage_settled(); leave
        the validators frozen."""
        deadline = time.time() + timeout
        while True:
            self.resume()
            self.shuttle_until(lambda: self.fresh_lineage_settled(max_lag), timeout=max(1, deadline - time.time()))
            self.freeze()
            # A block staked after the last check can rotate or raise the lag.
            if self.fresh_lineage_settled(max_lag):
                return
            if time.time() >= deadline:
                raise AssertionError(f"the fresh lineage did not settle within {timeout} s")

    def prebuild_burn(self):
        self.log.info("(3) Gate READY on new observations; a burn is prebuilt without broadcasting it")
        n0 = self.nodes[0]
        self.settle_fresh_lineage(BURN_FINALITY_LAG_MARGIN)
        self.observe_ethereum_state("before the conflict")
        self.settle_fresh_lineage(BURN_FINALITY_LAG_MARGIN)
        info = self.check_gate_ready("before the conflict")
        assert_equal(info["deposits"], {"status": "OPEN"})
        prebuilt = n0.bridgewithdraw(DRILL_BURN_RAW, ETH_RECIPIENT, {"broadcast": False})
        assert_equal(prebuilt["broadcast"], False)
        assert prebuilt["txid"] not in n0.getrawmempool()
        # Keep its native fee inputs for steps 7 and 10: the carriers below
        # must not spend them. Its bUSD V2 input stays unlocked, so the
        # wallet can still try (and be refused) a burn of its own.
        bridged = {(utxo["txid"], utxo["vout"]) for utxo in self.wallet_asset(n0, self.v2info["asset_id"])["utxos"]}
        inputs = [(vin["txid"], vin["vout"]) for vin in n0.decoderawtransaction(prebuilt["hex"])["vin"]]
        assert any(outpoint in bridged for outpoint in inputs)
        native = [{"txid": txid, "vout": vout} for txid, vout in inputs if (txid, vout) not in bridged]
        assert native and n0.lockunspent(False, native)
        accepted = {node.index: node.testmempoolaccept([prebuilt["hex"]])[0] for node in self.bridge_nodes()}
        assert all(result["allowed"] for result in accepted.values())
        self.evidence.assertion("prebuilt burn acceptable while READY", expected=True, actual=accepted,
                                prebuilt_txid=prebuilt["txid"], burn_gate=info["burn_gate"],
                                deposits=info["deposits"])
        return prebuilt

    # -- 4. the frozen inputs ------------------------------------------------------
    def history_leaves(self, node, count):
        """Checkpoint-history leaves 0..count-1 of node's active chain."""
        leaves = []
        for index in range(count):
            height = FRESH_GENESIS + index * CHECKPOINT_INTERVAL
            leaves.append(history_leaf_v2(height, raw_block_hash(node, height)))
        return leaves

    def read_conflict_inputs(self):
        self.log.info("(4) Frozen: B3's fresh committees and Verifier V2's latest checkpoint k")
        anvil, verifier, n0 = self.anvil, self.verifier, self.nodes[0]
        self.freeze()
        assert self.fresh_lineage_settled(BURN_FINALITY_LAG_MARGIN)
        lineages = [node.getfinalitylineage() for node in self.bridge_validators()]
        for lineage in lineages[1:]:
            for key in LINEAGE_VIEW_KEYS:
                assert_equal(lineage.get(key), lineages[0].get(key))
        lineage = lineages[0]
        epoch = lineage["epoch"]
        assert_greater_than_or_equal(epoch, 1)
        sets = {"previous": lineage["previous_set"], "current": lineage["current_set"], "next": lineage["next_set"]}
        assert_equal([sets[name]["epoch"] for name in ("previous", "current", "next")], [epoch - 1, epoch, epoch + 1])
        for header in sets.values():
            assert_equal((header["validator_count"], header["total_weight"]),
                         (VALIDATOR_COUNT, sum(self.committee_weights)))

        k = call_uint(anvil, verifier, "latestIndex()")
        h_k = call_uint(anvil, verifier, "latestHeight()")
        assert_equal(h_k, FRESH_GENESIS + k * CHECKPOINT_INTERVAL)
        assert_equal(self.relayed[-1]["checkpoint_index"], k)
        assert_greater_than_or_equal(h_k, BRIDGE_V2_HEIGHT)
        canonical_hash = raw_block_hash(n0, h_k)
        assert_equal(call_bytes32(anvil, verifier, "latestBlockHash()"), canonical_hash)
        history_root = call_bytes32(anvil, verifier, "latestHistoryRoot()")
        # Re-derived: the depth-32 history tree over leaves 0..k-1 of B3's chain.
        assert_equal(withdrawal_root(self.history_leaves(n0, k)), history_root)
        root_k = self.expected_withdrawal_root(h_k)
        assert_equal(call_bytes32(anvil, verifier, "latestBridgeRoot()"), root_k)
        verifier_epoch = call_uint(anvil, verifier, "currentEpoch()")
        assert epoch - 2 <= verifier_epoch <= epoch, (verifier_epoch, epoch)
        # B3's own statement of epoch e: the newest carried certificate signed in e.
        own = [carrier for carrier in self.fresh_certificates(n0, FRESH_ACTIVATION) if carrier["fb"]["epoch"] == epoch]
        assert own, f"no carried certificate of fresh epoch {epoch}"
        frozen = {"epoch": epoch, "sets": sets, "k": k, "h_k": h_k, "canonical_hash": canonical_hash,
                  "history_root": history_root, "root_k": root_k, "statement": own[-1]["fb"],
                  "statement_inclusion": own[-1]["inclusion"], "verifier_epoch": verifier_epoch,
                  "tip": n0.getblockcount()}
        self.evidence_step("conflict inputs frozen", anvil, fresh_epoch=epoch,
                           set_hashes={name: header["set_hash"] for name, header in sets.items()},
                           k=k, h_k=h_k, canonical_hash=to_0x(canonical_hash), history_root=to_0x(history_root),
                           withdrawal_root=to_0x(root_k), verifier_epoch=verifier_epoch,
                           own_statement={"inclusion": own[-1]["inclusion"], "height": own[-1]["fb"]["height"],
                                          "cert_id": to_0x(keccak256(own[-1]["fb"]["encoded"]))})
        return frozen

    # -- 5-6. kind 13 evidence ---------------------------------------------------
    def conflict_statement(self, frozen, **changes):
        """A FinalizedBlockV2 at Verifier V2's checkpoint k naming another block,
        by default of epoch e+1 (the next committee on B3)."""
        fb = {"lineage_id": hx(self.lineage_id), "height": frozen["h_k"], "block_hash": OTHER_BRANCH_HASH,
              "withdrawal_root": frozen["root_k"], "validator_set_hash": EVIDENCE_SUCCESSOR,
              "epoch": frozen["epoch"] + 1, "checkpoint_index": frozen["k"],
              "checkpoint_history_root": frozen["history_root"], "bridge_flags": 0}
        fb.update(changes)
        fb["encoded"] = encode_finalized_block_v2(fb)
        return fb

    def certify(self, fb, label=COMMITTEE_LABEL, expected_set=None):
        """b3-bridge-regtest-certify certify, cross-checked: the digest, certId,
        payload and kind 13 record equal the Python encodings; with
        expected_set the tool's committee is that B3 set, field for field."""
        encoded = encode_finalized_block_v2(fb)
        request = {"chain_domain": to_0x(self.domain), "fb_encoded": to_0x(encoded), "label": label,
                   "weights": self.committee_weights}
        if expected_set is not None:
            request["expected_set_hash"] = expected_set["set_hash"]
        out = self.regtest_certify("certify", request)
        assert_equal(hx(out["fb"]["encoded"]), encoded)
        assert_equal(hx(out["digest_v2"]), finality_digest_v2(self.domain, encoded))
        assert_equal(hx(out["cert_id"]), keccak256(encoded))
        assert_equal(out["expected_set_hash_checked"], expected_set is not None)
        if expected_set is not None:
            assert_equal(out["signing_set"], expected_set)
        payload = encode_certificate_payload_v2(encoded, out["signer_bitmap"], out["aggregate_signature"])
        assert_equal(hx(out["payload"]), payload)
        record = encode_finality_conflict_record(out["signing_set"]["validator_count"], payload)
        assert_equal(hx(out["finality_conflict_record"]), record)
        out["record"] = record
        return out

    def refuse_bad_evidence(self, frozen):
        self.log.info("(5) kind 13 negatives: B3's own statement, a rotated-out committee, a rogue committee")
        n0, epoch = self.nodes[0], frozen["epoch"]
        statement = frozen["statement"]
        resigned = self.certify(statement, expected_set=frozen["sets"]["current"])
        assert_equal(hx(resigned["cert_id"]), keccak256(statement["encoded"]))
        old = self.certify(self.conflict_statement(frozen, epoch=epoch - 1), expected_set=frozen["sets"]["previous"])
        rogue = self.certify(self.conflict_statement(frozen, epoch=epoch), label=ROGUE_LABEL)
        assert hx(rogue["signing_set"]["set_hash"]) != hx(frozen["sets"]["current"]["set_hash"])
        cases = (
            ("B3's own epoch-e statement re-signed by Set'_e", resigned,
             "bridge finality conflict equals B3's canonical statement"),
            ("a statement of Set'_{e-1}", old,
             "bridge finality conflict is not signed by the lineage's current or next committee"),
            ("a rogue committee (not B3's keys)", rogue, "bridge finality conflict certificate does not verify"),
        )
        refusals = {}
        for name, certificate, message in cases:
            refusals[name] = rpc_refusal(RPC_VERIFY_REJECTED, message, n0.submitbridgecarrier,
                                         certificate["record"].hex())
        assert_equal(n0.getblockcount(), frozen["tip"])
        assert "conflict" not in self.v2_infos()
        self.evidence.assertion("kind 13 refused: canonical, rotated-out and rogue evidence",
                                expected=[message for _, _, message in cases], actual=refusals,
                                cert_ids={name: certificate["cert_id"] for name, certificate, _ in cases})

    def submit_conflict(self, frozen):
        self.log.info("(6) Set'_{e+1} signs another block at Verifier V2's checkpoint k; kind 13 is mined")
        n0 = self.nodes[0]
        fb = self.conflict_statement(frozen)
        certificate = self.certify(fb, expected_set=frozen["sets"]["next"])
        record = certificate["record"]
        assert_equal((record[:4], len(record)), (bytes([13, 0, 0, VALIDATOR_COUNT]), 286))

        def create():
            result = n0.submitbridgecarrier(record.hex())
            assert_equal(result["record_kind"], "finality-conflict")
            return result

        txid, height = self.submit_and_mine("kind 13 FINALITY_CONFLICT mined", create)
        # Every statement B3 signs from this height on carries bridgeFlags bit 0.
        self.conflict_height = height
        cert_id = keccak256(fb["encoded"])
        python_record = encode_finality_conflict_record(VALIDATOR_COUNT, encode_certificate_payload_v2(
            fb, certificate["signer_bitmap"], certificate["aggregate_signature"]))
        assert_equal(record, python_record)
        conflict = {
            "fb": fb, "certificate": certificate, "txid": txid, "height": height, "cert_id": cert_id,
            "expected": {"reason": "kind13", "height": height, "block": n0.getblockhash(height), "anchor_block": 0,
                         "evidence_cert_id_evm": to_0x(cert_id), "evidence_checkpoint_index": frozen["k"],
                         "evidence_epoch": frozen["epoch"] + 1},
            "deposits_reason": f"sticky kind 13 conflict at height {height}",
        }
        self.evidence.assertion("the tool's kind 13 record equals the Python encoding",
                                expected=to_0x(python_record),
                                actual=certificate["finality_conflict_record"], record_bytes=len(record),
                                fb=to_0x(fb["encoded"]), cert_id=to_0x(cert_id),
                                signing_set=certificate["signing_set"], txid=txid, mined_height=height)
        return conflict

    # -- 7. closed for good on B3 --------------------------------------------------
    def check_closed_for_good(self, frozen, conflict, prebuilt):
        self.log.info("(7) Every node: sticky conflict, gate C4, deposits closed for good; wallets and relayers refuse")
        n0 = self.nodes[0]
        # Rows C1-C3 pass again, so C4 is the row that refuses.
        self.shuttle_fresh_until_lag(BURN_FINALITY_LAG_MARGIN)
        info = self.v2_infos()
        assert_equal(info["conflict"], conflict["expected"])
        gate = info["burn_gate"]
        assert_equal((gate["status"], gate["row"], gate["reason"]), ("NOT_READY", "C4", C4_CONFLICT_REASON))
        reason = conflict["deposits_reason"]
        assert_equal(info["deposits"], {"status": "CLOSED_FOR_GOOD", "reason": reason})
        refusals = {"bridgewithdraw": rpc_refusal(
            RPC_VERIFY_REJECTED, f"bridge V2 burn gate is not ready: C4: {C4_CONFLICT_REASON}",
            n0.bridgewithdraw, DRILL_BURN_RAW, ETH_RECIPIENT)}
        for node in self.bridge_nodes():
            refused = node.testmempoolaccept([prebuilt["hex"]])[0]
            assert_equal((refused["allowed"], refused["reject-reason"]), (False, "bridge-v2-burn-not-ready"))
            refusals[f"node{node.index} testmempoolaccept (prebuilt burn)"] = refused["reject-reason"]
            refusals[f"node{node.index} getbridgedepositrecipient"] = rpc_refusal(
                RPC_VERIFY_REJECTED, f"bridge V2 deposits are closed for good: {reason}; a deposit made now mints "
                                     "bUSD V2 that can never be redeemed", node.getbridgedepositrecipient)
            try:
                run_deposit_guard(node)
            except DepositGuardError as e:
                assert_equal(e.message, f"bridge V2 deposits are closed for good: {reason}; "
                                        "this relayer refuses deposits and flushes")
                refusals[f"node{node.index} deposit guard"] = e.message
            else:
                raise AssertionError(f"the deposit guard reported OPEN on node{node.index}")
        second = self.certify(self.conflict_statement(frozen, block_hash=SECOND_BRANCH_HASH),
                              expected_set=frozen["sets"]["next"])
        already = "bridge V2 sticky conflict is already set"
        refusals["a second kind 13"] = rpc_refusal(RPC_VERIFY_REJECTED, already, n0.submitbridgecarrier,
                                                   second["record"].hex())
        refusals["the same kind 13 again"] = rpc_refusal(RPC_VERIFY_REJECTED, already, n0.submitbridgecarrier,
                                                         conflict["certificate"]["record"].hex())
        assert_equal(self.v2_infos()["conflict"], conflict["expected"])
        self.evidence.assertion("B3 closed burns and deposits for good (kind 13), on every node",
                                expected={"conflict": conflict["expected"], "row": "C4",
                                          "deposits": "CLOSED_FOR_GOOD"},
                                actual={"conflict": info["conflict"], "burn_gate": gate, "deposits": info["deposits"]},
                                refusals=refusals)

    # -- 8. bridgeFlags bit 0 closes Verifier V2 deposits ------------------------------
    def deposits_closed_logs(self):
        return self.anvil.rpc("eth_getLogs", {"address": self.verifier, "fromBlock": "0x0", "toBlock": "latest",
                                              "topics": [to_0x(keccak256(DEPOSITS_CLOSED_EVENT.encode()))]})

    def check_verifier_deposits_closed(self, closing):
        anvil, verifier, vault = self.anvil, self.verifier, self.vault
        state = {"depositsClosed": call_uint(anvil, verifier, "depositsClosed()"),
                 "depositsOpen": call_uint(anvil, verifier, "depositsOpen()"),
                 "depositGateOpen": call_uint(anvil, vault, "depositGateOpen()")}
        assert_equal(state, {"depositsClosed": 1, "depositsOpen": 0, "depositGateOpen": 0})
        # The depositor still holds tokens and the approval: only the gate refuses.
        deposit = abi_call("deposit(uint256,bytes32)", enc_uint(DEPOSIT_RAW), recipient_v1(self.recipient))
        assert_equal(revert_data(anvil, vault, deposit, self.depositor), error_selector("DepositClosed"))
        logs = self.deposits_closed_logs()
        assert_equal(len(logs), 1)
        assert_equal([hx(topic) for topic in logs[0]["topics"]],
                     [keccak256(DEPOSITS_CLOSED_EVENT.encode()), hx(closing["cert_id"]), enc_uint(closing["height"])])
        return state

    def relay_flagged_certificates(self, conflict):
        self.log.info("(8) Certificates from c carry bridgeFlags bit 0; relayed, Verifier V2 closes deposits for good")
        anvil, verifier, vault, n0 = self.anvil, self.verifier, self.vault, self.nodes[0]
        c = conflict["height"]
        self.resume()
        self.shuttle_until(lambda: any(carrier["fb"]["height"] >= c for carrier in self.fresh_certificates(n0, c + 1)))
        self.freeze()
        carriers = self.check_carried_roots()
        flagged = [carrier for carrier in carriers if carrier["fb"]["height"] >= c]
        assert flagged and any(carrier["fb"]["height"] < c for carrier in carriers)
        first = flagged[0]
        export = self.export_certificate(first["inclusion"])
        encoded = hx(export["encoded"])
        assert_equal((export["bridge_flags"], encoded[184]), (1, FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED))
        assert_equal(hx(export["cert_id"]), keccak256(encoded))
        # Without the bit the same statement is another certId: B3 signs only the flagged one.
        assert keccak256(encode_finalized_block_v2(dict(first["fb"], bridge_flags=0))) != keccak256(encoded)
        before = {"depositsClosed": call_uint(anvil, verifier, "depositsClosed()"),
                  "depositsOpen": call_uint(anvil, verifier, "depositsOpen()"),
                  "depositGateOpen": call_uint(anvil, vault, "depositGateOpen()")}
        assert_equal(before, {"depositsClosed": 0, "depositsOpen": 1, "depositGateOpen": 1})
        assert_equal(self.deposits_closed_logs(), [])

        relayed = self.relay_certificates("bridge flags bit 0")
        # The first relayed certificate at or above c sets depositsClosed.
        closing = next(entry for entry in relayed if entry["height"] >= c)
        after = self.check_verifier_deposits_closed(closing)

        self.log.info("A later certificate leaves Verifier V2 deposits closed")
        latest_index = call_uint(anvil, verifier, "latestIndex()")
        self.resume()
        self.shuttle_until(lambda: any(carrier["fb"]["checkpoint_index"] > latest_index
                                       for carrier in self.fresh_certificates(n0, self.relayed[-1]["inclusion"] + 1)))
        self.freeze()
        self.check_carried_roots()
        later = self.relay_certificates("after deposits closed")
        self.check_verifier_deposits_closed(closing)
        self.evidence.assertion("bridgeFlags bit 0 from c; Verifier V2 closed deposits for good",
                                expected={"flags_from": c, "verifier": after},
                                actual={"carried": [(carrier["inclusion"], carrier["fb"]["height"],
                                                     carrier["fb"]["bridge_flags"]) for carrier in carriers
                                                    if carrier["inclusion"] > c],
                                        "before_relay": before, "after_relay": after},
                                closing_certificate=closing, flagged_encoded=to_0x(encoded),
                                later_relayed=[entry["checkpoint_index"] for entry in later])

    # -- 9. reportConflict -------------------------------------------------------------
    def report_conflict(self, frozen, conflict):
        self.log.info("(9) reportConflict (earlier-index form) with the same certificate halts Verifier V2")
        anvil, verifier, n0 = self.anvil, self.verifier, self.nodes[0]
        k, h_k, canonical = frozen["k"], frozen["h_k"], frozen["canonical_hash"]
        latest_index = call_uint(anvil, verifier, "latestIndex()")
        assert_greater_than(latest_index, k)
        signing = conflict["certificate"]["signing_set"]
        set_hashes = (call_bytes32(anvil, verifier, "currentSetHash()"), call_bytes32(anvil, verifier, "nextSetHash()"))
        assert hx(signing["set_hash"]) in set_hashes, "Set'_{e+1} is no longer the verifier's current or next set"
        latest = self.relayed[-1]
        assert_equal(latest["checkpoint_index"], latest_index)
        export = self.export_certificate(latest["inclusion"], k)
        path = [hx(sibling) for sibling in export["history"]["ancestry_path"]]
        history_root = call_bytes32(anvil, verifier, "latestHistoryRoot()")
        assert_equal(hx(export["checkpoint_history_root"]), history_root)
        leaf = history_leaf_v2(h_k, canonical)
        assert_equal(fold_withdrawal_path(leaf, k, path), history_root)
        # Python's own history tree over leaves 0..latest-1 places leaf k the same way.
        assert_equal(withdrawal_root_and_path(self.history_leaves(n0, latest_index), k), (history_root, path))
        proof = conflict["certificate"]["proof"]
        calldata = encode_report_conflict(conflict["fb"], signing, h_k, canonical, path, proof)
        # B3's own block hash at k is no conflict (refused before the proof).
        canonical_fb = dict(conflict["fb"], block_hash=canonical)
        no_conflict = revert_data(anvil, verifier, encode_report_conflict(canonical_fb, signing, h_k, canonical, path,
                                                                          proof), self.relayer)
        assert_equal(no_conflict, error_selector("NoConflict"))
        assert_equal(anvil.call(verifier, calldata, sender=self.relayer), b"")
        receipt = self.transact(verifier, calldata, "B3FinalityVerifierV2.reportConflict (earlier index)",
                                checkpoint_index=k, epoch=conflict["fb"]["epoch"])
        logs = [log for log in receipt["logs"] if hx(log["address"]) == hx(verifier)]
        assert_equal(len(logs), 1)
        assert_equal([hx(topic) for topic in logs[0]["topics"]],
                     [keccak256(CONFLICT_REPORTED_EVENT.encode()), conflict["cert_id"], enc_uint(k),
                      enc_address(self.relayer)])
        assert_equal(hx(logs[0]["data"]), enc_uint(VERIFIER_V2_CONFLICT_EARLIER_INDEX))
        assert_equal(call_uint(anvil, verifier, "status()"), VERIFIER_V2_STATUS_HALTED_CONFLICT)
        not_live = {"submitCertificate": revert_data(anvil, verifier, hx(export["calldata"]), self.relayer),
                    "reportConflict again": revert_data(anvil, verifier, calldata, self.relayer)}
        assert_equal(not_live, {name: error_selector("NotLive") for name in not_live})
        assert_equal(call_uint(anvil, verifier, "depositsOpen()"), 0)
        self.evidence.assertion("reportConflict halted Verifier V2 with B3's kind 13 evidence",
                                expected={"status": VERIFIER_V2_STATUS_HALTED_CONFLICT,
                                          "form": VERIFIER_V2_CONFLICT_EARLIER_INDEX},
                                actual={"status": call_uint(anvil, verifier, "status()"),
                                        "form": int.from_bytes(hx(logs[0]["data"]), "big")},
                                cert_id=to_0x(conflict["cert_id"]), leaf_height=h_k, leaf_hash=to_0x(canonical),
                                latest_index=latest_index, no_conflict=to_0x(no_conflict),
                                not_live={name: to_0x(data) for name, data in not_live.items()},
                                tx=receipt["transactionHash"])

    # -- 10. burns stay refused ------------------------------------------------------------
    def burns_stay_refused(self, conflict, prebuilt):
        self.log.info("(10) New observations show Verifier V2 halted: burns stay refused (C1)")
        anvil, verifier, n0 = self.anvil, self.verifier, self.nodes[0]
        light_client = self.update_light_client_to_head(label="V2 light client at the anvil head (verifier halted)")
        output = self.submit_observations_v2(subjects=(1, 2, 3), label="kind 9 observations mined (verifier halted)")
        block = light_client["finalized_block"]
        called = self.anvil_observed_values(block)
        assert_equal(self.proven_observed_values(block), called)
        info = self.v2_infos()
        for observation in output["observations"]:
            record = info["observations"][ETH_OBSERVATION_KEYS[observation["subject"]]]
            assert_equal((record["values"], record["verdict"], observation["verdict"], record["anchor_block"]),
                         (called[observation["subject"]], 0, 0, block))
        observed = called[1]
        assert_equal((observed["status"], observed["status_name"], observed["deposits_closed"]),
                     (VERIFIER_V2_STATUS_HALTED_CONFLICT, "HALTED_CONFLICT", True))
        assert observed["status"] != VERIFIER_V2_STATUS_LIVE
        gate = info["burn_gate"]
        assert_equal((gate["status"], gate["row"], gate["reason"]), ("NOT_READY", "C1", C1_HALTED_REASON))
        assert_equal(info["conflict"], conflict["expected"])
        assert_equal(info["deposits"], {"status": "CLOSED_FOR_GOOD", "reason": conflict["deposits_reason"]})
        refusals = {"bridgewithdraw": rpc_refusal(
            RPC_VERIFY_REJECTED, f"bridge V2 burn gate is not ready: C1: {C1_HALTED_REASON}",
            n0.bridgewithdraw, DRILL_BURN_RAW, ETH_RECIPIENT)}
        for node in self.bridge_nodes():
            refused = node.testmempoolaccept([prebuilt["hex"]])[0]
            assert_equal((refused["allowed"], refused["reject-reason"]), (False, "bridge-v2-burn-not-ready"))
            refusals[f"node{node.index} testmempoolaccept (prebuilt burn)"] = refused["reject-reason"]

        self.log.info("B3 keeps certifying with bridgeFlags bit 0; the halted verifier takes none")
        latest_index = call_uint(anvil, verifier, "latestIndex()")
        self.resume()
        self.shuttle_until(lambda: any(carrier["fb"]["checkpoint_index"] > latest_index
                                       for carrier in self.fresh_certificates(n0, FRESH_ACTIVATION)))
        self.freeze()
        self.check_carried_roots()
        newer = [carrier for carrier in self.fresh_certificates(n0, FRESH_ACTIVATION)
                 if carrier["fb"]["checkpoint_index"] > latest_index]
        export = self.export_certificate(newer[-1]["inclusion"], latest_index)
        assert_equal(export["bridge_flags"], 1)
        refusals["submitCertificate of a newer certificate"] = to_0x(
            revert_data(anvil, verifier, hx(export["calldata"]), self.relayer))
        assert_equal(hx(refusals["submitCertificate of a newer certificate"]), error_selector("NotLive"))
        self.evidence.assertion("burns stay refused once Verifier V2 is halted (C1)",
                                expected=f"C1: {C1_HALTED_REASON}", actual=gate, refusals=refusals,
                                verifier_e=observed, deposits=info["deposits"])

    # -- 12. replay and reindex -------------------------------------------------------------
    def check_replay_and_reindex(self, conflict):
        self.log.info("(12) n3 restarts (replay) and n0 reindexes: the conflict and the lineage are kept")
        n0, n1, n3 = self.nodes[0], self.nodes[1], self.nodes[3]
        self.freeze()
        reference = gate_view(self.v2_infos())
        assert_equal(reference["conflict"], conflict["expected"])
        lineage_reference = n1.getfinalitylineage()
        height, tip = n3.getblockcount(), n3.getbestblockhash()
        self.stop_node(3)
        self.start_node(3)
        assert_equal((n3.getblockcount(), n3.getbestblockhash()), (height, tip))
        self.connect_nodes(3, 2)
        self.sync_bridge_blocks()
        assert_equal(gate_view(n3.getbridgev2info()), reference)

        self.restart_node(0, extra_args=list(n0.extra_args) + ["-reindex"])
        self.wait_until(lambda: n0.getbestblockhash() == n1.getbestblockhash(), timeout=300)
        self.connect_nodes(1, 0)
        n0.syncwithvalidationinterfacequeue()
        info = self.v2_infos()
        assert_equal(gate_view(info), reference)
        assert_equal(info["deposits"], {"status": "CLOSED_FOR_GOOD", "reason": conflict["deposits_reason"]})
        for node in self.bridge_nodes():
            lineage = node.getfinalitylineage()
            for key in LINEAGE_VIEW_KEYS:
                assert_equal(lineage.get(key), lineage_reference.get(key))
        self.evidence_step("n3 replayed and n0 reindexed; the kind 13 conflict is kept", self.anvil,
                           tip=n0.getbestblockhash(), conflict=info["conflict"], burn_gate=info["burn_gate"],
                           deposits=info["deposits"], fresh_finalized=lineage_reference.get("finalized"))


if __name__ == '__main__':
    B3BridgeRT4cConflictTest(__file__).main()
