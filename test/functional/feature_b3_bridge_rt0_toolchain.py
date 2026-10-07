#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""RT0: regtest bridge toolchain smoke (doc/design/b3-bridge-v2-regtest-plan.md §5).

REGTEST ONLY. Every process is local: four B3 regtest validators linked only
to each other and one anvil on 127.0.0.1 (chain 31337, never forked). forge
runs with --offline and writes below <BUILDDIR>/test/forge.

1. The contracts build and their full Foundry suite passes offline; no
   production source imports a regtest mock.
2. anvil enforces Osaka's EIP-7825 per-transaction gas cap.
3. anvil's EIP-2537 G1 precompiles agree with an independent Python G1
   doubling, and its pairing check (which B3FinalityVerifier depends on)
   accepts e(G1,G2)·e(-G1,G2) and e(2·G1,G2)·e(-G1,G2)² and refuses
   e(G1,G2) alone.
4. Every anvil header re-encodes to its hash, and every receipts root
   rebuilds from the block's receipts (the B3 relayer codecs).
5. A historical eth_getProof storage proof verifies against that block's
   state root after the slot has changed.
6. The unchanged production Deploy.s.sol deploys the V1 stack against
   MockTetherToken; the candidate manifest matches the chain.
7. The B3 four-validator fixture brings up live BLS finality and survives a
   one-day clock jump: blocks continue on one tip and every validator's
   finalized height advances under the jumped clock.
