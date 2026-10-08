#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""REGTEST ONLY LayerZero test relayer between two local anvils.

Moves the packets of contracts/regtest/MockLzEndpoint.sol from one anvil to
the other (doc/design/b3-bridge-v2-regtest-plan.md sections 3.1 and 4.1):
the Ethereum anvil (chain id 31337, endpoint id 30101: MockOFTAdapter over
MockTetherToken) and the Arbitrum anvil (chain id 31338, endpoint id 30110:
MockUSDT0). Route (b) runs 31338 -> 31337 (USDT0 burned on "Arbitrum", USDT
unlocked to the deposit forwarder F_d), route (d3) 31337 -> 31338 (USDT locked
by the adapter, USDT0 minted to the user).

One `deliver` run:

1. reads the source endpoint's PacketSent(nonce, dstEid, sender, receiver,
   message) logs from --from-block to the source's latest block, keeping only
   those whose dstEid is the destination endpoint's eid();
2. skips a packet the destination endpoint reports isDelivered(srcEid,
   sender, nonce) (so a run can be repeated: delivery is exactly once);
3. simulates deliver(srcEid, sender, nonce, receiver, message) on the
   destination by eth_call, then sends it from --from (an unlocked anvil
   account); a simulated or mined revert, such as an adapter without locked
   USDT, a paused endpoint or a receiver that refuses, is reported as a
   retryable failure and the packet stays undelivered for a later run.

Not modelled, as in the mock endpoint: DVNs and their wait (the harness
decides when to run), executor options, compose and per-path ordering.

Fail closed: both endpoints are plain http on 127.0.0.1 or localhost, read
without any HTTP proxy; both chain ids are in {31337, 31338} and differ; the
two endpoints must have code and different eids. No key is handled here.

