"""TEST ONLY shared-spot reference model. No network, signing or node imports.

Inputs marked authorized/verified are synthetic trust-boundary fixtures, NOT
cryptographic proofs. This code must never be used as a production verifier.
"""
from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path

from curves import auction, buy_bound, evaluate, validate_curve


class ModelError(ValueError):
    """Entire candidate rejected; the caller's committed model is unchanged."""


class Refusal(Exception):
    """Authenticated state refusal; consumes only sequence and stored outcome."""


def canonical(value):
    def inspect(v):
        if type(v) in (str, int, bool) or v is None:
            return
        if type(v) is list:
            for x in v:
                inspect(x)
            return
        if type(v) is dict and all(type(k) is str for k in v):
            for x in v.values():
                inspect(x)
            return
        raise ModelError("NON_CANONICAL_TYPE")
    inspect(value)
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("ascii")


def _hash(tag, value):
    # TEST serialization only. Not a proposed production signed codec.
    return hashlib.sha256(canonical([tag, value])).hexdigest()


def _id(value):
    if (type(value) is not str or len(value) != 64
            or any(c not in "0123456789abcdef" for c in value)):
        raise ModelError("BAD_EXACT_ID")
    return value


def _integer(value, maximum, minimum=0):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ModelError("INTEGER_BOUND")
    return value


def load_profile():
    return json.loads(Path(__file__).with_name("TEST_PROFILE.json").read_text())


def action(account, sequence, kind, **params):
    return {"profile_id": "flowmesh-v2-isolated-accounting-test/1",
            "account": account, "sequence": sequence, "kind": kind,
            "params": deepcopy(params), "authorized": True}


def _semantic(a):
    return {k: deepcopy(a[k]) for k in ("profile_id", "account", "sequence", "kind", "params")}


def action_id(a):
    return _hash("TEST/INSTRUCTION/1", _semantic(a))


def order_id(a):
    return _hash("TEST/ORDER/1", [a["profile_id"], action_id(a)])


