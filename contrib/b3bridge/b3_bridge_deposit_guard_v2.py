#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""REGTEST ONLY one-shot bridge V2 deposit guard for relayers.

Once B3 has closed bridge V2 burns for good (doc/design/b3-bridge-v2-node-
changes.md section 8.5: a sticky conflict from kind 13 FINALITY_CONFLICT
evidence, E5 or E8, a broken fresh lineage, or a latest VERIFIER_E showing
the verifier halted, cancelled, expired or depositsClosed), B3 still mints
every proven Vault V2 deposit, because the vault cannot refund, but the bUSD
V2 it mints can never be redeemed. A relayer therefore asks B3 before it
makes a deposit or flushes deposits:

1. getblockchaininfo must report chain "regtest";
2. getbridgev2info must report configured and active for the next block
   (a node without a bridge V2 envelope refuses getbridgev2info);
3. getbridgev2info.deposits.status decides:
   - OPEN prints {"deposits": "OPEN", "next_height": n} and exits 0;
   - CLOSED_FOR_GOOD prints "error: bridge V2 deposits are closed for good:
     <reason>; this relayer refuses deposits and flushes" and exits 1;
   - UNAVAILABLE, or any other answer, exits 1 as well (fail closed).

OPEN only means B3 has not closed deposits; it does not mean Verifier V2
depositsOpen() is true, which the vault checks itself.

The B3 endpoint must be plain http on 127.0.0.1 or localhost and is read
without any HTTP proxy (JsonRpc and check_local_url of
b3_bridge_observe_v2.py). deposit_guard(node) is the importable check for a
deposit or flush path: it returns the OPEN result and raises
DepositsClosedError when B3 closed deposits for good, or ObserverError for
every other refusal. Standard library only; no key is handled here.
"""

import argparse
import json
from pathlib import Path
import sys

_HERE = str(Path(__file__).resolve().parent)
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from b3_bridge_observe_v2 import (  # noqa: E402
    REGTEST_CHAIN,
    JsonRpc,
    ObserverError,
    RpcError,
    check_local_url,
    quantity,
)

STATUS_OPEN = "OPEN"
STATUS_CLOSED = "CLOSED_FOR_GOOD"
STATUS_UNAVAILABLE = "UNAVAILABLE"
REFUSAL_SUFFIX = "this relayer refuses deposits and flushes"


class DepositsClosedError(ObserverError):
    """B3 closed bridge V2 deposits for good; .reason is B3's reason."""

    def __init__(self, reason):
        self.reason = reason
        super().__init__(f"bridge V2 deposits are closed for good: {reason}; {REFUSAL_SUFFIX}")


def deposit_guard(node):
    """{"deposits": "OPEN", "next_height": n} when B3 has not closed bridge V2
    deposits for good; DepositsClosedError when it has, ObserverError for a
    non-regtest chain, an unconfigured or inactive bridge V2, an unavailable
    or unknown status."""
    chain_info = node.call("getblockchaininfo")
    chain = chain_info.get("chain") if isinstance(chain_info, dict) else None
    if chain != REGTEST_CHAIN:
        raise ObserverError(f"refusing B3 chain {chain!r}: the bridge V2 deposit guard is regtest only")
    try:
        info = node.call("getbridgev2info")
    except RpcError as e:
        # A node without a bridge V2 envelope answers "Bridge V2 is not
        # configured on this chain"; any other refusal fails closed as well.
        raise ObserverError(f"getbridgev2info refused: {e.message}; {REFUSAL_SUFFIX}") from None
    if not isinstance(info, dict) or info.get("configured") is not True:
        raise ObserverError(f"the bridge V2 configuration is incomplete; {REFUSAL_SUFFIX}")
    if info.get("active") is not True:
        raise ObserverError(f"bridge V2 rules are not active for the next block "
                            f"(height {info.get('next_height')}, V {info.get('activation_height')}); "
                            f"{REFUSAL_SUFFIX}")
    next_height = quantity(info.get("next_height"), "next_height")
    deposits = info.get("deposits")
    if not isinstance(deposits, dict):
        raise ObserverError(f"getbridgev2info reports no deposit status; {REFUSAL_SUFFIX}")
    status = deposits.get("status")
    reason = deposits.get("reason")
    if status == STATUS_OPEN:
        return {"deposits": STATUS_OPEN, "next_height": next_height}
    if status == STATUS_CLOSED:
        raise DepositsClosedError(reason if isinstance(reason, str) and reason else "no reason reported")
    if status == STATUS_UNAVAILABLE:
        raise ObserverError(f"the bridge V2 deposit status is unavailable ({reason}); {REFUSAL_SUFFIX}")
    raise ObserverError(f"unknown bridge V2 deposit status {status!r}; {REFUSAL_SUFFIX}")


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--b3-rpc-url", required=True, help="local B3 node RPC")
    p.add_argument("--b3-rpc-user")
    p.add_argument("--b3-rpc-password")
    p.add_argument("--b3-cookie")
    p.add_argument("--timeout", type=float, default=120)
    args = p.parse_args(argv)
    if bool(args.b3_rpc_user) != bool(args.b3_rpc_password):
        p.error("--b3-rpc-user and --b3-rpc-password must be supplied together")
    if args.b3_cookie and args.b3_rpc_user:
        p.error("choose cookie or user/password B3 RPC authentication")
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        # Refused before any request unless plain http on 127.0.0.1 or localhost.
        node = JsonRpc(check_local_url(args.b3_rpc_url), args.b3_rpc_user, args.b3_rpc_password,
                       args.b3_cookie, timeout=args.timeout)
        result = deposit_guard(node)
    except ObserverError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