Output: one JSON object on stdout ({"packets": [...], "delivered": n,
"already_delivered": n, "failed": n, ...}). Exit 0 when nothing failed, 2
when a packet failed (retryable), 1 with "error: ..." on stderr for a
refusal. relay_packets() is importable. Standard library only.
"""

import argparse
import json
from pathlib import Path
import sys

_CONTRIB = str(Path(__file__).resolve().parent.parent)
if _CONTRIB not in sys.path:
    sys.path.insert(0, _CONTRIB)

from b3_bridge_forwarders_v2 import (  # noqa: E402
    DEFAULT_RECEIPT_TIMEOUT,
    CallReverted,
    ForwarderError,
    as_address,
    code_of,
    decode_abi,
    encode_call,
    eth_call,
    eth_chain_id,
    event_topic,
    get_logs,
    log_parts,
    send_transaction,
    to_0x,
    view,
)
from b3_bridge_observe_v2 import JsonRpc, ObserverError, quantity  # noqa: E402

# Ethereum (31337) and Arbitrum (31338) regtest anvils.
LZ_CHAIN_IDS = (31337, 31338)
EV_PACKET_SENT = "PacketSent(uint64,uint32,address,bytes32,bytes)"
SIG_EID = "eid()"
SIG_IS_DELIVERED = "isDelivered(uint32,bytes32,uint64)"
SIG_DELIVER = "deliver(uint32,bytes32,uint64,address,bytes)"

STATUS_DELIVERED = "delivered"
STATUS_ALREADY = "already-delivered"
STATUS_FAILED = "failed"
STATUS_UNDELIVERABLE = "undeliverable"
EXIT_RETRYABLE = 2


class LzRelayError(ForwarderError):
    pass


def _chain_id(eth, label):
    chain_id = eth_chain_id(eth)
    if chain_id not in LZ_CHAIN_IDS:
        raise LzRelayError(f"refusing {label} chain id {chain_id}: the LayerZero test relayer runs only between "
                           f"the regtest anvils {LZ_CHAIN_IDS[0]} and {LZ_CHAIN_IDS[1]}")
    return chain_id


def _endpoint_eid(eth, endpoint, label):
    if not code_of(eth, endpoint):
        raise LzRelayError(f"{label} endpoint {to_0x(endpoint)} has no code")
    eid = view(eth, endpoint, SIG_EID, returns="uint32")
    if eid == 0:
        raise LzRelayError(f"{label} endpoint reports eid 0")
    return eid


def decode_packet(log):
    """(nonce, dst_eid, sender word, receiver word, message) of a PacketSent log."""
    _, topics, data = log_parts(log)
    if len(topics) != 4 or topics[0] != event_topic(EV_PACKET_SENT):
        raise LzRelayError("not a PacketSent log")
    nonce = int.from_bytes(topics[1], "big")
    dst_eid = int.from_bytes(topics[2], "big")
    if nonce >> 64 or dst_eid >> 32 or topics[3][:12] != bytes(12):
        raise LzRelayError("malformed PacketSent topics")
    receiver, message = decode_abi(["bytes32", "bytes"], data)
    return nonce, dst_eid, topics[3], receiver, message


def relay_packets(src, src_endpoint, dst, dst_endpoint, sender, from_block=0, timeout=DEFAULT_RECEIPT_TIMEOUT):
    """Deliver every undelivered packet src_endpoint sent to dst_endpoint's eid."""
    src_endpoint = as_address(src_endpoint, "--src-endpoint")
    dst_endpoint = as_address(dst_endpoint, "--dst-endpoint")
    sender = as_address(sender, "--from")
    if isinstance(from_block, bool) or not isinstance(from_block, int) or from_block < 0:
        raise LzRelayError("--from-block must be a non-negative block number")
    src_chain = _chain_id(src, "source")
    dst_chain = _chain_id(dst, "destination")
    if src_chain == dst_chain:
        raise LzRelayError(f"source and destination are both chain id {src_chain}")
    src_eid = _endpoint_eid(src, src_endpoint, "source")
    dst_eid = _endpoint_eid(dst, dst_endpoint, "destination")
    if src_eid == dst_eid:
        raise LzRelayError(f"source and destination endpoints both report eid {src_eid}")

    to_block = quantity(src.call("eth_blockNumber"), "eth_blockNumber")
    logs = get_logs(src, src_endpoint, [event_topic(EV_PACKET_SENT), None, dst_eid.to_bytes(32, "big")],
                    from_block, to_block) if from_block <= to_block else []
    packets = []
    for log in logs:
        nonce, packet_dst_eid, sender_word, receiver_word, message = decode_packet(log)
        if packet_dst_eid != dst_eid:
            continue
        entry = {"nonce": nonce, "sender": to_0x(sender_word[12:]), "receiver": to_0x(receiver_word),
                 "message": to_0x(message), "source_tx": log.get("transactionHash"),
                 "source_block": quantity(log.get("blockNumber"), "blockNumber")}
        packets.append(entry)
        if receiver_word[:12] != bytes(12):
            entry.update(status=STATUS_UNDELIVERABLE, retryable=False,
                         error="receiver is not an EVM address word")
            continue
        if view(dst, dst_endpoint, SIG_IS_DELIVERED, src_eid, sender_word, nonce, returns="bool"):
            entry["status"] = STATUS_ALREADY
            continue
        data = encode_call(SIG_DELIVER, src_eid, sender_word, nonce, receiver_word[12:], message)
        try:
            eth_call(dst, dst_endpoint, data, sender=sender, what="deliver")
            receipt = send_transaction(dst, sender, dst_endpoint, data, timeout=timeout, what="deliver")
        except CallReverted as e:
            entry.update(status=STATUS_FAILED, retryable=True, error=e.reason)
            continue
        except ForwarderError as e:
            entry.update(status=STATUS_FAILED, retryable=True, error=str(e))
            continue
        entry.update(status=STATUS_DELIVERED, tx=receipt.get("transactionHash"),
                     block=quantity(receipt.get("blockNumber"), "blockNumber"))

    def count(status):
        return sum(1 for packet in packets if packet["status"] == status)

    return {
        "src_chain_id": src_chain, "dst_chain_id": dst_chain, "src_eid": src_eid, "dst_eid": dst_eid,
        "src_endpoint": to_0x(src_endpoint), "dst_endpoint": to_0x(dst_endpoint),
        "from_block": from_block, "to_block": to_block,
        "packets": packets,
        "delivered": count(STATUS_DELIVERED),
        "already_delivered": count(STATUS_ALREADY),
        "failed": count(STATUS_FAILED) + count(STATUS_UNDELIVERABLE),
    }


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)
    deliver = sub.add_parser("deliver", help="deliver every undelivered packet from one anvil to the other")
    deliver.add_argument("--src-url", required=True, help="source anvil JSON-RPC")
    deliver.add_argument("--src-endpoint", required=True, help="MockLzEndpoint on the source")
    deliver.add_argument("--dst-url", required=True, help="destination anvil JSON-RPC")
    deliver.add_argument("--dst-endpoint", required=True, help="MockLzEndpoint on the destination")
    deliver.add_argument("--from", dest="sender", required=True,
                         help="unlocked destination anvil account that sends deliver() and pays gas")
    deliver.add_argument("--from-block", type=int, default=0, help="first source block scanned")
    deliver.add_argument("--timeout", type=float, default=DEFAULT_RECEIPT_TIMEOUT)
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        src = JsonRpc(args.src_url, timeout=args.timeout)
        dst = JsonRpc(args.dst_url, timeout=args.timeout)
        result = relay_packets(src, args.src_endpoint, dst, args.dst_endpoint, args.sender,
                               from_block=args.from_block, timeout=args.timeout)
    except ObserverError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return EXIT_RETRYABLE if result["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
