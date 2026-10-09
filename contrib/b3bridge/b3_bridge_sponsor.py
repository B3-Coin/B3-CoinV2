#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""REGTEST ONLY sponsored-fee relayer for bridge V2 burns (owner ruling R10,
open issue OI-8).

A wallet that holds only bUSD V2 cannot pay the native-B3 network fee of a
bridge V2 burn (routes d1 and d3). With bridgewithdraw options.sponsor it
returns a partial transaction instead. The partial has bUSD inputs only, the
BURN output and the BRIDGE_RECORD binding output, a bUSD tip output to this
relayer's address and bUSD change. It has no native output, and every input
is signed SIGHASH_ALL|ANYONECANPAY. The relayer adds one native-B3 fee input
and broadcasts. The user's signatures commit every output, so no one can
change the burn, the recipient, the tip or the change. Each signature
commits only its own input: anyone can add inputs and, with more than one
user input, drop or replace a user input by supplying its bUSD.

Subcommands (each one shot, against the relayer's own --wallet):

refill
    Keeps a pool of pre-sized native fee coins: coins of exactly --value B3
    on addresses labelled --label (default b3-sponsor-fee).
    1. It counts the pool with listunspent: safe, solvable, unlocked coins of
       that label and value with at least --minconf confirmations. The
       default 0 counts a refill that is still unconfirmed, so it is not
       repeated.
    2. It creates the deficit up to --count in one sendmany to fresh legacy
       (P2PKH) addresses of that label. At most 500 coins are created per
       run; remaining_deficit says whether to run again.
    3. Every labelled coin is locked in memory around the sendmany and
       unlocked afterwards, so a refill never spends the pool it refills.
    With no deficit nothing is sent and txid is null.

status
    Reports the pool (the labelled coins, of --value when given), the
    wallet's locked outpoints, its native balances, and the tips: the bUSD
    V2 asset (getbridgev2info.asset_id) this wallet holds.

complete
    Calls the wallet's bridgesponsorburn(--hex, --min-tip, --max-fee,
    {fee_label: --label, ...}). The node refuses a partial that is not a
    bridge V2 burn under the sponsored sighash rule, that pays no tip or a
    tip below --min-tip, that pays native B3, or whose user signatures do
    not commit the unchanged outputs. Otherwise it appends the smallest
    spendable native coin on a --label address between the needed fee and
    --max-fee as the fee input. The whole coin is fee and no change output
    is added. It signs only that input, SIGHASH_ALL, and broadcasts unless
    --no-broadcast. --max-fee is required: pass the pool value, so a coin
    larger than a pool coin is never spent as fee. The txid changes with the
    fee input, and anyone holding the partial may complete it with another
    fee input until it confirms. The user finds the confirmed burn with
    getbridgewithdrawals.

release
    The fee coin of a completion stays locked in memory. If that completion
    never confirms (another completion of the same partial won, or it was
    run with --no-broadcast and never sent), release unlocks it: every
    locked coin on a --label address that is still unspent and not spent in
    the mempool. Locked coins that are spent, or not on a pool address, stay
    locked. Broadcast a --no-broadcast completion you still want before
    running release.

Tips accrue as bUSD V2 in this wallet. Buying native B3 with them needs a
bridge V2 B3/bUSD FlowMesh market, which does not exist yet. Until it does,
keep the pool funded with native B3 from elsewhere.

Fail closed: the B3 endpoint must be plain http on 127.0.0.1 or localhost,
and it is read without any HTTP proxy. getblockchaininfo must report chain
"regtest", because bridge V2 is configured only by the regtest -b3bridgepins
v2 section.

