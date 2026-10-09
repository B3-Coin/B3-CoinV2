#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""REGTEST ONLY one-shot bridge V2 Ethereum state observer.

Builds and submits kind 9 ETH_STATE_OBSERVATION records
(doc/design/b3-bridge-v2-node-changes.md section 8.2), which open the bridge
V2 burn gate (section 8.4, rows E1-E8):

1. getbridgev2info gives observation_plan: the newest retained V2 execution
   anchor that carries a state root and, per subject, the accounts and
   storage keys to prove (1 VERIFIER_E, 2 RESERVE_E, 3 TOKEN_FLAGS and, from
   a B-23 successor's migration height, 18 RESERVE_E_SUCCESSOR and 19
   TOKEN_FLAGS_SUCCESSOR, the same reads of the successor vault; node
   changes section 7.6).
2. For each requested subject (by default every subject the plan lists, so
   18 and 19 join 1-3 from the migration height without a flag),
   eth_getProof(address, slots, anchor.number) is called for every account
   of the plan, in plan order.
3. buildbridgeobservationv2(subject, anchor.hash_evm, proofs) verifies the
   proofs against the anchor's state root and returns the canonical payload
   (with B3's own VERIFIER_E verdict).
4. Unless --dry-run, the wallet's submitbridgecarrier funds, signs and
   broadcasts each payload.

Every subject is built before anything is submitted, so a refusal while
building submits nothing (a submission refused after an earlier one names the
carriers already broadcast). Fail closed:

- both endpoints are plain http on 127.0.0.1 or localhost, read without any
  HTTP proxy: bridge V2 is configured only by the regtest -b3bridgepins v2
  section, and getblockchaininfo must report chain "regtest";
- the Ethereum endpoint's eth_chainId must equal the plan's origin_chain_id;
- the plan must carry an anchor, its block on the Ethereum endpoint must have
  the anchor's hash, and the anchor must be newer than each requested
  subject's latest observation (a record naming an older anchor is invalid);
- a requested subject must be in the plan (the successor subjects are listed
  only once the migration is active for the next block);
- every eth_getProof result must answer for the plan's account and keys, in
  order (the node checks this again and verifies every proof);
- each built record must name the requested subject and the plan's anchor,
  and each submission must report record_kind eth-state-observation.

Output: one JSON object on stdout (the anchor and, per subject, the decoded
values, verdict, payload and, when submitted, the txid). Any refusal prints
"error: ..." on stderr and exits 1. Standard library only; no private key is
handled here (the B3 wallet signs).
"""

import argparse
import base64
import json
from pathlib import Path
import sys
import urllib.error
import urllib.parse
import urllib.request

LOCAL_HOSTS = ("127.0.0.1", "localhost")
REGTEST_CHAIN = "regtest"
SUBJECTS = {1: "VERIFIER_E", 2: "RESERVE_E", 3: "TOKEN_FLAGS",
            18: "RESERVE_E_SUCCESSOR", 19: "TOKEN_FLAGS_SUCCESSOR"}
SUBJECT_ALIASES = {name.lower(): number for number, name in SUBJECTS.items()}
RECORD_KIND = "eth-state-observation"
USER_AGENT = "b3-bridge-observe-v2/1"

_DIRECT_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class ObserverError(RuntimeError):
    pass


class RpcError(ObserverError):
    def __init__(self, method, error):
        self.method = method
        self.code = error.get("code") if isinstance(error, dict) else None
        self.message = error.get("message", str(error)) if isinstance(error, dict) else str(error)
        # An Ethereum node's revert data (eth_call "execution reverted"), if any.
        self.data = error.get("data") if isinstance(error, dict) else None
        super().__init__(f"{method}: RPC {self.code}: {self.message}")


def check_local_url(url):
    """Refuse every endpoint except plain http on 127.0.0.1 or localhost."""
    try:
        parts = urllib.parse.urlsplit(url)
        host = parts.hostname
        parts.port  # Raises ValueError on a malformed port.
    except (TypeError, ValueError, AttributeError):
        raise ObserverError(f"refusing malformed endpoint {url!r}") from None
    if (parts.scheme != "http" or host not in LOCAL_HOSTS or
            parts.username is not None or parts.password is not None):
        raise ObserverError(
            f"refusing non-local endpoint {url!r}: only http://127.0.0.1 or "
            "http://localhost are allowed (bridge V2 is regtest only)")
    return url


class JsonRpc:
    """JSON-RPC 2.0 over a local endpoint, never through a proxy.

    params is a list, or a dict for named arguments. A subclass (such as
    b3_bridge_sponsor.LocalRpc) may set user_agent; error, the ObserverError
    subclass raised for a local or transport failure; reply_json_options,
    json.loads keyword arguments for replies (parse_float=Decimal, say); and
    json_default, a staticmethod used as json.dumps' default for request
    values. The endpoint check and the proxy-free opener (_DIRECT_OPENER)
    are not hooks.
    """

    user_agent = USER_AGENT
    error = ObserverError
    reply_json_options = {}
    json_default = None

    def __init__(self, url, user=None, password=None, cookie=None, timeout=120):
        self.url = check_local_url(url).rstrip("/")
        self.user, self.password, self.cookie = user, password, cookie
        self.timeout = timeout
        self.next_id = 0

    def wallet(self, name):
        return type(self)(self.url + "/wallet/" + urllib.parse.quote(name, safe=""),
                          self.user, self.password, self.cookie, self.timeout)

    def call(self, method, params=()):
        self.next_id += 1
        headers = {"Content-Type": "application/json", "User-Agent": self.user_agent}
        auth = None
        if self.cookie:
            try:
                auth = Path(self.cookie).read_text().strip()
            except OSError as e:
                raise self.error(f"cannot read the B3 RPC cookie: {e}") from None
        elif self.user is not None:
            auth = f"{self.user}:{self.password}"
        if auth is not None:
            headers["Authorization"] = "Basic " + base64.b64encode(auth.encode()).decode()
        body = json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method,
                           "params": dict(params) if isinstance(params, dict) else list(params)},
                          default=self.json_default).encode()
        request = urllib.request.Request(self.url, data=body, headers=headers)
        try:
            with _DIRECT_OPENER.open(request, timeout=self.timeout) as response:
                reply = json.loads(response.read(), **self.reply_json_options)
        except urllib.error.HTTPError as e:
            try:
                reply = json.loads(e.read(), **self.reply_json_options)
            except (ValueError, OSError):
                raise self.error(f"{method}: HTTP {e.code} from {self.url}") from None
        except (OSError, ValueError) as e:
            raise self.error(f"{method}: transport failure ({type(e).__name__})") from None
        if not isinstance(reply, dict):
            raise self.error(f"{method}: malformed JSON-RPC reply")
        if reply.get("error") is not None:
            raise RpcError(method, reply["error"])
        return reply.get("result")


# -- value checks ---------------------------------------------------------------
def hex_bytes(value, size=None, name="value"):
    """Raw bytes of a 0x-prefixed (or bare) hex string of an exact size."""
    if not isinstance(value, str):
        raise ObserverError(f"{name} is not a hexadecimal string")
    text = value[2:] if value[:2] in ("0x", "0X") else value
    try:
        data = bytes.fromhex(text)
    except ValueError:
        raise ObserverError(f"{name} is not hexadecimal: {value!r}") from None
    if size is not None and len(data) != size:
        raise ObserverError(f"{name} must be {size} bytes, got {len(data)}")
    return data


def storage_key(value, name):
    """An eth_getProof storage key as 32 bytes (a shorter quantity is left-padded)."""
    if not isinstance(value, str):
        raise ObserverError(f"{name} is not a hexadecimal string")
    text = value[2:] if value[:2] in ("0x", "0X") else value
    if not 0 < len(text) <= 64:
        raise ObserverError(f"{name} is not a storage key of at most 32 bytes")
    return hex_bytes(text.rjust(64, "0"), 32, name)


def quantity(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ObserverError(f"{name} is not an integer quantity")
    if isinstance(value, int):
        return value
    try:
        return int(value, 16) if value[:2] in ("0x", "0X") else int(value)
    except ValueError:
        raise ObserverError(f"{name} is not an integer quantity: {value!r}") from None


def parse_subjects(text):
    """'1,2,3' (18, 19 for the successor) or names (verifier_e, ...), deduplicated, ascending."""
    chosen = set()
    for item in text.split(","):
        item = item.strip().lower()
        if item in SUBJECT_ALIASES:
            chosen.add(SUBJECT_ALIASES[item])
        elif item.isdigit() and int(item) in SUBJECTS:
            chosen.add(int(item))
        else:
            raise ObserverError(f"unknown subject {item!r}: expected one of "
                                f"{', '.join(str(n) for n in sorted(SUBJECTS))} or "
                                f"{', '.join(sorted(SUBJECT_ALIASES))}")
    if not chosen:
        raise ObserverError("no subject requested")
    return sorted(chosen)


# -- the observation round -----------------------------------------------------
def load_plan(node, eth):
    """getbridgev2info's plan after the chain, configuration and anchor checks."""
    chain = (node.call("getblockchaininfo") or {}).get("chain")
    if chain != REGTEST_CHAIN:
        raise ObserverError(f"refusing B3 chain {chain!r}: bridge V2 observations are regtest only")
    info = node.call("getbridgev2info")
    if not isinstance(info, dict) or info.get("configured") is not True:
        raise ObserverError("the bridge V2 configuration is incomplete")
    if info.get("active") is not True:
        raise ObserverError(f"bridge V2 rules are not active for the next block "
                            f"(height {info.get('next_height')}, V {info.get('activation_height')})")
    if info.get("state_available") is not True:
        raise ObserverError("the B3 bridge state is unavailable at the active tip")
    origin_chain_id = quantity(info.get("origin_chain_id"), "origin_chain_id")
    eth_chain_id = quantity(eth.call("eth_chainId"), "eth_chainId")
    if eth_chain_id != origin_chain_id:
        raise ObserverError(f"the Ethereum endpoint reports chain id {eth_chain_id}; the bridge V2 "
                            f"vault entry is on chain {origin_chain_id}")
    plan = info.get("observation_plan")
    if not isinstance(plan, dict):
        raise ObserverError("getbridgev2info has no observation_plan")
    anchor = plan.get("anchor")
    if not isinstance(anchor, dict):
        raise ObserverError("no retained bridge V2 execution anchor carries a state root yet; "
                            "relay a light-client update first")
    number = quantity(anchor.get("number"), "anchor.number")
    anchor_hash = hex_bytes(anchor.get("hash_evm"), 32, "anchor.hash_evm")
    header = eth.call("eth_getBlockByNumber", [hex(number), False])
    if not isinstance(header, dict) or hex_bytes(header.get("hash"), 32, "block hash") != anchor_hash:
        raise ObserverError(f"Ethereum block {number} on the endpoint is not the B3 anchor "
                            f"0x{anchor_hash.hex()}")
    return info, plan, {"number": number, "hash_evm": "0x" + anchor_hash.hex(),
                        "timestamp": anchor.get("timestamp")}


def planned_subjects(plan):
    """The default subjects: every subject the plan lists that this observer
    knows, ascending. getbridgev2info lists 1-3 and, once a B-23 successor's
    migration is active for the next block, 18 and 19, whose observations
    the successor's burn gate (rows E7 and E8) reads."""
    listed = sorted({entry.get("subject") for entry in plan.get("subjects") or []
                     if isinstance(entry, dict) and type(entry.get("subject")) is int
                     and entry.get("subject") in SUBJECTS})
    if not listed:
        raise ObserverError("observation_plan lists no subject")
    return listed


def subject_plan(plan, subject):
    entries = [entry for entry in plan.get("subjects") or [] if entry.get("subject") == subject]
    if len(entries) != 1:
        hint = (" (the successor subjects exist only from the B-23 migration height)"
                if subject in (18, 19) and not entries else "")
        raise ObserverError(f"observation_plan has no single entry for subject {subject}{hint}")
    entry = entries[0]
    if entry.get("name") != SUBJECTS[subject]:
        raise ObserverError(f"observation_plan names subject {subject} {entry.get('name')!r}, "
                            f"not {SUBJECTS[subject]}")
    if entry.get("anchor_is_newer") is not True:
        raise ObserverError(
            f"{SUBJECTS[subject]}: the plan's anchor is not newer than its latest observation "
            f"(anchor block {entry.get('latest_anchor_block')}); relay a newer light-client update first")
    accounts = entry.get("accounts")
    if not isinstance(accounts, list) or not accounts:
        raise ObserverError(f"{SUBJECTS[subject]}: the plan lists no account")
    return entry


def fetch_proofs(eth, entry, anchor):
    """eth_getProof for each plan account at the anchor block, checked against the plan."""
    proofs = []
    name = entry["name"]
    for i, account in enumerate(entry["accounts"]):
        address = hex_bytes(account.get("address"), 20, f"{name} account {i} address")
        slots = [storage_key(slot, f"{name} account {i} slot") for slot in account.get("slots") or []]
        if not slots:
            raise ObserverError(f"{name}: the plan lists no slot for account {i}")
        result = eth.call("eth_getProof", ["0x" + address.hex(), ["0x" + slot.hex() for slot in slots],
                                           hex(anchor["number"])])
        if not isinstance(result, dict):
            raise ObserverError(f"{name}: eth_getProof returned no result object")
        if hex_bytes(result.get("address"), 20, "eth_getProof address") != address:
            raise ObserverError(f"{name}: eth_getProof answered for another address than 0x{address.hex()}")
        storage = result.get("storageProof")
        if not isinstance(storage, list) or len(storage) != len(slots):
            raise ObserverError(f"{name}: eth_getProof proves {len(storage or [])} slots of 0x{address.hex()}, "
                                f"the plan needs {len(slots)}")
        for j, (proved, slot) in enumerate(zip(storage, slots)):
            if not isinstance(proved, dict) or storage_key(proved.get("key"), "storageProof key") != slot:
                raise ObserverError(f"{name}: eth_getProof storage proof {j} of 0x{address.hex()} is not "
                                    f"for the plan's slot 0x{slot.hex()}")
        if not isinstance(result.get("accountProof"), list) or not result["accountProof"]:
            raise ObserverError(f"{name}: eth_getProof has no account proof for 0x{address.hex()}")
        proofs.append(result)
    return proofs


def build_observation(node, eth, plan, anchor, subject):
    entry = subject_plan(plan, subject)
    proofs = fetch_proofs(eth, entry, anchor)
    built = node.call("buildbridgeobservationv2", [subject, anchor["hash_evm"], proofs])
    if not isinstance(built, dict) or built.get("subject") != subject:
        raise ObserverError(f"{SUBJECTS[subject]}: buildbridgeobservationv2 built another subject")
    if (hex_bytes(built.get("anchor_hash_evm"), 32, "anchor_hash_evm") != hex_bytes(anchor["hash_evm"], 32) or
            built.get("anchor_block") != anchor["number"]):
        raise ObserverError(f"{SUBJECTS[subject]}: buildbridgeobservationv2 named another anchor")
    payload = hex_bytes(built.get("payload_hex"), None, "payload_hex")
    if not payload:
        raise ObserverError(f"{SUBJECTS[subject]}: buildbridgeobservationv2 returned an empty payload")
    return {
        "subject": subject,
        "name": SUBJECTS[subject],
        "anchor_block": built["anchor_block"],
        "anchor_hash_evm": built["anchor_hash_evm"],
        "t_obs": built.get("t_obs"),
        "next_height": built.get("next_height"),
        "verdict": built.get("verdict"),
        "values": built.get("values"),
        "payload_hex": payload.hex(),
        "payload_bytes": len(payload),
        "accounts": [{"address": account["address"], "slots": list(account["slots"])}
                     for account in entry["accounts"]],
    }


def observe(node, wallet, eth, subjects=None, dry_run=False):
    """One observation round; returns the stdout result object. `subjects`
    None observes every subject the plan lists (planned_subjects)."""
    info, plan, anchor = load_plan(node, eth)
    if subjects is None:
        subjects = planned_subjects(plan)
    built = [build_observation(node, eth, plan, anchor, subject) for subject in subjects]
    if not dry_run:
        if wallet is None:
            raise ObserverError("submitting needs a wallet")
        submitted = []
        for observation in built:
            try:
                result = wallet.call("submitbridgecarrier", [observation["payload_hex"]])
            except RpcError as e:
                raise ObserverError(f"{observation['name']}: submitbridgecarrier refused the record "
                                    f"({e.message}); already submitted: {submitted}") from None
            if not isinstance(result, dict) or result.get("record_kind") != RECORD_KIND:
                raise ObserverError(f"{observation['name']}: submitbridgecarrier did not create an "
                                    f"{RECORD_KIND} carrier; already submitted: {submitted}")
            observation["txid"] = result.get("txid")
            submitted.append(observation["txid"])
    return {
        "origin_chain_id": info["origin_chain_id"],
        "next_height": info.get("next_height"),
        "anchor": anchor,
        "dry_run": dry_run,
        "observations": built,
    }


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ethereum-rpc", required=True, help="local Ethereum JSON-RPC (anvil)")
    p.add_argument("--b3-rpc-url", required=True, help="local B3 node RPC")
    p.add_argument("--b3-rpc-user")
    p.add_argument("--b3-rpc-password")
    p.add_argument("--b3-cookie")
    p.add_argument("--wallet", help="B3 wallet that funds the carriers (required unless --dry-run)")
    p.add_argument("--subjects",
                   help="comma list of 1/verifier_e, 2/reserve_e, 3/token_flags and, from a B-23 "
                        "successor's migration height, 18/reserve_e_successor, "
                        "19/token_flags_successor (default: every subject getbridgev2info's "
                        "observation_plan lists, i.e. 1,2,3 and, from the migration height, 18,19)")
    p.add_argument("--dry-run", action="store_true", help="build and print the records, submit nothing")
    p.add_argument("--timeout", type=float, default=120)
    args = p.parse_args(argv)
    if bool(args.b3_rpc_user) != bool(args.b3_rpc_password):
        p.error("--b3-rpc-user and --b3-rpc-password must be supplied together")
    if args.b3_cookie and args.b3_rpc_user:
        p.error("choose cookie or user/password B3 RPC authentication")
    if not args.dry_run and not args.wallet:
        p.error("--wallet is required unless --dry-run")
    try:
        args.subject_list = None if args.subjects is None else parse_subjects(args.subjects)
    except ObserverError as e:
        p.error(str(e))
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        eth = JsonRpc(args.ethereum_rpc, timeout=args.timeout)
        node = JsonRpc(args.b3_rpc_url, args.b3_rpc_user, args.b3_rpc_password, args.b3_cookie,
                       timeout=args.timeout)
        wallet = node.wallet(args.wallet) if args.wallet else None
        result = observe(node, wallet, eth, args.subject_list, dry_run=args.dry_run)
    except ObserverError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
