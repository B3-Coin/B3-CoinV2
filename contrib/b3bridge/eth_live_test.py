#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Live ETH -> B3 deposit-leg test (relayer dry-run).

Fetches CURRENT Ethereum mainnet light-client data and a receipt proof for
the latest finalized execution block, writes them to a work directory, and
runs b3-bridge-ethcheck, which performs every verification step in C++:

  checkpoint bootstrap -> period update(s) -> today's finality_update
  -> proven receipts_root -> Merkle-Patricia receipt proof -> receipt decode

Usage:
  eth_live_test.py <workdir>                         smoke test (latest block)
  eth_live_test.py <workdir> --tx 0x.. --vault 0x..  prove a REAL vault deposit
                                                     (tx must be finalized; the
                                                     driver builds the exec-header
                                                     ancestry chain to its block)
"""
import argparse
import json
import subprocess
import sys
import urllib.request

import capture_eth_receipts_fixture as rec  # keccak/rlp/trie/receipt encoding


def fetch(url):
    req = urllib.request.Request(url, headers={
        "Accept": "application/json", "User-Agent": "curl/8.7.1"})
    with urllib.request.urlopen(req, timeout=60) as f:
        return json.load(f)


def rpc(url, method, params):
    req = urllib.request.Request(url, data=json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode(),
        headers={"Content-Type": "application/json", "User-Agent": "curl/8.7.1"})
    with urllib.request.urlopen(req, timeout=60) as f:
        resp = json.load(f)
    if "error" in resp:
        raise RuntimeError(resp["error"])
    return resp["result"]


def _hr(hd):
    import hashlib

    def h(a, b):
        return hashlib.sha256(a + b).digest()

    def u64(v):
        return int(v).to_bytes(8, "little") + b"\x00" * 24

    def hx(s):
        return bytes.fromhex(s[2:])

    leaves = [u64(hd["slot"]), u64(hd["proposer_index"]), hx(hd["parent_root"]),
              hx(hd["state_root"]), hx(hd["body_root"])]
    zero = b"\x00" * 32
    for _ in range(3):
        nxt = []
        for i in range(0, len(leaves), 2):
            nxt.append(h(leaves[i], leaves[i + 1] if i + 1 < len(leaves) else zero))
        leaves = nxt
        zero = h(zero, zero)
    return leaves[0]


EXEC_HEADER_FIELDS = [
    ("parentHash", "h"), ("sha3Uncles", "h"), ("miner", "h"), ("stateRoot", "h"),
    ("transactionsRoot", "h"), ("receiptsRoot", "h"), ("logsBloom", "h"),
    ("difficulty", "n"), ("number", "n"), ("gasLimit", "n"), ("gasUsed", "n"),
    ("timestamp", "n"), ("extraData", "h"), ("mixHash", "h"), ("nonce", "h"),
    ("baseFeePerGas", "n"), ("withdrawalsRoot", "h"), ("blobGasUsed", "n"),
    ("excessBlobGas", "n"), ("parentBeaconBlockRoot", "h"), ("requestsHash", "h"),
]


def encode_exec_header(hdr):
    """Re-encode a JSON execution block header to its canonical RLP. Optional
    trailing fork fields are included only when present. The caller MUST
    verify keccak(result) == the block hash."""
    fields = []
    for name, kind in EXEC_HEADER_FIELDS:
        if name not in hdr or hdr[name] is None:
            break  # trailing optional fields stop here
        v = hdr[name]
        fields.append(int(v, 16) if kind == "n" else bytes.fromhex(v[2:]))
    return rec.rlp(fields)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("workdir")
    ap.add_argument("--tool", default="build/bin/b3-bridge-ethcheck")
    ap.add_argument("--beacon", default="https://ethereum-beacon-api.publicnode.com")
    ap.add_argument("--rpc", default="https://ethereum-rpc.publicnode.com")
    ap.add_argument("--tx", help="transaction hash of a vault deposit to prove")
    ap.add_argument("--vault", help="B3DepositVault address (checks Deposit extraction)")
    args = ap.parse_args()
    import os
    os.makedirs(args.workdir, exist_ok=True)

    def save(name, obj):
        with open(os.path.join(args.workdir, name), "w") as f:
            json.dump(obj, f)

    print("== fetching live mainnet light-client data ==")
    genesis = fetch(args.beacon + "/eth/v1/beacon/genesis")["data"]
    forks = fetch(args.beacon + "/eth/v1/config/fork_schedule")["data"]
    fin_now = fetch(args.beacon + "/eth/v1/beacon/headers/finalized")["data"]["header"]["message"]
    period = int(fin_now["slot"]) // 8192
    print(f"   current finalized slot {fin_now['slot']} (period {period})")

    updates = fetch(args.beacon +
                    f"/eth/v1/beacon/light_client/updates?start_period={period-1}&count=2")
    boot_root = _hr(updates[0]["data"]["finalized_header"]["beacon"])
    bootstrap = fetch(args.beacon + f"/eth/v1/beacon/light_client/bootstrap/0x{boot_root.hex()}")
    finality_update = fetch(args.beacon + "/eth/v1/beacon/light_client/finality_update")

    electra_epoch = None
    for f in forks:
        if f["current_version"].startswith("0x05"):
            electra_epoch = int(f["epoch"])
    assert electra_epoch is not None
    save("config.json", {
        "genesis_validators_root": genesis["genesis_validators_root"],
        "forks": [{"epoch": int(f["epoch"]), "version": f["current_version"]} for f in forks],
        "electra_epoch": electra_epoch,
        "min_participants": 342,
        "trusted_root": "0x" + boot_root.hex(),
    })
    save("bootstrap.json", bootstrap)
    save("updates.json", updates)
    save("finality_update.json", finality_update)

    fin_exec = finality_update["data"]["finalized_header"]["execution"]
    fin_block = int(fin_exec["block_number"])

    exec_chain = None
    if args.tx:
        txr = rpc(args.rpc, "eth_getTransactionReceipt", [args.tx])
        if txr is None:
            sys.exit(f"transaction {args.tx} not found")
        if int(txr["status"], 16) != 1:
            sys.exit(f"transaction {args.tx} FAILED on chain; nothing to prove")
        block_number = int(txr["blockNumber"], 16)
        idx = int(txr["transactionIndex"], 16)
        if block_number > fin_block:
            sys.exit(f"tx block {block_number} is not yet finalized "
                     f"(latest proven finalized block {fin_block}); retry in ~15 minutes")
        depth = fin_block - block_number
        if depth > 20000:
            sys.exit(f"tx is {depth} blocks behind finality; rerun soon after the deposit "
                     f"(the ancestry chain would be excessive)")
        print(f"== building exec-header ancestry: {depth + 1} headers "
              f"({fin_block} down to {block_number}) ==")
        exec_chain = []
        for n in range(fin_block, block_number - 1, -1):
            hdr = rpc(args.rpc, "eth_getBlockByNumber", [hex(n), False])
            enc = encode_exec_header(hdr)
            assert rec.keccak256(enc) == bytes.fromhex(hdr["hash"][2:]),                 f"header re-encoding mismatch at block {n} (new fork field?)"
            exec_chain.append("0x" + enc.hex())
        print(f"   {len(exec_chain)} headers, all keccak-checked against RPC hashes")
    else:
        block_number = fin_block

    print(f"== building receipt proof for block {block_number} ==")
    receipts = rpc(args.rpc, "eth_getBlockReceipts", [hex(block_number)])
    items = [(rec.rlp(i), rec.encode_receipt(r)) for i, r in enumerate(receipts)]
    trie = rec.Trie(items)
    root = trie.root()
    if block_number == fin_block:
        want = bytes.fromhex(fin_exec["receipts_root"][2:])
        assert root == want, f"local trie {root.hex()} != proven receipts_root {want.hex()}"
    else:
        want = bytes.fromhex(rpc(args.rpc, "eth_getBlockByNumber",
                                 [hex(block_number), False])["receiptsRoot"][2:])
        assert root == want, f"local trie {root.hex()} != header receiptsRoot {want.hex()}"
    print(f"   rebuilt {len(receipts)} receipts; root matches the header: 0x{root.hex()}")

    if not args.tx:
        transfer_topic = "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef"
        idx = len(receipts) - 1
        for i, r in enumerate(receipts):
            if any(l["topics"] and l["topics"][0] == transfer_topic for l in r["logs"]):
                idx = i
                break
    key = rec.rlp(idx)
    proof_obj = {
        "block_number": block_number,
        "receipts_root": "0x" + want.hex(),
        "index": idx,
        "key": "0x" + key.hex(),
        "value": "0x" + items[idx][1].hex(),
        "proof": ["0x" + n.hex() for n in trie.prove(key)],
    }
    if exec_chain is not None:
        proof_obj["exec_chain"] = exec_chain
    if args.vault:
        proof_obj["vault"] = args.vault
        proof_obj["expect_deposits"] = 1
    save("receipt_proof.json", proof_obj)

    print(f"== running {args.tool} ==")
    sys.stdout.flush()
    r = subprocess.run([args.tool, args.workdir])
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