Output: one JSON object on stdout. B3 amounts are exact decimal strings,
never floats. Any refusal prints "error: ..." on stderr and exits 1. Uses
the standard library only. No private key is handled here: the wallet signs.
"""

import argparse
from decimal import Decimal, InvalidOperation
import json
from pathlib import Path
import re
import sys

_HERE = str(Path(__file__).resolve().parent)
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from b3_bridge_observe_v2 import (  # noqa: E402
    _DIRECT_OPENER,
    REGTEST_CHAIN,
    JsonRpc,
    ObserverError,
    RpcError,
)

USER_AGENT = "b3-bridge-sponsor/1"
DEFAULT_LABEL = "b3-sponsor-fee"
# P2PKH: spendable on B3 without witness activation and smallest to sign.
POOL_ADDRESS_TYPE = "legacy"
# One sendmany per run stays far below the standard transaction size.
MAX_REFILL_OUTPUTS = 500
# B3 amounts have nine decimal places (KILO_COIN atomic units per B3);
# fee rates are atomic units per vB with three (AmountFromValue(..., 3)).
B3_DECIMALS = 9
FEE_RATE_DECIMALS = 3
MARKET_NOTE = ("Owner ruling R10: tips accrue as bUSD V2 in this wallet. Buying native B3 with them needs a "
               "bridge V2 B3/bUSD FlowMesh market, which does not exist yet. Keep the fee pool funded with "
               "native B3 from elsewhere.")
_TXID = re.compile(r"[0-9a-f]{64}")


class SponsorError(ObserverError):
    pass


# -- JSON-RPC with exact amounts ------------------------------------------------
def _json_default(value):
    """Decimal as an exact decimal string, never in exponent notation.
    The node's AmountFromValue accepts an amount as a number or a string."""
    if isinstance(value, Decimal):
        return format(value, "f")
    raise TypeError(f"{type(value).__name__} is not JSON serializable")


def dumps(value, **kwargs):
    return json.dumps(value, default=_json_default, **kwargs)


class LocalRpc(JsonRpc):
    """JsonRpc (local endpoints only, never through a proxy) whose replies
    parse JSON numbers with a fraction as Decimal. Decimal parameters are
    sent as exact decimal strings. params is a list, or a dict for named
    arguments. A local or transport failure raises SponsorError."""

    user_agent = USER_AGENT
    error = SponsorError
    reply_json_options = {"parse_float": Decimal}
    json_default = staticmethod(_json_default)


# -- value checks ---------------------------------------------------------------
def require_regtest(node):
    info = node.call("getblockchaininfo")
    chain = info.get("chain") if isinstance(info, dict) else None
    if chain != REGTEST_CHAIN:
        raise SponsorError(f"refusing B3 chain {chain!r}: sponsored bridge V2 fees are regtest only")


def exact_decimal(value, decimals, name, positive=True):
    """A finite Decimal with at most `decimals` places (positive, or >= 0)."""
    if isinstance(value, bool) or not isinstance(value, (Decimal, int, str)):
        raise SponsorError(f"{name} is not a decimal amount")
    try:
        number = Decimal(value)
    except (InvalidOperation, ValueError):
        raise SponsorError(f"{name} is not a decimal amount: {value!r}") from None
    if not number.is_finite():
        raise SponsorError(f"{name} is not a finite amount")
    if number < 0 or (positive and number == 0):
        raise SponsorError(f"{name} must be {'positive' if positive else 'non-negative'}")
    try:
        exact = number == number.quantize(Decimal(1).scaleb(-decimals))
    except InvalidOperation:
        raise SponsorError(f"{name} is out of range") from None
    if not exact:
        raise SponsorError(f"{name} has more than {decimals} decimal places")
    return number


def _coin_amount(coin):
    return exact_decimal(coin.get("amount"), B3_DECIMALS, f"listunspent amount of {_outpoint_text(coin)}",
                         positive=False)


def _reason(error):
    return error.message if isinstance(error, RpcError) else str(error)


def _outpoint(coin):
    return {"txid": coin.get("txid"), "vout": coin.get("vout")}


def _outpoint_text(coin):
    return f"{coin.get('txid')}:{coin.get('vout')}"


def _coin_json(coin):
    return {"txid": coin["txid"], "vout": coin["vout"], "amount": _coin_amount(coin),
            "confirmations": coin.get("confirmations")}


def labelled_coins(wallet, label):
    """Every safe, solvable, unlocked unspent coin of the wallet whose
    address carries `label`, at any depth (listunspent leaves out locked
    coins and asset outputs)."""
    coins = wallet.call("listunspent", [0])
    if not isinstance(coins, list):
        raise SponsorError("listunspent returned no list")
    out = []
    for coin in coins:
        if not isinstance(coin, dict) or coin.get("label") != label:
            continue
        if (coin.get("spendable") is not True or coin.get("solvable") is not True or
                coin.get("safe") is not True):
            continue
        if not isinstance(coin.get("txid"), str) or not _TXID.fullmatch(coin["txid"]):
            raise SponsorError(f"listunspent returned a malformed txid {coin.get('txid')!r}")
        if isinstance(coin.get("vout"), bool) or not isinstance(coin.get("vout"), int):
            raise SponsorError(f"listunspent returned a malformed vout for {coin['txid']}")
        confirmations = coin.get("confirmations")
        if isinstance(confirmations, bool) or not isinstance(confirmations, int):
            raise SponsorError(f"listunspent returned no confirmation count for {_outpoint_text(coin)}")
        _coin_amount(coin)
        out.append(coin)
    return out