"""

import hashlib
import json
import os
import re
import time

from test_framework.anvil import AnvilRPCError, deploy, run_forge
from test_framework.b3bridge import (
    BRIDGE_ACTIVATION_HEIGHT,
    EMPTY_TRIE_ROOT,
    MODERN_START,
    REGTEST_ETH_CHAIN_ID,
    B3BridgeChainMixin,
    Trie,
    abi_call,
    call_address,
    call_bytes32,
    call_uint,
    enc_address,
    enc_uint,
    encode_exec_header,
    encode_receipt,
    hx,
    keccak256,
    mapping_slot,
    rlp,
    regtest_chain_domain,
    to_0x,
    verify_account_storage_proof,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)

# EIP-7825 (Osaka): transactions above 2**24 gas are invalid.
EIP7825_TX_GAS_CAP = 1 << 24

# EIP-2537 precompiles and the BLS12-381 G1 and G2 generators.
G1ADD = "0x" + "00" * 19 + "0b"
G1MSM = "0x" + "00" * 19 + "0c"
PAIRING_CHECK = "0x" + "00" * 19 + "0f"
BLS12_381_P = int(
    "1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab", 16)
G1_X = int(
    "17f1d3a73197d7942695638c4fa9ac0fc3688c4f9774b905a14e3a3f171bac586c55e83ff97a1aeffb3af00adb22c6bb", 16)
G1_Y = int(
    "08b3f481e3aaa0f1a09e30ed741d8ae4fcf5e095d5d00af600db18cb2c04b3edd03cc744a2888ae40caa232946c5e7e1", 16)
# G2 coordinates are Fp2 elements c0 + c1·u (u² = -1); EIP-2537 encodes c0 first.
G2_X = (
    int("024aa2b2f08f0a91260805272dc51051c6e47ad4fa403b02b4510b647ae3d1770bac0326a805bbefd48056c8c121bdb8", 16),
    int("13e02b6052719f607dacd3a088274f65596bd0d09920b61ab5da61bbdc7f5049334cf11213945d57e5ac7d055d042b7e", 16))
G2_Y = (
    int("0ce5d527727d6e118cc9cdc6da2e351aadfd9baa8cbdd3a76d429a695160d12c923ac9cc3baca289e193548608b82801", 16),
    int("0606c4a02ea734cc32acd2b02bc28b99cb3e287e85a763af267492ab572e99ab3f370d275cec1da1aaa9075ff05f79be", 16))
PAIRING_TRUE = (1).to_bytes(32, "big")
PAIRING_FALSE = bytes(32)

# Tether-layout balances mapping (MockTetherToken slot 2).
TETHER_BALANCES_SLOT = 2

# The 3-of-4 bootstrap header of contracts/test/BlsBootstrapVector.t.sol and
# BlsCertificateGasBound.t.sol, whose packed hash is the bootstrap_set_hash
# listed in contracts/test/vectors/README.md.
BOOTSTRAP_AGGREGATE_PUBKEY = bytes.fromhex(
    "afea1502f003a9ab0d8bf89c1826bc78d3e01413f9e78e836b0b99dba05c3ce5d9e81f1478697a4d776aeee83449ec54")
BOOTSTRAP_MEMBERS_ROOT = bytes.fromhex("78621e43cecacca419e7d29cb7458f31262616dce6083cf3f32ddec1b67b5910")
BOOTSTRAP_VALIDATOR_COUNT = 4
BOOTSTRAP_TOTAL_WEIGHT = 4
BOOTSTRAP_QUORUM_WEIGHT = 3

DEPLOY_CAP_RAW = 1_000_000_000
MIN_EPOCH_DURATION = 86_400
MAX_EPOCH_LAG = 259_200
MAX_CERTIFICATE_AGE = 86_400
MIN_DEPOSIT_EXIT_WINDOW = 86_400
BOOTSTRAP_DEADLINE_AHEAD = 3_600

FORGE_TIMEOUT = 1_800

ONE_DAY = 86_400
JUMP_BLOCKS = 10
FINALITY_AFTER_JUMP_TIMEOUT = 180


def fp64(value):
    """EIP-2537 field element: 64 bytes, big-endian, top 16 bytes zero."""
    assert 0 <= value < BLS12_381_P
    return value.to_bytes(64, "big")


def g1_point(x, y):
    return fp64(x) + fp64(y)


def g2_point(x, y):
    return fp64(x[0]) + fp64(x[1]) + fp64(y[0]) + fp64(y[1])


def fp2_mul(a, b):
    p = BLS12_381_P
    return ((a[0] * b[0] - a[1] * b[1]) % p, (a[0] * b[1] + a[1] * b[0]) % p)


def g1_double(x, y):
    """Affine doubling on y^2 = x^3 + 4 over Fp."""
    p = BLS12_381_P
    slope = 3 * x * x * pow(2 * y, -1, p) % p
    x3 = (slope * slope - 2 * x) % p
    return x3, (slope * (x - x3) - y) % p


def bootstrap_set_hash(pubkey, members_root):
    """keccak256 of the packed SetHeader (contracts/src/IB3FinalityProver.sol)."""
    return keccak256(
        (0).to_bytes(8, "big") + (1).to_bytes(2, "big") +
        BOOTSTRAP_VALIDATOR_COUNT.to_bytes(4, "big") +
        BOOTSTRAP_TOTAL_WEIGHT.to_bytes(8, "big") +
        BOOTSTRAP_QUORUM_WEIGHT.to_bytes(8, "big") + pubkey + members_root)


def gas_used(receipt):
    return int(receipt["gasUsed"], 16)


class B3BridgeRT0ToolchainTest(B3BridgeChainMixin, BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_foundry(("anvil", "forge"))

    def setup_network(self):
        self.start_bridge_validators()

    @property
    def contracts_dir(self):
        return os.path.join(self.bridge_srcdir, "contracts")

    def run_test(self):
        self.check_forge_toolchain()
        anvil = self.start_anvil()
        self.accounts = anvil.accounts()
        assert_equal(anvil.chain_id_rpc(), REGTEST_ETH_CHAIN_ID)
        self.evidence_step("anvil started", anvil, url=anvil.url, hardfork=anvil.hardfork)
        self.check_eip7825(anvil)
        self.check_eip2537(anvil)
        token = self.check_headers_and_receipts(anvil)
        self.check_historical_storage_proof(anvil, token)
        self.check_v1_stack_deploy(anvil, token)
        self.check_b3_day_jump(anvil)

    # -- (1) forge ---------------------------------------------------------
    def check_forge_toolchain(self):
        self.log.info("(1) Offline forge build and the full Foundry suite")
        start = time.time()
        # A cold via-IR compile of every contract and test can take minutes.
        self.bridge_forge_build(timeout=FORGE_TIMEOUT)
        result = self.bridge_forge_test(timeout=FORGE_TIMEOUT)
        output = result.stdout + result.stderr
        summaries = re.findall(
            r"(\d+) tests passed, (\d+) failed, (\d+) skipped \((\d+) total tests\)", output)
        assert summaries, "forge test printed no summary line"
        passed, failed, skipped, total = map(int, summaries[-1])
        assert_equal(failed, 0)
        assert_greater_than(passed, 0)
        for suite in ("MockTetherTokenTest", "MockDepositVerifierTest"):
            assert suite in output, f"forge test did not run {suite}"
        self.evidence.assertion("forge test --offline", expected={"failed": 0},
                                actual={"passed": passed, "failed": failed,
                                        "skipped": skipped, "total": total},
                                seconds=round(time.time() - start, 1))

        # Production sources never import a regtest mock, and every mock
        # carries the regtest-only header.
        production = [os.path.join(self.contracts_dir, "B3DepositVault.sol")]
        for subdir in ("src", "script"):
            for root, _, files in os.walk(os.path.join(self.contracts_dir, subdir)):
                production += [os.path.join(root, f) for f in files if f.endswith(".sol")]
        assert_greater_than(len(production), 1)
        referencing = []
        for path in production:
            with open(path, encoding="utf8") as f:
                if "regtest/" in f.read():
                    referencing.append(os.path.relpath(path, self.contracts_dir))
        assert_equal(referencing, [])
        mocks = sorted(f for f in os.listdir(os.path.join(self.contracts_dir, "regtest"))
                       if f.endswith(".sol"))
        assert mocks, "no regtest mocks found"
        for name in mocks:
            with open(os.path.join(self.contracts_dir, "regtest", name), encoding="utf8") as f:
                assert "REGTEST ONLY, never imported by src/ or script/." in f.read(), name
        self.evidence.assertion("no production file references contracts/regtest",
                                expected=[], actual=referencing, production_files=len(production),
                                mocks=mocks)

    # -- (2) EIP-7825 ------------------------------------------------------
    def check_eip7825(self, anvil):
        self.log.info("(2) EIP-7825: the per-transaction gas cap is enforced")
        sender, recipient = self.accounts[0], self.accounts[1]
        nonce_before = int(anvil.rpc("eth_getTransactionCount", sender, "latest"), 16)
        try:
            anvil.send(sender, recipient, gas=EIP7825_TX_GAS_CAP + 1)
        except AnvilRPCError as e:
            refusal = e.message
        else:
            raise AssertionError("anvil accepted a transaction above the EIP-7825 cap")
        assert "gas" in refusal.lower(), refusal
        assert_equal(int(anvil.rpc("eth_getTransactionCount", sender, "latest"), 16), nonce_before)
        receipt = anvil.send(sender, recipient, gas=EIP7825_TX_GAS_CAP)
        assert_equal(int(receipt["status"], 16), 1)
        self.evidence.assertion("EIP-7825 cap", expected=EIP7825_TX_GAS_CAP,
                                actual={"refused": EIP7825_TX_GAS_CAP + 1,
                                        "accepted": EIP7825_TX_GAS_CAP},
                                refusal=refusal)

    # -- (3) EIP-2537 ------------------------------------------------------
    def check_eip2537(self, anvil):
        self.log.info("(3) EIP-2537: G1 add and MSM agree with Python doubling; pairing check")
        p = BLS12_381_P
        assert_equal(G1_Y * G1_Y % p, (G1_X ** 3 + 4) % p)
        generator = g1_point(G1_X, G1_Y)
        doubled = g1_point(*g1_double(G1_X, G1_Y))
        negated = g1_point(G1_X, p - G1_Y)
        g1add = anvil.call(G1ADD, generator + generator)
        g1msm = anvil.call(G1MSM, generator + enc_uint(2))
        g1add_inverse = anvil.call(G1ADD, generator + negated)
        assert_equal(g1add, doubled)
        assert_equal(g1msm, doubled)
        assert_equal(g1add_inverse, bytes(128))

        # y² = x³ + 4(1 + u) on E'(Fp2); anvil checks G2 subgroup membership.
        g2_x3 = fp2_mul(fp2_mul(G2_X, G2_X), G2_X)
        assert_equal(fp2_mul(G2_Y, G2_Y), ((g2_x3[0] + 4) % p, (g2_x3[1] + 4) % p))
        g2 = g2_point(G2_X, G2_Y)
        pairings = {
            "e(G1,G2)·e(-G1,G2)": anvil.call(PAIRING_CHECK, generator + g2 + negated + g2),
            "e(2·G1,G2)·e(-G1,G2)²": anvil.call(PAIRING_CHECK, doubled + g2 + negated + g2 + negated + g2),
            "e(G1,G2)": anvil.call(PAIRING_CHECK, generator + g2),
        }
        expected_pairings = {"e(G1,G2)·e(-G1,G2)": PAIRING_TRUE,
                             "e(2·G1,G2)·e(-G1,G2)²": PAIRING_TRUE,
                             "e(G1,G2)": PAIRING_FALSE}
        assert_equal(pairings, expected_pairings)
        self.evidence.assertion("EIP-2537 G1ADD(G,G) == G1MSM(G,2) == 2G; G1ADD(G,-G) == O; pairing check",
                                expected={"2G": to_0x(doubled), "O": to_0x(bytes(128)),
                                          **{k: to_0x(v) for k, v in expected_pairings.items()}},
                                actual={"G1ADD(G,G)": to_0x(g1add), "G1MSM(G,2)": to_0x(g1msm),
                                        "G1ADD(G,-G)": to_0x(g1add_inverse),
                                        **{k: to_0x(v) for k, v in pairings.items()}})

    # -- (4) headers and receipts -------------------------------------------
    def check_headers_and_receipts(self, anvil):
        self.log.info("(4) MockTetherToken deployed; headers and receipts roots re-encode")
        owner, holder = self.accounts[0], self.accounts[1]
        token, block, code_hash = self.deploy_contract(
            anvil, owner, "MockTetherToken.sol", "MockTetherToken")
        self.token_code_hash = code_hash
        issue = anvil.send(owner, token, abi_call("issue(uint256)", enc_uint(10**12)))
        transfer = anvil.send(owner, token, abi_call("transfer(address,uint256)",
                                                    enc_address(holder), enc_uint(25_000_000)))
        self.evidence.gas("MockTetherToken.issue", gas_used(issue))
        self.evidence.gas("MockTetherToken.transfer", gas_used(transfer))
        assert_equal(call_uint(anvil, token, "decimals()"), 6)
        assert_equal(call_uint(anvil, token, "balanceOf(address)", enc_address(holder)),
                     25_000_000)

        # One block with several receipts, legacy and EIP-1559, all with logs,
        # so the receipts trie has more than one leaf and both encodings.
        transfers = []
        anvil.rpc("evm_setAutomine", False)
        try:
            for i in range(3):
                tx = {"from": owner, "to": token, "gas": hex(100_000),
                      "data": to_0x(abi_call("transfer(address,uint256)",
                                             enc_address(holder), enc_uint(1_000_000 + i)))}
                if i == 1:
                    tx["gasPrice"] = anvil.rpc("eth_gasPrice")
                transfers.append(anvil.rpc("eth_sendTransaction", tx))
            anvil.mine(1)
        finally:
            anvil.rpc("evm_setAutomine", True)
        batch = [anvil.receipt(tx_hash) for tx_hash in transfers]
        assert_equal({receipt["blockNumber"] for receipt in batch}, {batch[0]["blockNumber"]})
        assert_equal(sorted(receipt["type"] for receipt in batch), ["0x0", "0x2", "0x2"])
        assert all(int(receipt["status"], 16) == 1 and receipt["logs"] for receipt in batch)

        head = anvil.block_number()
        with_receipts = 0
        for number in range(head + 1):
            header = anvil.block(number)
            assert_equal(keccak256(encode_exec_header(header)), hx(header["hash"]))
            transactions = header["transactions"]
            if not transactions:
                assert_equal(hx(header["receiptsRoot"]), EMPTY_TRIE_ROOT)
                continue
            block_receipts = anvil.rpc("eth_getBlockReceipts", header["number"])
            assert_equal(len(block_receipts), len(transactions))
            trie = Trie([(rlp(i), encode_receipt(r)) for i, r in enumerate(block_receipts)])
            assert_equal(trie.root(), hx(header["receiptsRoot"]))
            with_receipts += 1
        # Osaka-shaped headers carry the Prague requests hash.
        assert "requestsHash" in anvil.block(head)
        assert_greater_than_or_equal(with_receipts, 5)
        self.evidence_step("headers and receipts re-encoded", anvil, blocks=head + 1,
                           blocks_with_receipts=with_receipts, token=token,
                           token_block=block)
        return token

    # -- (5) historical eth_getProof ------------------------------------------
    def check_historical_storage_proof(self, anvil, token):
        self.log.info("(5) Historical eth_getProof verifies against its own state root")
        owner, holder = self.accounts[0], self.accounts[2]
        slot = mapping_slot(enc_address(holder), TETHER_BALANCES_SLOT)
        old_value, new_value = 7_000_000, 7_000_000 + 11_000_000
        first = anvil.send(owner, token, abi_call("transfer(address,uint256)",
                                                 enc_address(holder), enc_uint(old_value)))
        block_x = int(first["blockNumber"], 16)
        second = anvil.send(owner, token, abi_call("transfer(address,uint256)",
                                                  enc_address(holder), enc_uint(new_value - old_value)))
        block_y = int(second["blockNumber"], 16)
        assert_greater_than(block_y, block_x)
        head = anvil.mine(10)
        assert_greater_than_or_equal(head, block_y + 10)

        root_x = hx(anvil.block(block_x)["stateRoot"])
        root_head = hx(anvil.block(head)["stateRoot"])
        proof_x = anvil.get_proof(token, [slot], block_x)
        proven_x = verify_account_storage_proof(root_x, token, slot, proof_x)
        assert_equal(proven_x["value"], old_value)
        assert_equal(proven_x["code_hash"], self.token_code_hash)
        assert_equal(call_uint(anvil, token, "balanceOf(address)", enc_address(holder),
                               block=block_x), old_value)
        proven_head = verify_account_storage_proof(
            root_head, token, slot, anvil.get_proof(token, [slot], head))
        assert_equal(proven_head["value"], new_value)
        try:
            verify_account_storage_proof(root_head, token, slot, proof_x)
        except ValueError:
            pass
        else:
            raise AssertionError("a block-X proof verified against the head state root")
        self.evidence_step("historical storage proof", anvil, block_x=block_x,
                           block_y=block_y, slot=hex(slot), old_value=old_value,
                           new_value=new_value)

    # -- (6) production V1 deploy script -----------------------------------
    def check_v1_stack_deploy(self, anvil, token):
        self.log.info("(6) Production Deploy.s.sol deploys the V1 stack against the mock")
        deployer = self.accounts[0]
        with open(os.path.join(self.contracts_dir, "test", "vectors", "README.md"),
                  encoding="utf8") as f:
            vector_facts = f.read()
        set_hash = bootstrap_set_hash(BOOTSTRAP_AGGREGATE_PUBKEY, BOOTSTRAP_MEMBERS_ROOT)
        assert f"bootstrap_set_hash={set_hash.hex()}" in vector_facts, set_hash.hex()

        domain = regtest_chain_domain()
        now = int(anvil.block()["timestamp"], 16)
        manifest_path = os.path.join(self.contracts_dir, "deployments",
                                     f"local-rt0-{os.getpid()}.json")
        assert not os.path.exists(manifest_path)
        env = {
            "EXPECTED_DEPLOYER": deployer,
            "EXPECTED_CHAIN_ID": str(REGTEST_ETH_CHAIN_ID),
            "B3_CHAIN_DOMAIN": to_0x(domain),
            "ORIGIN_TOKEN": token,
            "EXPECTED_ORIGIN_TOKEN_CODE_HASH": to_0x(self.token_code_hash),
            "MAX_DEPOSIT_RAW": str(DEPLOY_CAP_RAW),
            "B3_MAX_PER_BLOCK_RAW": str(DEPLOY_CAP_RAW),
            "BOOTSTRAP_AGGREGATE_PUBKEY": to_0x(BOOTSTRAP_AGGREGATE_PUBKEY),
            "BOOTSTRAP_MEMBERS_ROOT": to_0x(BOOTSTRAP_MEMBERS_ROOT),
            "EXPECTED_BOOTSTRAP_SET_HASH": to_0x(set_hash),
            "BOOTSTRAP_MEMBERS_MANIFEST_SHA256": to_0x(
                hashlib.sha256(b"B3-REGTEST-RT0/bootstrap-members-manifest").digest()),
            "SOURCE_BUILD_PROVENANCE_SHA256": to_0x(
                hashlib.sha256(b"B3-REGTEST-RT0/source-build-provenance").digest()),
            "MODERN_START_HEIGHT": str(MODERN_START),
            "BRIDGE_ACTIVATION_HEIGHT": str(BRIDGE_ACTIVATION_HEIGHT),
            "MIN_BRIDGE_VALIDATORS": "4",
            "MAX_BRIDGE_VALIDATORS": "64",
            "MIN_BRIDGE_TOTAL_WEIGHT": "4",
            "MIN_EPOCH_DURATION_SECONDS": str(MIN_EPOCH_DURATION),
            "MAX_EPOCH_LAG_SECONDS": str(MAX_EPOCH_LAG),
            "MAX_CERTIFICATE_AGE_SECONDS": str(MAX_CERTIFICATE_AGE),
            "MIN_DEPOSIT_EXIT_WINDOW_SECONDS": str(MIN_DEPOSIT_EXIT_WINDOW),
            "BOOTSTRAP_DEADLINE_UNIX": str(now + BOOTSTRAP_DEADLINE_AHEAD),
            "CANDIDATE_MANIFEST_PATH": manifest_path,
            # Keep this run's broadcast records with the test's tmpdir.
            "FOUNDRY_BROADCAST": os.path.join(self.options.tmpdir, "forge-broadcast"),
        }
        first_block = anvil.block_number() + 1
        try:
            script = os.path.join(self.contracts_dir, "script", "Deploy.s.sol")
            run_forge(self.foundry["forge"], self.bridge_srcdir, self.bridge_builddir,
                      "script", f"{script}:Deploy", "--rpc-url", anvil.url, "--broadcast",
                      "--unlocked", "--sender", deployer, env=env, timeout=600)
            with open(manifest_path, encoding="utf8") as f:
                manifest = json.load(f)
        finally:
            if os.path.exists(manifest_path):
                os.remove(manifest_path)
        assert os.path.isfile(os.path.join(env["FOUNDRY_BROADCAST"], "Deploy.s.sol",
                                           str(REGTEST_ETH_CHAIN_ID), "run-latest.json"))

        assert_equal(manifest["status"], "candidate_requires_block_finalization")
        assert_equal(manifest["production_approved"], False)
        assert_equal(manifest["ethereum_chain_id"], REGTEST_ETH_CHAIN_ID)
        assert_equal(manifest["deployer"].lower(), deployer)
        assert_equal(hx(manifest["b3_chain_domain"]), domain)
        assert_equal(hx(manifest["bootstrap_set_hash"]), set_hash)
        assert_equal(manifest["origin_token"].lower(), token)
        assert_equal(hx(manifest["origin_token_runtime_code_hash"]), anvil.code_hash(token))
        deployed = {}
        for role in ("prover", "verifier", "vault"):
            address = manifest[role].lower()
            assert_equal(hx(manifest[f"{role}_runtime_code_hash"]), anvil.code_hash(address))
            deployed[address] = role
        vault, verifier = manifest["vault"].lower(), manifest["verifier"].lower()

        assert_equal(call_address(anvil, vault, "ORIGIN_TOKEN()"), token)
        assert_equal(call_uint(anvil, vault, "ORIGIN_CHAIN_ID()"), REGTEST_ETH_CHAIN_ID)
        assert_equal(call_uint(anvil, vault, "BRIDGE_ACTIVATION_HEIGHT()"), BRIDGE_ACTIVATION_HEIGHT)
        assert_equal(call_uint(anvil, vault, "MAX_DEPOSIT_RAW()"), DEPLOY_CAP_RAW)
        assert_equal(call_bytes32(anvil, vault, "B3_CHAIN_DOMAIN()"), domain)
        assert_equal(call_bytes32(anvil, vault, "B3_ASSET_ID()"), hx(manifest["b3_asset_id"]))
        assert_equal(call_address(anvil, vault, "verifier()"), verifier)
        assert_equal(call_bytes32(anvil, verifier, "BOOTSTRAP_SET_HASH()"), set_hash)
        assert_equal(call_uint(anvil, verifier, "MODERN_START_HEIGHT()"), MODERN_START)

        # Indicative deployment gas from the mined creation receipts.
        head = anvil.block_number()
        for number in range(first_block, head + 1):
            for tx_hash in anvil.block(number)["transactions"]:
                receipt = anvil.receipt(tx_hash)
                assert_equal(receipt["from"].lower(), deployer)
                created = (receipt.get("contractAddress") or "").lower()
                if created in deployed:
                    self.evidence.gas(f"Deploy.s.sol {deployed.pop(created)}", gas_used(receipt),
                                      block=number)
        assert_equal(deployed, {})
        self.evidence_step("V1 stack deployed by Deploy.s.sol", anvil, vault=vault,
                           verifier=verifier, prover=manifest["prover"].lower(),
                           b3_asset_id=manifest["b3_asset_id"])

    # -- (7) B3 fixture and a one-day jump ---------------------------------
    def check_b3_day_jump(self, anvil):
        self.log.info("(7) B3: four validators with live BLS finality")
        self.bring_up_validators(MODERN_START + 10)
        self.evidence_step("B3 validators live", anvil,
                           finalized=self.finalized_height(self.nodes[0]))

        self.log.info("(7) B3: a one-day clock jump, then mock-time production")
        self.freeze()
        nodes = self.bridge_nodes()
        height_before = self.nodes[0].getblockcount()
        finalized_before = [self.finalized_height(node) for node in nodes]
        jump_to = int(time.time()) + ONE_DAY
        self.set_chain_time(jump_to)
        # Several validators may be eligible at once after the jump, so one
        # production round can add more than one block; only the floor counts.
        self.mine_pos_blocks_mocktime(JUMP_BLOCKS, allow_overshoot=True)
        self.sync_bridge_blocks()

        height_after = self.nodes[0].getblockcount()
        assert_greater_than_or_equal(height_after, height_before + JUMP_BLOCKS)
        tips = {node.getbestblockhash() for node in nodes}
        assert_equal(len(tips), 1)
        tip_time = self.nodes[0].getblock(tips.pop())["time"]
        assert_greater_than_or_equal(tip_time, jump_to)
        self.evidence.jump("b3", ONE_DAY, before=height_before, after=height_after,
                           mock_time=jump_to, tip_time=tip_time)

        self.log.info("(7) B3: every validator finalizes past its pre-jump height")
        finalized_after = self.stake_until_finalized_past(finalized_before)
        for node in nodes:
            assert_equal(node.getfinalitystatus()["lineage_broken"], False)
        assert_equal(len({node.getbestblockhash() for node in nodes}), 1)
        self.evidence.assertion("finality advanced on every validator after the one-day jump",
                                expected={"after > before": True},
                                actual={"before": finalized_before, "after": finalized_after})
        self.evidence_step("B3 survived a one-day jump", anvil,
                           finalized_before=finalized_before, finalized_after=finalized_after)

    def stake_until_finalized_past(self, before, timeout=FINALITY_AFTER_JUMP_TIMEOUT):
        """Stake on a mock clock ticking one second per second until every
        validator's finalized height exceeds its entry in `before`; returns
        the finalized heights, frozen on one tip."""
        nodes = self.bridge_nodes()
        self.start_pos()
        deadline = time.time() + timeout
        while True:
            after = [self.finalized_height(node) for node in nodes]
            if all(height > floor for height, floor in zip(after, before)):
                break
            if time.time() >= deadline:
                raise AssertionError(f"finality did not advance within {timeout} s of the jump: "
                                     f"{before} -> {after}")
            self.set_chain_time(self.mock_time + 1)
            time.sleep(1)
        self.freeze()
        return [self.finalized_height(node) for node in nodes]

    def deploy_contract(self, anvil, sender, file, contract, ctor_bytes=b""):
        address, block, code_hash = deploy(anvil, sender, self.bridge_artifact(file, contract),
                                           ctor_bytes)
        receipt_block = anvil.block(block, full=True)
        for tx in receipt_block["transactions"]:
            receipt = anvil.receipt(tx["hash"])
            if (receipt.get("contractAddress") or "").lower() == address:
                self.evidence.gas(f"deploy {contract}", gas_used(receipt), block=block)
        return address, block, code_hash


if __name__ == '__main__':
    B3BridgeRT0ToolchainTest(__file__).main()