class Model:
    def __init__(self, profile, assets, markets, seats, treasury_owner, subaccounts):
        self.profile = deepcopy(profile)
        self.assets = deepcopy(assets)
        self.seats = deepcopy(seats)
        self.treasury_owner = _id(treasury_owner)
        self.subaccounts = deepcopy(subaccounts)
        self.market_inputs = deepcopy(markets)
        expected = load_profile()
        # Bounds and synthetic stable IDs may vary in named test fixtures;
        # every complete configuration is retained in snapshots/market identity.
        for k, v in expected.items():
            if k not in ("limits", "stable_fee_assets") and profile.get(k) != v:
                raise ModelError("UNSUPPORTED_TEST_PROFILE")
        if set(profile) != set(expected) or set(profile["limits"]) != set(expected["limits"]):
            raise ModelError("BAD_PROFILE_SHAPE")
        for n in profile["limits"].values():
            _integer(n, expected["limits"]["intermediate"], 1)
        self.limits = self.profile["limits"]
        for aid, meta in self.assets.items():
            _id(aid)
            if set(meta) != {"decimals", "symbol"} or type(meta["symbol"]) is not str:
                raise ModelError("BAD_ASSET_METADATA")
            _integer(meta["decimals"], 18)
        stable = profile["stable_fee_assets"]
        if type(stable) is not list or len(stable) != len(set(stable)):
            raise ModelError("BAD_STABLE_ASSET_LIST")
        for aid in stable:
            _id(aid)
        if not seats or len(seats) > self.limits["records"]:
            raise ModelError("BAD_RECIPIENT_CONTEXT")
        for seat, authority in seats.items():
            _id(seat)
            _id(authority)
        for sub, owner in subaccounts.items():
            _id(sub)
            _id(owner)
        self.markets = {}
        self.market_ids = []
        if not 0 < len(markets) <= self.limits["markets"]:
            raise ModelError("MARKET_LIMIT")
        for supplied in markets:
            if set(supplied) != {"base", "quote", "base_atoms_per_lot", "quote_atoms_per_tick"}:
                raise ModelError("BAD_MARKET_SHAPE")
            m = deepcopy(supplied)
            if m["base"] not in assets or m["quote"] not in assets or m["base"] == m["quote"]:
                raise ModelError("BAD_MARKET_ASSETS")
            if m["quote"] not in stable:
                raise ModelError("NON_STABLE_QUOTE_NOT_IMPLEMENTED")
            for k in ("base_atoms_per_lot", "quote_atoms_per_tick"):
                self._amount(m[k], 1)
            mid = _hash("TEST/MARKET/1", [self.profile, m,
                        assets[m["base"]]["decimals"], assets[m["quote"]]["decimals"]])
            if mid in self.markets:
                raise ModelError("DUPLICATE_MARKET")
            self.markets[mid] = m
            self.market_ids.append(mid)
        self.state = {k: {} for k in (
            "spot", "futures", "custody", "orders", "next_sequence", "outcomes",
            "instructions", "deposits", "pending", "settled", "fn", "treasury",
            "fee_pool", "fees_collected", "v1", "v1_receipts", "v1_external")}
        self.assert_invariants()

    def _amount(self, n, minimum=0):
        return _integer(n, self.limits["amount"], minimum)

    def _product(self, *items):
        value = 1
        for item in items:
            value *= item
            _integer(value, self.limits["intermediate"])
        return value

    def _fee(self, notional, ceil=False):
        n = self._product(notional, self.profile["fee_ppm_per_side"])
        quotient, remainder = divmod(n, 1_000_000)
        return quotient + (1 if ceil and remainder else 0)

    def _get(self, bucket, owner, asset):
        return self.state[bucket].get(owner, {}).get(asset, 0)

    def _add(self, bucket, owner, asset, delta):
        new = self._get(bucket, owner, asset) + delta
        self._amount(new)
        self.state[bucket].setdefault(owner, {})[asset] = new

    def _flat_add(self, bucket, asset, delta):
        new = self.state[bucket].get(asset, 0) + delta
        self._amount(new)
        self.state[bucket][asset] = new

    def fn_account(self, seat):
        if seat not in self.seats:
            raise ModelError("UNKNOWN_SEAT")
        return _hash("TEST/FN-CLAIM/1", [self.profile, seat, self.seats[seat]])

    def treasury_account(self):
        return _hash("TEST/TREASURY-CLAIM/1", [self.profile, self.treasury_owner])

    def _protocol_accounts(self):
        return {self.treasury_account()} | {self.fn_account(s) for s in self.seats}

    def _deposit_identity(self, fact):
        return _hash("TEST/DEPOSIT/1", [fact["chain"], fact["custody_version"],
                     fact["custody_domain"], fact["txid"], fact["vout"]])

    def _validate_deposit(self, fact):
        required = {"chain", "custody_version", "custody_domain", "txid", "vout",
                    "account", "asset", "amount", "height", "tx_index", "output_index", "verified"}
        if set(fact) != required or fact["verified"] is not True:
            raise ModelError("INVALID_DEPOSIT_FACT")
        if type(fact["chain"]) is not str or not fact["chain"] or fact["custody_version"] != 2:
            raise ModelError("WRONG_CUSTODY_VERSION")
        _integer(fact["custody_version"], 2, 2)
        for k in ("custody_domain", "txid", "account", "asset"):
            _id(fact[k])
        if fact["asset"] not in self.assets or fact["account"] in self._protocol_accounts():
            raise ModelError("BAD_DEPOSIT_TARGET")
        self._amount(fact["amount"], 1)
        for k in ("vout", "height", "tx_index", "output_index"):
            _integer(fact[k], self.limits["sequence"])
        if fact["vout"] != fact["output_index"]:
            raise ModelError("OUTPOINT_POSITION_MISMATCH")

    def _credit(self, fact):
        self._validate_deposit(fact)
        identity = self._deposit_identity(fact)
        if identity in self.state["deposits"]:
            if self.state["deposits"][identity] != fact:
                raise ModelError("CONTRADICTORY_DEPOSIT")
            return
        if len(self.state["deposits"]) >= self.limits["records"]:
            raise ModelError("DEPOSIT_RECORD_LIMIT")
        self._flat_add("custody", fact["asset"], fact["amount"])
        self._add("spot", fact["account"], fact["asset"], fact["amount"])
        self.state["deposits"][identity] = deepcopy(fact)

    def _settle(self, fact):
        required = {"receipt_id", "asset", "amount", "destination", "height",
                    "tx_index", "event_index", "verified"}
        if set(fact) != required or fact["verified"] is not True:
            raise ModelError("INVALID_SETTLEMENT_FACT")
        for k in ("receipt_id", "asset", "destination"):
            _id(fact[k])
        self._amount(fact["amount"], 1)
        for k in ("height", "tx_index", "event_index"):
            _integer(fact[k], self.limits["sequence"])
        rid = fact["receipt_id"]
        if rid in self.state["settled"]:
            if self.state["settled"][rid]["fact"] != fact:
                raise ModelError("CONTRADICTORY_SETTLEMENT")
            return
        pending = self.state["pending"].get(rid)
        if pending is None or any(pending[k] != fact[k] for k in ("asset", "amount", "destination")):
            raise ModelError("SETTLEMENT_DOES_NOT_MATCH_CLAIM")
        self._flat_add("custody", fact["asset"], -fact["amount"])
        self.state["settled"][rid] = {"claim": self.state["pending"].pop(rid), "fact": deepcopy(fact)}

    def _shape_action(self, a):
        if set(a) != {"profile_id", "account", "sequence", "kind", "params", "authorized"}:
            raise ModelError("BAD_ACTION_SHAPE")
        if a["authorized"] is not True:
            raise ModelError("UNAUTHENTICATED_ACTION")
        if a["profile_id"] != self.profile["profile_id"]:
            raise ModelError("WRONG_PROFILE")
        _id(a["account"])
        _integer(a["sequence"], self.limits["sequence"])
        schemas = {
            "OPEN": {"market", "side", "curve"},
            "REPLACE": {"order_id", "expected_revision", "curve"},
            "CANCEL": {"order_id", "expected_revision"},
            "WITHDRAW": {"asset", "amount", "destination"},
            "SPOT_TO_FUTURES": {"subaccount", "asset", "amount", "risk_config"},
            "FUTURES_TO_SPOT": {"subaccount", "asset", "amount", "risk_config"},
            "CLAIM_FN": {"seat", "asset", "amount", "destination", "authority"},
            "CLAIM_TREASURY": {"asset", "amount", "destination", "authority"},
        }
        if type(a["kind"]) is not str or a["kind"] not in schemas or type(a["params"]) is not dict:
            raise ModelError("UNSUPPORTED_ACTION")
        p = a["params"]
        if set(p) != schemas[a["kind"]]:
            raise ModelError("BAD_ACTION_PARAMS")
        for key in set(p) & {"market", "order_id", "asset", "destination", "subaccount", "risk_config", "seat", "authority"}:
            _id(p[key])
        if "amount" in p:
            self._amount(p["amount"], 1)
        if "expected_revision" in p:
            _integer(p["expected_revision"], self.limits["sequence"])
        if "curve" in p:
            if type(p["curve"]) is not list:
                raise ModelError("BAD_CURVE")
            for pair in p["curve"]:
                if type(pair) is not list or len(pair) != 2:
                    raise ModelError("BAD_CURVE")
                _integer(pair[0], self.limits["price"])
                _integer(pair[1], self.limits["lots"])
        if a["kind"] == "OPEN" and p["side"] not in ("BUY", "SELL"):
            raise ModelError("BAD_SIDE")
        protocol = a["account"] in self._protocol_accounts()
        if a["kind"] not in ("CLAIM_FN", "CLAIM_TREASURY") and protocol:
            raise ModelError("PROTOCOL_ACCOUNT_NOT_USER")
        if a["kind"] == "CLAIM_FN":
            if (p["seat"] not in self.seats or a["account"] != self.fn_account(p["seat"])
                    or p["authority"] != self.seats[p["seat"]]):
                raise ModelError("WRONG_HISTORICAL_AUTHORITY")
        if a["kind"] == "CLAIM_TREASURY" and (
                a["account"] != self.treasury_account() or p["authority"] != self.treasury_owner):
            raise ModelError("WRONG_TREASURY_AUTHORITY")

    def _curve(self, side, points):
        try:
            validate_curve(side, points, self.limits["price"], self.limits["lots"], self.limits["curve_points"])
        except ValueError as e:
            raise Refusal("INVALID_CURVE") from e
        # Bound interpolation work before admitting a persistent curve, not
        # only the final monetary result. Python itself never wraps integers.
        for left, right in zip(points, points[1:]):
            self._product(abs(right[1] - left[1]), right[0] - left[0])

    def _buy_bound(self, m, points):
        return buy_bound(points, m["quote_atoms_per_tick"],
                         max_intermediate=self.limits["intermediate"])

    def _bound(self, m, side, points, t, charged):
        if side == "SELL":
            return self._amount(self._product(max(q for _, q in points), m["base_atoms_per_lot"]))
        n = self._buy_bound(m, points)
        self._amount(n)
        self._amount(t + n)
        return self._amount(n + self._fee(t + n, ceil=True) - charged)

    def _order_action(self, a):
        p, account, kind = a["params"], a["account"], a["kind"]
        if kind == "OPEN":
            m = self.markets.get(p["market"])
            if m is None:
                raise Refusal("UNKNOWN_MARKET")
            self._curve(p["side"], p["curve"])
            if any(o["active"] and (o["account"], o["market"], o["side"]) ==
                   (account, p["market"], p["side"]) for o in self.state["orders"].values()):
                raise Refusal("ORDER_SLOT_OCCUPIED")
            if len(self.state["orders"]) >= self.limits["orders"]:
                raise Refusal("ORDER_LIMIT")
            reserve = self._bound(m, p["side"], p["curve"], 0, 0)
            asset = m["quote"] if p["side"] == "BUY" else m["base"]
            if self._get("spot", account, asset) < reserve:
                raise Refusal("INSUFFICIENT_AVAILABLE")
            oid = order_id(a)
            self._add("spot", account, asset, -reserve)
            self.state["orders"][oid] = {
                "order_id": oid, "account": account, "market": p["market"], "side": p["side"],
                "curve": deepcopy(p["curve"]), "revision": 0, "revision_filled": 0,
                "lifetime_filled": 0, "notional": 0, "charged": 0, "spent": 0,
                "bound": self._buy_bound(m, p["curve"]) if p["side"] == "BUY" else 0,
                "reservation": reserve, "active": True, "terminal": None}
            return {"order_id": oid}
        o = self.state["orders"].get(p["order_id"])
        if o is None or o["account"] != account:
            raise Refusal("NOT_OWN_ORDER")
        if not o["active"]:
            raise Refusal("ORDER_CLOSED")
        if o["revision"] != p["expected_revision"]:
            raise Refusal("STALE_REVISION")
        if kind == "CANCEL" or (p.get("curve") and all(q == 0 for _, q in p["curve"])):
            self._close(o, "CANCELLED")
            return {"order_id": o["order_id"]}
        self._curve(o["side"], p["curve"])
        if o["revision"] >= self.limits["sequence"]:
            raise Refusal("REVISION_EXHAUSTED")
        m = self.markets[o["market"]]
        new = self._bound(m, o["side"], p["curve"], o["notional"], o["charged"])
        asset = m["quote"] if o["side"] == "BUY" else m["base"]
        delta = new - o["reservation"]
        if delta > self._get("spot", account, asset):
            raise Refusal("INSUFFICIENT_AVAILABLE")
        self._add("spot", account, asset, -delta)
        o.update(curve=deepcopy(p["curve"]), revision=o["revision"] + 1,
                 revision_filled=0, spent=0, reservation=new,
                 bound=self._buy_bound(m, p["curve"]) if o["side"] == "BUY" else 0)
        return {"order_id": o["order_id"]}

    def _close(self, o, reason):
        m = self.markets[o["market"]]
        asset = m["quote"] if o["side"] == "BUY" else m["base"]
        self._add("spot", o["account"], asset, o["reservation"])
        o.update(reservation=0, active=False, terminal=reason)

    def _withdraw(self, a, capacities):
        p, account, kind = a["params"], a["account"], a["kind"]
        asset, amount = p["asset"], p["amount"]
        if asset not in self.assets:
            raise Refusal("UNKNOWN_ASSET")
        pending = sum(x["amount"] for x in self.state["pending"].values() if x["asset"] == asset)
        if asset not in capacities or pending + amount > capacities[asset]:
            raise Refusal("INSUFFICIENT_SYNTHETIC_CAPACITY")
        if len(self.state["pending"]) + len(self.state["settled"]) >= self.limits["records"]:
            raise Refusal("RECEIPT_LIMIT")
        source = "spot"
        source_id = account
        if kind == "CLAIM_FN":
            source, source_id = "fn", p["seat"]
        elif kind == "CLAIM_TREASURY":
            source, source_id = "treasury", self.treasury_owner
        available = (self.state["treasury"].get(asset, 0) if source == "treasury"
                     else self._get(source, source_id, asset))
        if available < amount:
            raise Refusal("INSUFFICIENT_AVAILABLE")
        if source == "treasury":
            self._flat_add("treasury", asset, -amount)
        else:
            self._add(source, source_id, asset, -amount)
        rid = _hash("TEST/RECEIPT/1", action_id(a))
        self.state["pending"][rid] = {"asset": asset, "amount": amount,
            "destination": p["destination"], "account": account,
            "source": source, "source_id": source_id}
        return {"receipt_id": rid}

    def _transfer(self, a, risk, allowances):
        p, account = a["params"], a["account"]
        sub, asset, amount = p["subaccount"], p["asset"], p["amount"]
        if self.subaccounts.get(sub) != account:
            raise Refusal("SUBACCOUNT_NOT_AUTHORIZED")
        r = risk.get(sub)
        if (r is None or r["config"] != p["risk_config"] or r["enabled"] is not True
                or r["asset"] != asset or r["status"] != "PASS"):
            raise Refusal("RISK_NOT_APPROVED")
        if a["kind"] == "SPOT_TO_FUTURES":
            if self._get("spot", account, asset) < amount:
                raise Refusal("INSUFFICIENT_AVAILABLE")
            self._add("spot", account, asset, -amount)
            self._add("futures", sub, asset, amount)
        else:
            if (self._get("futures", sub, asset) < amount
                    or amount > allowances.get(sub, 0)):
                raise Refusal("INSUFFICIENT_WITHDRAWABLE_CASH")
            self._add("futures", sub, asset, -amount)
            self._add("spot", account, asset, amount)
            allowances[sub] -= amount
        return {}

    def apply_batch(self, deposits=(), settlements=(), actions=(), capacities=None, risk=None, fault=None):
        if fault not in (None, "after_actions", "after_first_fill"):
            raise ModelError("UNKNOWN_FAULT_SEAM")
        draft = deepcopy(self)
        # Copy caller-owned inputs before validation/execution.
        try:
            report = draft._batch(deepcopy(list(deposits)), deepcopy(list(settlements)),
                                  deepcopy(list(actions)), deepcopy(capacities or {}),
                                  deepcopy(risk or {}), fault)
            draft.assert_invariants()
            if any(draft.state["fee_pool"].values()):
                raise ModelError("UNDISTRIBUTED_FEES")
        except (ValueError, KeyError, TypeError) as e:
            if isinstance(e, ModelError):
                raise
            raise ModelError("INVALID_CANDIDATE_INPUT") from e
        self.state = draft.state
        return deepcopy(report)

    def _batch(self, deposits, settlements, actions, capacities, risk, fault):
        canonical([deposits, settlements, actions, capacities, risk])
        if (len(actions) > self.limits["actions_per_batch"]
                or len(deposits) + len(settlements) > self.limits["facts_per_batch"]):
            raise ModelError("BATCH_LIMIT")
        for aid, value in capacities.items():
            if aid not in self.assets:
                raise ModelError("UNKNOWN_CAPACITY_ASSET")
            self._amount(value)
        for sub, r in risk.items():
            _id(sub)
            if set(r) != {"config", "enabled", "asset", "status", "withdrawable"}:
                raise ModelError("BAD_SYNTHETIC_RISK_FACT")
            _id(r["config"])
            if r["asset"] not in self.assets or type(r["enabled"]) is not bool:
                raise ModelError("BAD_SYNTHETIC_RISK_FACT")
            if r["status"] not in ("PASS", "DENY", "UNKNOWN", "NOT_DEFINED"):
                raise ModelError("BAD_SYNTHETIC_RISK_FACT")
            self._amount(r["withdrawable"])
        allowances = {sub: r["withdrawable"] for sub, r in risk.items()}
        for fact in sorted(settlements, key=lambda f: (f["height"], f["tx_index"], f["event_index"], f["receipt_id"])):
            self._settle(fact)
            self.assert_invariants()
        for fact in sorted(deposits, key=lambda f: (f["height"], f["tx_index"], f["output_index"], f["txid"])):
            self._credit(fact)
            self.assert_invariants()
        unique = {}
        for a in actions:
            self._shape_action(a)
            unique[action_id(a)] = a
        results, groups = {}, {}
        for aid, a in sorted(unique.items()):
            # Frozen completed evidence precedes unconsumed equivocation groups.
            if aid in self.state["outcomes"]:
                results[aid] = deepcopy(self.state["outcomes"][aid])
            elif a["sequence"] < self.state["next_sequence"].get(a["account"], 0):
                results[aid] = {"action_id": aid, "sequence": a["sequence"],
                                "status": "REJECTED", "code": "CONSUMED_SEQUENCE_CONFLICT"}
            else:
                groups.setdefault((a["account"], a["sequence"]), []).append((aid, a))
        for (account, seq), members in sorted(groups.items()):
            if len(members) > 1:
                for aid, _ in members:
                    results[aid] = {"action_id": aid, "sequence": seq,
                                    "status": "REJECTED", "code": "EQUIVOCATION"}
                continue
            aid, a = members[0]
            expected = self.state["next_sequence"].get(account, 0)
            if seq != expected or expected == self.limits["sequence"]:
                results[aid] = {"action_id": aid, "sequence": seq, "status": "REJECTED",
                                "code": "SEQUENCE_EXHAUSTED" if expected == self.limits["sequence"] else "BAD_SEQUENCE"}
                continue
            if len(self.state["outcomes"]) >= self.limits["records"]:
                raise ModelError("OUTCOME_RECORD_LIMIT")
            before, allowance_before = deepcopy(self.state), deepcopy(allowances)
            outcome = {"action_id": aid, "sequence": seq, "status": "ACCEPTED", "code": "OK"}
            try:
                if a["kind"] in ("OPEN", "REPLACE", "CANCEL"):
                    outcome.update(self._order_action(a))
                elif a["kind"] in ("WITHDRAW", "CLAIM_FN", "CLAIM_TREASURY"):
                    outcome.update(self._withdraw(a, capacities))
                else:
                    outcome.update(self._transfer(a, risk, allowances))
            except Refusal as e:
                self.state, allowances = before, allowance_before
                outcome.update(status="REJECTED", code=str(e))
            self.state["next_sequence"][account] = seq + 1
            self.state["outcomes"][aid] = deepcopy(outcome)
            self.state["instructions"][aid] = canonical(_semantic(a)).decode("ascii")
            results[aid] = outcome
            self.assert_invariants()
        if fault == "after_actions":
            raise ModelError("INJECTED_AFTER_ACTIONS")
        fills = []
        first = True
        for mid, m in sorted(self.markets.items()):
            orders = [o for o in self.state["orders"].values() if o["active"] and o["market"] == mid]
            result = auction(orders, max_intermediate=self.limits["intermediate"])
            fills.append({"market": mid, **result})
            if result["volume"] == 0:
                continue
            # Full market settlement is staged. All orders independently retain
            # their own reservation; an error rolls back the entire batch.
            for oid, lots in sorted(result["fills"].items()):
                if lots:
                    self._fill(self.state["orders"][oid], lots, result["price"])
                    if first and fault == "after_first_fill":
                        raise ModelError("INJECTED_AFTER_FIRST_FILL")
                    first = False
            self.assert_invariants()
        self._allocate()
        self.assert_invariants()
        return {"outcomes": dict(sorted(results.items())), "fills": fills}

    def _fill(self, o, lots, price):
        m = self.markets[o["market"]]
        if not 0 < lots <= max(0, evaluate(o["curve"], price,
                              max_intermediate=self.limits["intermediate"]) - o["revision_filled"]):
            raise ModelError("INVALID_FILL")
        base = self._amount(self._product(lots, m["base_atoms_per_lot"]))
        quote = self._amount(self._product(lots, price, m["quote_atoms_per_tick"]))
        total = self._amount(o["notional"] + quote)
        fee = self._fee(total) - o["charged"]
        if fee < 0 or fee > quote:
            raise ModelError("FEE_EXCEEDS_PROCEEDS")
        if o["side"] == "BUY":
            if quote > o["bound"] - o["spent"] or quote + fee > o["reservation"]:
                raise ModelError("UNBACKED_FILL")
            o["reservation"] -= quote + fee
            o["spent"] += quote
            self._add("spot", o["account"], m["base"], base)
        else:
            if base > o["reservation"]:
                raise ModelError("UNBACKED_FILL")
            o["reservation"] -= base
            self._add("spot", o["account"], m["quote"], quote - fee)
        self._flat_add("fee_pool", m["quote"], fee)
        self._flat_add("fees_collected", m["quote"], fee)
        o["notional"], o["charged"] = total, o["charged"] + fee
        o["revision_filled"] += lots
        o["lifetime_filled"] = self._amount(o["lifetime_filled"] + lots)
        if o["revision_filled"] == max(q for _, q in o["curve"]):
            self._close(o, "FILLED")

    def _allocate(self):
        for asset, amount in sorted(self.state["fee_pool"].items()):
            treasury = self._product(amount, self.profile["treasury_percent"]) // 100
            fn = amount - treasury
            share, extra = divmod(fn, len(self.seats))
            self._flat_add("treasury", asset, treasury)
            for index, seat in enumerate(sorted(self.seats)):
                self._add("fn", seat, asset, share + (index < extra))
            self.state["fee_pool"][asset] = 0

    def assert_invariants(self):
        s = self.state
        expected_keys = {"spot", "futures", "custody", "orders", "next_sequence", "outcomes",
            "instructions", "deposits", "pending", "settled", "fn", "treasury", "fee_pool",
            "fees_collected", "v1", "v1_receipts", "v1_external"}
        if set(s) != expected_keys:
            raise ModelError("STATE_SCHEMA")
        if any(type(v) is not dict for v in s.values()):
            raise ModelError("STATE_BUCKET_SCHEMA")
        for bucket in ("spot", "futures", "next_sequence", "outcomes", "instructions", "deposits",
                       "v1", "v1_receipts", "v1_external"):
            if len(s[bucket]) > self.limits["records"]:
                raise ModelError("STATE_RECORD_LIMIT")
        if len(s["orders"]) > self.limits["orders"] or len(s["pending"]) + len(s["settled"]) > self.limits["records"]:
            raise ModelError("STATE_RECORD_LIMIT")
        for bucket in ("custody", "treasury", "fee_pool", "fees_collected"):
            if any(a not in self.assets for a in s[bucket]):
                raise ModelError("UNKNOWN_BALANCE_ASSET")
        for did, fact in s["deposits"].items():
            self._validate_deposit(fact)
            if did != self._deposit_identity(fact):
                raise ModelError("DEPOSIT_IDENTITY")
        if set(s["instructions"]) != set(s["outcomes"]):
            raise ModelError("OUTCOME_HISTORY")
        consumed, accepted_claims, opened_orders = {}, {}, {}
        for aid, text in s["instructions"].items():
            semantic = json.loads(text)
            self._shape_action({**semantic, "authorized": True})
            if _hash("TEST/INSTRUCTION/1", semantic) != aid or canonical(semantic).decode("ascii") != text:
                raise ModelError("INSTRUCTION_IDENTITY")
            key = (semantic["account"], semantic["sequence"])
            outcome = s["outcomes"][aid]
            if key in consumed or outcome.get("action_id") != aid or outcome.get("sequence") != key[1]:
                raise ModelError("DOUBLE_SEQUENCE_OR_OUTCOME_IDENTITY")
            if outcome.get("status") not in ("ACCEPTED", "REJECTED"):
                raise ModelError("INVALID_STORED_OUTCOME")
            if type(outcome.get("code")) is not str or not outcome["code"]:
                raise ModelError("INVALID_STORED_OUTCOME")
            if (outcome["status"] == "ACCEPTED") != (outcome["code"] == "OK"):
                raise ModelError("INVALID_STORED_OUTCOME")
            consumed[key] = aid
            fields = {"action_id", "sequence", "status", "code"}
            if outcome["status"] == "ACCEPTED":
                kind, p = semantic["kind"], semantic["params"]
                if kind in ("OPEN", "REPLACE", "CANCEL"):
                    fields.add("order_id")
                    oid = order_id(semantic) if kind == "OPEN" else p["order_id"]
                    if outcome.get("order_id") != oid:
                        raise ModelError("OUTCOME_ORDER_IDENTITY")
                    if kind == "OPEN":
                        opened_orders[oid] = semantic
                if kind in ("WITHDRAW", "CLAIM_FN", "CLAIM_TREASURY"):
                    fields.add("receipt_id")
                    rid = _hash("TEST/RECEIPT/1", aid)
                    if outcome.get("receipt_id") != rid:
                        raise ModelError("OUTCOME_RECEIPT_IDENTITY")
                    source, owner = "spot", semantic["account"]
                    if kind == "CLAIM_FN":
                        source, owner = "fn", p["seat"]
                    elif kind == "CLAIM_TREASURY":
                        source, owner = "treasury", self.treasury_owner
                    accepted_claims[rid] = {"asset": p["asset"], "amount": p["amount"],
                        "destination": p["destination"], "account": semantic["account"],
                        "source": source, "source_id": owner}
            if set(outcome) != fields:
                raise ModelError("OUTCOME_SCHEMA")
        if set(accepted_claims) != set(s["pending"]) | set(s["settled"]):
            raise ModelError("CLAIM_HISTORY")
        for rid, claim in s["pending"].items():
            if claim != accepted_claims[rid]:
                raise ModelError("CLAIM_INSTRUCTION_MISMATCH")
        for rid, record in s["settled"].items():
            if set(record) != {"claim", "fact"} or record["claim"] != accepted_claims[rid]:
                raise ModelError("SETTLED_CLAIM_HISTORY")
            fact = record["fact"]
            if set(fact) != {"receipt_id", "asset", "amount", "destination", "height", "tx_index", "event_index", "verified"}:
                raise ModelError("SETTLEMENT_SCHEMA")
            if fact["receipt_id"] != rid or fact["verified"] is not True:
                raise ModelError("SETTLEMENT_IDENTITY")
            if any(fact[k] != record["claim"][k] for k in ("asset", "amount", "destination")):
                raise ModelError("SETTLEMENT_CLAIM_MISMATCH")
            for k in ("height", "tx_index", "event_index"):
                _integer(fact[k], self.limits["sequence"])
        if set(opened_orders) != set(s["orders"]):
            raise ModelError("ORDER_OPEN_HISTORY")
        totals = {a: 0 for a in self.assets}
        for bucket in ("spot", "futures", "fn"):
            for owner, balances in s[bucket].items():
                _id(owner)
                if bucket == "futures" and owner not in self.subaccounts:
                    raise ModelError("UNKNOWN_FUTURES_SUBACCOUNT")
                if bucket == "fn" and owner not in self.seats:
                    raise ModelError("UNKNOWN_REWARD_SEAT")
                if bucket == "spot" and owner in self._protocol_accounts():
                    raise ModelError("PROTOCOL_SPOT_ALIAS")
                for asset, value in balances.items():
                    if asset not in totals:
                        raise ModelError("UNKNOWN_BALANCE_ASSET")
                    self._amount(value)
                    totals[asset] += value
        slots = set()
        for oid, o in s["orders"].items():
            if oid != o["order_id"] or o["market"] not in self.markets:
                raise ModelError("ORDER_IDENTITY")
            _id(oid)
            _id(o["account"])
            opening = opened_orders[oid]
            if (o["account"] != opening["account"] or o["market"] != opening["params"]["market"]
                    or o["side"] != opening["params"]["side"] or type(o["active"]) is not bool):
                raise ModelError("ORDER_OPEN_IDENTITY")
            m = self.markets[o["market"]]
            try:
                self._curve(o["side"], o["curve"])
            except Refusal as e:
                raise ModelError("INVALID_STORED_CURVE") from e
            for k in ("revision", "revision_filled", "lifetime_filled", "notional", "charged", "spent", "bound", "reservation"):
                self._amount(o[k])
            if o["charged"] != self._fee(o["notional"]) or o["revision_filled"] > max(q for _, q in o["curve"]):
                raise ModelError("ORDER_COUNTERS")
            if o["lifetime_filled"] < o["revision_filled"]:
                raise ModelError("LIFETIME_COUNTER")
            if o["active"]:
                key = (o["account"], o["market"], o["side"])
                if key in slots or o["terminal"] is not None:
                    raise ModelError("DUPLICATE_ORDER_SLOT")
                slots.add(key)
                if o["side"] == "BUY":
                    if o["bound"] != self._buy_bound(m, o["curve"]):
                        raise ModelError("BAD_PERSISTENT_BOUND")
                    remaining = o["bound"] - o["spent"]
                    expected = remaining + self._fee(o["notional"] + remaining, ceil=True) - o["charged"]
                    if remaining < 0 or o["reservation"] != expected:
                        raise ModelError("BAD_RESERVATION")
                    totals[m["quote"]] += o["reservation"]
                else:
                    expected = (max(q for _, q in o["curve"]) - o["revision_filled"]) * m["base_atoms_per_lot"]
                    if expected != o["reservation"]:
                        raise ModelError("BAD_RESERVATION")
                    totals[m["base"]] += o["reservation"]
            elif o["reservation"] != 0 or o["terminal"] not in ("CANCELLED", "FILLED"):
                raise ModelError("TERMINAL_RESERVATION")
        for bucket in ("treasury", "fee_pool"):
            for asset, value in s[bucket].items():
                if asset not in self.profile["stable_fee_assets"]:
                    raise ModelError("WRONG_FEE_ASSET")
                self._amount(value)
                totals[asset] += value
        for rid, claim in s["pending"].items():
            _id(rid)
            if rid in s["settled"] or claim["asset"] not in totals:
                raise ModelError("RECEIPT_REPLAY")
            self._amount(claim["amount"], 1)
            totals[claim["asset"]] += claim["amount"]
        for asset in self.assets:
            deposited = sum(f["amount"] for f in s["deposits"].values() if f["asset"] == asset)
            paid = sum(f["claim"]["amount"] for f in s["settled"].values() if f["claim"]["asset"] == asset)
            custody = s["custody"].get(asset, 0)
            self._amount(custody)
            if totals[asset] != custody or custody != deposited - paid:
                raise ModelError("ASSET_CONSERVATION")
            earned = sum(o["charged"] for o in s["orders"].values() if self.markets[o["market"]]["quote"] == asset)
            protocol = s["treasury"].get(asset, 0) + s["fee_pool"].get(asset, 0)
            protocol += sum(b.get(asset, 0) for b in s["fn"].values())
            protocol += sum(c["amount"] for c in s["pending"].values() if c["asset"] == asset and c["source"] != "spot")
            protocol += sum(f["claim"]["amount"] for f in s["settled"].values() if f["claim"]["asset"] == asset and f["claim"]["source"] != "spot")
            if earned != s["fees_collected"].get(asset, 0) or earned != protocol:
                raise ModelError("FEE_CONSERVATION")
        for account, seq in s["next_sequence"].items():
            _integer(seq, self.limits["sequence"])
            values = sorted(q for a, q in consumed if a == account)
            if len(values) != seq or any(i != q for i, q in enumerate(values)):
                raise ModelError("SEQUENCE_HISTORY_GAP")
        for account, _ in consumed:
            if account not in s["next_sequence"]:
                raise ModelError("MISSING_HIGH_WATER")
        for key, v in s["v1"].items():
            if set(v) != {"asset", "vault", "initial", "custody", "available", "reserved", "pending"}:
                raise ModelError("V1_SCHEMA")
            if v["asset"] not in self.assets or key != _hash("TEST/V1-VAULT/1", [_id(v["vault"]), v["asset"]]):
                raise ModelError("V1_IDENTITY")
            for k in ("initial", "custody", "available", "reserved", "pending"):
                self._amount(v[k])
            if v["custody"] != v["available"] + v["reserved"] + v["pending"]:
                raise ModelError("V1_CONSERVATION")
            external = sum(e["remaining"] for e in s["v1_external"].values() if e["key"] == key)
            redeposited = sum(e["amount"] for e in s["v1_external"].values() if e["key"] == key and e["deposit_id"] is not None)
            if v["initial"] != v["custody"] + external + redeposited:
                raise ModelError("MIGRATION_CONSERVATION")
        linked = set()
        if set(s["v1_receipts"]) != set(s["v1_external"]):
            raise ModelError("V1_RECEIPT_IDENTITY")
        for rid, e in s["v1_external"].items():
            _id(rid)
            if set(e) != {"key", "asset", "amount", "destination", "remaining", "deposit_id"}:
                raise ModelError("V1_EXTERNAL_SCHEMA")
            if rid not in s["v1_receipts"] or e["key"] not in s["v1"]:
                raise ModelError("V1_RECEIPT_IDENTITY")
            expected = {k: e[k] for k in ("key", "asset", "amount", "destination")}
            if s["v1_receipts"][rid] != expected or e["asset"] != s["v1"][e["key"]]["asset"]:
                raise ModelError("V1_RECEIPT_MISMATCH")
            _id(e["destination"])
            self._amount(e["amount"], 1)
            self._amount(e["remaining"])
            if e["deposit_id"] is None and e["remaining"] != e["amount"]:
                raise ModelError("V1_EXTERNAL_FUNDING")
            if e["deposit_id"] is not None:
                if e["deposit_id"] in linked:
                    raise ModelError("DUPLICATE_MIGRATION_CREDIT")
                linked.add(e["deposit_id"])
                f = s["deposits"].get(e["deposit_id"])
                if f is None or e["remaining"] != 0 or any(f[k] != e[k] for k in ("amount", "asset")) or f["account"] != e["destination"]:
                    raise ModelError("V1_V2_CREDIT_LINK")

    def seed_v1(self, vault, asset, available, reserved, pending):
        _id(vault)
        if asset not in self.assets:
            raise ModelError("UNKNOWN_ASSET")
        for n in (available, reserved, pending):
            self._amount(n)
        total = self._amount(available + reserved + pending)
        key = _hash("TEST/V1-VAULT/1", [vault, asset])
        if key in self.state["v1"]:
            raise ModelError("V1_FIXTURE_ALREADY_SEEDED")
        if len(self.state["v1"]) >= self.limits["records"]:
            raise ModelError("V1_RECORD_LIMIT")
        self.state["v1"][key] = {"asset": asset, "vault": vault, "initial": total,
            "custody": total, "available": available, "reserved": reserved, "pending": pending}
        self.assert_invariants()

    def settle_v1(self, vault, asset, receipt_id, amount, destination):
        _id(receipt_id)
        _id(destination)
        self._amount(amount, 1)
        key = _hash("TEST/V1-VAULT/1", [_id(vault), _id(asset)])
        record = {"key": key, "asset": asset, "amount": amount, "destination": destination}
        if receipt_id in self.state["v1_receipts"]:
            if self.state["v1_receipts"][receipt_id] != record:
                raise ModelError("CONFLICTING_V1_RECEIPT")
            return
        if len(self.state["v1_receipts"]) >= self.limits["records"]:
            raise ModelError("V1_RECEIPT_LIMIT")
        draft = deepcopy(self)
        v = draft.state["v1"].get(key)
        if v is None or v["pending"] < amount:
            raise ModelError("V1_PENDING_INSUFFICIENT")
        v["pending"] -= amount
        v["custody"] -= amount
        draft.state["v1_receipts"][receipt_id] = record
        draft.state["v1_external"][receipt_id] = {**record, "remaining": amount, "deposit_id": None}
        draft.assert_invariants()
        self.state = draft.state

    def redeposit_v1(self, receipt_id, deposit_fact):
        self._validate_deposit(deposit_fact)
        _id(receipt_id)
        e = self.state["v1_external"].get(receipt_id)
        did = self._deposit_identity(deposit_fact)
        if e is None:
            raise ModelError("NO_SETTLED_V1_FUNDING")
        if e["deposit_id"] == did and self.state["deposits"].get(did) == deposit_fact:
            return
        if (e["deposit_id"] is not None or did in self.state["deposits"]
                or e["remaining"] != deposit_fact["amount"] or e["asset"] != deposit_fact["asset"]
                or e["destination"] != deposit_fact["account"]):
            raise ModelError("MIGRATION_FUNDING_MISMATCH")
        draft = deepcopy(self)
        draft._credit(deposit_fact)
        draft.state["v1_external"][receipt_id].update(remaining=0, deposit_id=did)
        draft.assert_invariants()
        self.state = draft.state

    def snapshot(self):
        return canonical({"format": "TEST-MODEL-SNAPSHOT/1", "profile": self.profile,
            "assets": self.assets, "markets": sorted(self.market_inputs, key=canonical), "seats": self.seats,
            "treasury_owner": self.treasury_owner, "subaccounts": self.subaccounts, "state": self.state})

    def digest(self):
        return hashlib.sha256(self.snapshot()).hexdigest()

    @classmethod
    def restore(cls, encoded):
        try:
            document = json.loads(encoded)
            if canonical(document) != encoded or document["format"] != "TEST-MODEL-SNAPSHOT/1":
                raise ModelError("NON_CANONICAL_SNAPSHOT")
            if set(document) != {"format", "profile", "assets", "markets", "seats", "treasury_owner", "subaccounts", "state"}:
                raise ModelError("SNAPSHOT_SCHEMA")
            model = cls(*(document[k] for k in ("profile", "assets", "markets", "seats", "treasury_owner", "subaccounts")))
            model.state = deepcopy(document["state"])
            model.assert_invariants()
            if any(model.state["fee_pool"].values()):
                raise ModelError("PARTIAL_BATCH_SNAPSHOT")
            return model
        except (ValueError, KeyError, TypeError) as e:
            if isinstance(e, ModelError):
                raise
            raise ModelError("INVALID_SNAPSHOT") from e