def pool_coins(coins, value, minconf=0):
    return [coin for coin in coins if _coin_amount(coin) == value and coin["confirmations"] >= minconf]


# =============================================================================
# refill
# =============================================================================
def refill(node, wallet, count, value, label=DEFAULT_LABEL, minconf=0, fee_rate=None):
    """Create the deficit of `count` pool coins of exactly `value` B3."""
    require_regtest(node)
    if isinstance(count, bool) or not isinstance(count, int) or count < 1:
        raise SponsorError("--count must be a positive integer")
    value = exact_decimal(value, B3_DECIMALS, "--value")
    if fee_rate is not None:
        fee_rate = exact_decimal(fee_rate, FEE_RATE_DECIMALS, "--fee-rate")
    labelled = labelled_coins(wallet, label)
    pool = pool_coins(labelled, value, minconf)
    deficit = max(0, count - len(pool))
    create = min(deficit, MAX_REFILL_OUTPUTS)
    result = {
        "label": label,
        "value": value,
        "count": count,
        "minconf": minconf,
        "pool_before": len(pool),
        "created": create,
        "addresses": [],
        "txid": None,
        "remaining_deficit": deficit - create,
    }
    if create == 0:
        return result

    addresses = []
    for _ in range(create):
        address = wallet.call("getnewaddress", [label, POOL_ADDRESS_TYPE])
        if not isinstance(address, str) or not address or address in addresses:
            raise SponsorError(f"getnewaddress returned an unusable pool address {address!r}; nothing was sent")
        addresses.append(address)

    # Coin selection may otherwise pick the small pool coins themselves. The
    # locks are in memory only, and every labelled coin is protected, of
    # any value or depth.
    protected = [_outpoint(coin) for coin in labelled]
    if protected:
        try:
            wallet.call("lockunspent", [False, protected])
        except ObserverError as e:
            raise SponsorError(f"could not lock the {len(protected)} pool coins for the refill "
                               f"({_reason(e)}); nothing was sent; run refill again") from None
    params = {"dummy": "", "amounts": {address: value for address in addresses}}
    if fee_rate is not None:
        params["fee_rate"] = fee_rate
    txid = send_failure = unlock_failure = None
    try:
        txid = wallet.call("sendmany", params)
    except ObserverError as e:
        send_failure = e
    if protected:
        try:
            wallet.call("lockunspent", [True, protected])
        except ObserverError as e:
            unlock_failure = e
    if send_failure is None:
        state = f"the refill {txid!r} was sent"
    elif isinstance(send_failure, RpcError):
        state = f"sendmany refused the refill: {send_failure.message}"
    else:
        state = (f"sendmany failed ({send_failure}); the refill may have been sent, so check the wallet "
                 "before running refill again")
    if unlock_failure is not None:
        raise SponsorError(f"{state}; unlocking the {len(protected)} pool coins then failed "
                           f"({_reason(unlock_failure)}): unlock them with lockunspent true or restart the "
                           "wallet (the locks are in memory only)")
    if send_failure is not None:
        raise SponsorError(state)
    if not isinstance(txid, str) or not _TXID.fullmatch(txid):
        raise SponsorError(f"sendmany returned no txid ({txid!r})")
    result["addresses"] = addresses
    result["txid"] = txid
    result["protected"] = len(protected)
    return result


# =============================================================================
# status
# =============================================================================
def _tips(node, wallet):
    """(bridge V2 view, the wallet's bUSD V2 holding or None)."""
    try:
        info = node.call("getbridgev2info")
    except RpcError as e:
        return {"configured": False, "reason": e.message}, None
    if not isinstance(info, dict) or info.get("configured") is not True:
        return {"configured": False, "reason": "getbridgev2info reports no complete bridge V2 configuration"}, None
    asset_id = info.get("asset_id")
    if not isinstance(asset_id, str) or not _TXID.fullmatch(asset_id):
        raise SponsorError(f"getbridgev2info returned a malformed asset_id {asset_id!r}")
    view = {"configured": True, "active": info.get("active"), "asset_id": asset_id,
            "next_height": info.get("next_height")}
    held = wallet.call("getwalletassets", [asset_id, 0, True])
    if not isinstance(held, dict) or not isinstance(held.get("assets"), list):
        raise SponsorError("getwalletassets returned no assets list")
    entries = [entry for entry in held["assets"] if isinstance(entry, dict) and entry.get("asset_id") == asset_id]
    if len(entries) > 1:
        raise SponsorError("getwalletassets listed the bUSD V2 asset twice")
    tips = {"asset_id": asset_id, "confirmed": 0, "unconfirmed": 0, "spendable": 0, "utxos": 0}
    if entries:
        entry = entries[0]
        for key in ("confirmed", "unconfirmed", "spendable"):
            amount = entry.get(key)
            if isinstance(amount, bool) or not isinstance(amount, int) or amount < 0:
                raise SponsorError(f"getwalletassets returned a malformed {key} amount")
            tips[key] = amount
        tips["utxos"] = len(entry.get("utxos") or [])
    return view, tips


def status(node, wallet, label=DEFAULT_LABEL, value=None):
    require_regtest(node)
    if value is not None:
        value = exact_decimal(value, B3_DECIMALS, "--value")
    labelled = labelled_coins(wallet, label)
    coins = labelled if value is None else pool_coins(labelled, value)
    by_value = {}
    total = Decimal(0)
    for coin in labelled:
        amount = _coin_amount(coin)
        key = format(amount, "f")
        by_value[key] = by_value.get(key, 0) + 1
    for coin in coins:
        total += _coin_amount(coin)
    locked = wallet.call("listlockunspent")
    if not isinstance(locked, list):
        raise SponsorError("listlockunspent returned no list")
    balances = wallet.call("getbalances")
    if not isinstance(balances, dict) or not isinstance(balances.get("mine"), dict):
        raise SponsorError("getbalances returned no mine balances")
    bridge_v2, tips = _tips(node, wallet)
    return {
        "label": label,
        "value": value,
        "pool": {
            "coins": len(coins),
            "confirmed": sum(1 for coin in coins if coin["confirmations"] > 0),
            "unconfirmed": sum(1 for coin in coins if coin["confirmations"] == 0),
            "total": total,
            "outpoints": [_coin_json(coin) for coin in coins],
        },
        "labelled_by_value": by_value,
        "locked_outpoints": locked,
        "native_balance": balances["mine"],
        "bridge_v2": bridge_v2,
        "tips": tips,
        "complete_max_fee": value,
        "note": MARKET_NOTE,
    }


# =============================================================================
# complete
# =============================================================================
def partial_hex(text):
    """The partial transaction as lower-case hex (whitespace and a 0x prefix are dropped)."""
    if not isinstance(text, str):
        raise SponsorError("--hex is not a string")
    text = "".join(text.split())
    if text[:2] in ("0x", "0X"):
        text = text[2:]
    if not text or len(text) % 2 or not re.fullmatch(r"[0-9a-fA-F]+", text):
        raise SponsorError("--hex is not a hexadecimal transaction")
    return text.lower()


def complete(node, wallet, hex_text, min_tip, *, max_fee, fee_rate=None, minconf=None, broadcast=True,
             label=DEFAULT_LABEL):
    """bridgesponsorburn(hex, min_tip, max_fee, options), then a sanity check
    of its result. max_fee is required (the pool value); label, when not
    None, restricts the fee coin to the labelled pool (options.fee_label)."""
    require_regtest(node)
    partial = partial_hex(hex_text)
    if isinstance(min_tip, bool) or not isinstance(min_tip, int) or min_tip < 1:
        raise SponsorError("--min-tip must be a positive integer of raw bUSD units")
    if max_fee is None:
        raise SponsorError("--max-fee is required: pass the fee pool's coin value")
    max_fee = exact_decimal(max_fee, B3_DECIMALS, "--max-fee")
    options = {"broadcast": bool(broadcast)}
    if fee_rate is not None:
        options["fee_rate"] = exact_decimal(fee_rate, FEE_RATE_DECIMALS, "--fee-rate")
    if minconf is not None:
        if isinstance(minconf, bool) or not isinstance(minconf, int) or minconf < 0:
            raise SponsorError("--minconf must be a non-negative integer")
        options["minconf"] = minconf
    if label is not None:
        options["fee_label"] = _label(label)
    try:
        result = wallet.call("bridgesponsorburn", [partial, min_tip, max_fee, options])
    except RpcError as e:
        raise SponsorError(f"bridgesponsorburn refused the partial: {e.message}") from None

    # The node has checked everything; these checks only catch a reply that
    # contradicts the request. The txid is named because it may already be
    # broadcast.
    if not isinstance(result, dict) or not isinstance(result.get("txid"), str) or \
            not _TXID.fullmatch(result["txid"]):
        raise SponsorError("bridgesponsorburn returned no completed transaction")
    txid = result["txid"]
    if result.get("broadcast") is not options["broadcast"]:
        raise SponsorError(f"bridgesponsorburn reports broadcast={result.get('broadcast')!r} for {txid}, "
                           f"asked for {options['broadcast']}")
    tip = result.get("tip")
    tip_amount = tip.get("amount") if isinstance(tip, dict) else None
    if isinstance(tip_amount, bool) or not isinstance(tip_amount, int) or tip_amount < min_tip:
        raise SponsorError(f"bridgesponsorburn completed {txid} with tip {tip_amount!r}, below --min-tip {min_tip}")
    network_fee = exact_decimal(result.get("network_fee"), B3_DECIMALS, "network_fee")
    if network_fee > max_fee:
        raise SponsorError(f"bridgesponsorburn completed {txid} paying {format(network_fee, 'f')}, above "
                           f"--max-fee {format(max_fee, 'f')}")
    return result


# =============================================================================
# release
# =============================================================================
def release(node, wallet, label=DEFAULT_LABEL):
    """Unlock the stale fee-coin locks: every in-memory locked coin of this
    wallet on a `label` address that is still unspent and not spent in the
    mempool (gettxout with the mempool). Such a coin's completion is in
    neither the chain nor the mempool: another completion of the same
    partial confirmed first, or it was never broadcast. A locked coin that
    is spent (its completion is pending or confirmed), or that is not on a
    pool address, stays locked."""
    require_regtest(node)
    label = _label(label)
    locked = wallet.call("listlockunspent")
    if not isinstance(locked, list):
        raise SponsorError("listlockunspent returned no list")
    released, kept = [], []
    for entry in locked:
        txid = entry.get("txid") if isinstance(entry, dict) else None
        vout = entry.get("vout") if isinstance(entry, dict) else None
        if not isinstance(txid, str) or not _TXID.fullmatch(txid) or isinstance(vout, bool) or \
                not isinstance(vout, int) or vout < 0:
            raise SponsorError(f"listlockunspent returned a malformed outpoint {entry!r}")
        outpoint = {"txid": txid, "vout": vout}
        out = node.call("gettxout", [txid, vout, True])
        if out is None:
            kept.append({**outpoint, "reason": "spent in the chain or the mempool"})
            continue
        script = out.get("scriptPubKey") if isinstance(out, dict) else None
        address = script.get("address") if isinstance(script, dict) else None
        if not isinstance(address, str) or not address:
            kept.append({**outpoint, "reason": "no address"})
            continue
        info = wallet.call("getaddressinfo", [address])
        labels = info.get("labels") if isinstance(info, dict) else None
        if not isinstance(labels, list) or label not in labels:
            kept.append({**outpoint, "reason": f"not labelled {label}"})
            continue
        released.append(outpoint)
    if released:
        try:
            wallet.call("lockunspent", [True, released])
        except ObserverError as e:
            raise SponsorError(f"could not unlock the {len(released)} stale fee coins ({_reason(e)})") from None
    return {"label": label, "released": released, "kept": kept}


# =============================================================================
# CLI
# =============================================================================
def _argument(parse):
    def convert(text):
        try:
            return parse(text)
        except SponsorError as e:
            raise argparse.ArgumentTypeError(str(e)) from None
    return convert


def _integer(minimum, name):
    def parse(text):
        if not re.fullmatch(r"0|[1-9][0-9]*", text or ""):
            raise SponsorError(f"{name} must be a canonical integer")
        number = int(text)
        if number < minimum:
            raise SponsorError(f"{name} must be at least {minimum}")
        return number
    return _argument(parse)


def _b3_amount(name):
    return _argument(lambda text: exact_decimal(text, B3_DECIMALS, name))


def _fee_rate(text):
    return exact_decimal(text, FEE_RATE_DECIMALS, "--fee-rate")


def _label(text):
    if not text or text == "*":
        raise SponsorError("--label must be a non-empty label other than '*'")
    return text


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)

    def b3(parser):
        parser.add_argument("--b3-rpc-url", required=True, help="local B3 node RPC (http://127.0.0.1:P)")
        parser.add_argument("--b3-rpc-user")
        parser.add_argument("--b3-rpc-password")
        parser.add_argument("--b3-cookie")
        parser.add_argument("--wallet", required=True, help="the relayer's dedicated B3 wallet")
        parser.add_argument("--timeout", type=float, default=120)

    refill_p = sub.add_parser("refill", help="top the native fee-coin pool up to --count coins of --value")
    b3(refill_p)
    refill_p.add_argument("--count", type=_integer(1, "--count"), required=True,
                          help="pool size to reach (at most 500 coins are created per run)")
    refill_p.add_argument("--value", type=_b3_amount("--value"), required=True,
                          help="exact B3 value of each pool coin; pass the same value as complete --max-fee")
    refill_p.add_argument("--label", type=_argument(_label), default=DEFAULT_LABEL)
    refill_p.add_argument("--minconf", type=_integer(0, "--minconf"), default=0,
                          help="confirmations a pool coin needs to count (default 0: a pending refill counts)")
    refill_p.add_argument("--fee-rate", type=_argument(_fee_rate),
                          help="sendmany fee rate in atomic units per vB (default: the wallet's estimate)")

    status_p = sub.add_parser("status", help="report the fee pool, the tips and the R10 market note")
    b3(status_p)
    status_p.add_argument("--value", type=_b3_amount("--value"), help="report only pool coins of this value")
    status_p.add_argument("--label", type=_argument(_label), default=DEFAULT_LABEL)

    complete_p = sub.add_parser("complete", help="complete a sponsored burn partial with a fee input")
    b3(complete_p)
    complete_p.add_argument("--hex", required=True,
                            help="the partial transaction: the hex of the user's bridgewithdraw options.sponsor result")
    complete_p.add_argument("--min-tip", type=_integer(1, "--min-tip"), required=True,
                            help="smallest bUSD V2 tip, in raw units, this relayer accepts")
    complete_p.add_argument("--fee-rate", type=_argument(_fee_rate),
                            help="fee rate in atomic units per vB (default: the wallet's estimate)")
    complete_p.add_argument("--max-fee", type=_b3_amount("--max-fee"), required=True,
                            help="largest network fee in B3, the fee coin's whole value: pass the pool value "
                                 "(at most -maxtxfee)")
    complete_p.add_argument("--label", type=_argument(_label), default=DEFAULT_LABEL,
                            help="only coins on addresses with this label pay the fee (the pool's label)")
    complete_p.add_argument("--minconf", type=_integer(0, "--minconf"),
                            help="confirmations the fee coin needs (default 1)")
    complete_p.add_argument("--no-broadcast", action="store_true",
                            help="return the completed transaction without broadcasting it")

    release_p = sub.add_parser("release", help="unlock fee coins whose completion never confirmed")
    b3(release_p)
    release_p.add_argument("--label", type=_argument(_label), default=DEFAULT_LABEL)

    args = p.parse_args(argv)
    if bool(args.b3_rpc_user) != bool(args.b3_rpc_password):
        p.error("--b3-rpc-user and --b3-rpc-password must be supplied together")
    if args.b3_cookie and args.b3_rpc_user:
        p.error("choose cookie or user/password B3 RPC authentication")
    if not args.wallet:
        p.error("--wallet must name the relayer's wallet")
    return args


def run(args):
    node = LocalRpc(args.b3_rpc_url, args.b3_rpc_user, args.b3_rpc_password, args.b3_cookie,
                    timeout=args.timeout)
    wallet = node.wallet(args.wallet)
    if args.command == "refill":
        result = refill(node, wallet, args.count, args.value, label=args.label, minconf=args.minconf,
                        fee_rate=args.fee_rate)
    elif args.command == "status":
        result = status(node, wallet, label=args.label, value=args.value)
    elif args.command == "release":
        result = release(node, wallet, label=args.label)
    else:
        result = complete(node, wallet, args.hex, args.min_tip, max_fee=args.max_fee, fee_rate=args.fee_rate,
                          minconf=args.minconf, broadcast=not args.no_broadcast, label=args.label)
    result["wallet"] = args.wallet
    return result


def main(argv=None):
    args = parse_args(argv)
    try:
        result = run(args)
    except ObserverError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
