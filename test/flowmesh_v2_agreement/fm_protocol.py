"""Test-only authenticated proof vocabulary. No keys, sockets or production codec.

Authentication is an ideal capability oracle. The adversary can replay a minted
message, or mint as an explicitly Byzantine identity, but cannot forge an honest
identity. The oracle's audit log is for the external checker, not replica logic.
"""
from copy import deepcopy
import json
from pathlib import Path

from fm_application import canonical, digest


PROFILE = json.loads(Path(__file__).with_name("TEST_PROFILE.json").read_text())


class Invalid(ValueError):
    pass


class Exhausted(Invalid):
    pass


def integer(value, maximum, minimum=0):
    if type(value) is not int or not minimum <= value <= maximum:
        raise Invalid("INTEGER_BOUND")
    return value


def exact_id(value):
    if type(value) is not str or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
        raise Invalid("EXACT_ID")
    return value


def proposer(instance, view, n):
    return (instance["sequence"] + view) % n


def message(phase, sender, instance, view, value=None, **extra):
    return {"phase": phase, "sender": sender, "instance": deepcopy(instance),
            "view": view, "value": value, **deepcopy(extra)}


class Authentication:
    """Trusted simulator facility. Only bounded audit/checker APIs see issued."""
    def __init__(self, n, byzantine=()):
        if n not in (4, 7):
            raise Invalid("FIXED_TEST_SET_ONLY")
        self.n = n
        self.byzantine = frozenset(byzantine)
        if any(type(i) is not int or not 0 <= i < n for i in self.byzantine):
            raise Invalid("UNKNOWN_BYZANTINE_ID")
        if len(self.byzantine) > (n - 1) // 3:
            raise Invalid("FAULT_BUDGET")
        self.issued = []
        self._minted = {}
        self._caps = {i: object() for i in range(n)}

    def signer(self, index):
        cap = self._caps[index]
        def sign(payload):
            if payload.get("sender") != index:
                raise Invalid("CAPABILITY_IDENTITY")
            return self._issue(cap, payload)
        return sign

    def _issue(self, cap, payload):
        sender = payload.get("sender")
        if self._caps.get(sender) is not cap:
            raise Invalid("NO_SIGNING_CAPABILITY")
        raw = canonical(payload)
        if len(raw) > PROFILE["limits"]["proof_bytes"]:
            raise Exhausted("MESSAGE_BYTES")
        token = digest("SYNTHETIC/AUTH/1", payload)
        if token not in self._minted:
            if len(self.issued) >= PROFILE["limits"]["messages"]:
                raise Exhausted("AUTH_AUDIT_LIMIT")
            signed = {"payload": deepcopy(payload), "auth": token}
            self._minted[token] = signed
            self.issued.append(deepcopy(signed))
        return deepcopy(self._minted[token])

    def adversary_sign(self, payload):
        if payload.get("sender") not in self.byzantine:
            raise Invalid("ADVERSARY_CANNOT_FORGE_HONEST")
        return self._issue(self._caps[payload["sender"]], payload)

    def verify(self, signed):
        return (type(signed) is dict and set(signed) == {"payload", "auth"}
                and type(signed["auth"]) is str
                and self._minted.get(signed["auth"]) == signed)


class Proofs:
    """Pure structural/authentication verifier; application data is separate."""
    def __init__(self, n, authenticate):
        self.n, self.q = n, 2 * ((n - 1) // 3) + 1
        self.authenticate = authenticate
        self._work = 0

    def check(self, kind, obj, instance):
        try:
            if len(canonical(obj)) > PROFILE["limits"]["proof_bytes"]:
                raise Exhausted("PROOF_BYTES")
            self._work = 0
            return getattr(self, "_" + kind)(obj, instance, 0)
        except (KeyError, TypeError, RecursionError, AttributeError) as exc:
            raise Invalid("MALFORMED_PROOF") from exc

    def _signed(self, signed, phase, instance, depth):
        self._work += 1
        if self._work > PROFILE["limits"]["proof_work"] or depth > PROFILE["limits"]["proof_depth"]:
            raise Exhausted("PROOF_WORK")
        if not self.authenticate(signed):
            raise Invalid("AUTHENTICATION")
        p = signed["payload"]
        extras = {"PROPOSE": {"new_view"}, "PREPARE": set(), "COMMIT": set(),
                  "VIEW_CHANGE": {"prepared", "decision"}, "NEW_VIEW": {"reports"}}
        if phase not in extras or set(p) != {"phase", "sender", "instance", "view", "value"} | extras[phase]:
            raise Invalid("MESSAGE_SHAPE")
        if p["phase"] != phase or canonical(p["instance"]) != canonical(instance):
            raise Invalid("PHASE_OR_INSTANCE")
        integer(p["sender"], self.n - 1)
        integer(p["view"], PROFILE["limits"]["views"] - 1)
        if phase == "VIEW_CHANGE":
            if p["value"] is not None or p["view"] == 0:
                raise Invalid("VIEW_CHANGE_VALUE")
        else:
            exact_id(p["value"])
        return p

    def _proposal(self, obj, instance, depth):
        p = self._signed(obj, "PROPOSE", instance, depth)
        if p["sender"] != proposer(instance, p["view"], self.n):
            raise Invalid("WRONG_PROPOSER")
        if p["view"] == 0:
            if p["new_view"] is not None:
                raise Invalid("UNEXPECTED_NEW_VIEW")
        else:
            nv = self._new_view(p["new_view"], instance, depth + 1)
            if (nv["view"], nv["value"]) != (p["view"], p["value"]):
                raise Invalid("NEW_VIEW_LINK")
        return p

    def _votes(self, votes, phase, instance, view, value, depth):
        if type(votes) is not list or not self.q <= len(votes) <= self.n:
            raise Invalid("QUORUM_COUNT")
        seen = []
        for vote in votes:
            p = self._signed(vote, phase, instance, depth)
            if p["view"] != view or p["value"] != value:
                raise Invalid("VOTE_LINK")
            seen.append(p["sender"])
        if seen != sorted(set(seen)):
            raise Invalid("DUPLICATE_OR_UNSORTED_SIGNERS")

    def _prepared(self, obj, instance, depth):
        if type(obj) is not dict or set(obj) != {"proposal", "prepares"}:
            raise Invalid("PREPARED_SHAPE")
        p = self._proposal(obj["proposal"], instance, depth + 1)
        self._votes(obj["prepares"], "PREPARE", instance, p["view"], p["value"], depth + 1)
        return p

    def _commit(self, obj, instance, depth):
        if type(obj) is not dict or set(obj) != {"prepared", "commits"}:
            raise Invalid("COMMIT_SHAPE")
        p = self._prepared(obj["prepared"], instance, depth + 1)
        self._votes(obj["commits"], "COMMIT", instance, p["view"], p["value"], depth + 1)
        return p

    def _view_change(self, obj, instance, depth):
        p = self._signed(obj, "VIEW_CHANGE", instance, depth)
        if p["prepared"] is not None:
            old = self._prepared(p["prepared"], instance, depth + 1)
            if old["view"] >= p["view"]:
                raise Invalid("PREPARATION_NOT_BEFORE_TARGET")
        if p["decision"] is not None:
            old = self._commit(p["decision"], instance, depth + 1)
            if old["view"] >= p["view"]:
                raise Invalid("DECISION_NOT_BEFORE_TARGET")
        return p

    def _new_view(self, obj, instance, depth):
        p = self._signed(obj, "NEW_VIEW", instance, depth)
        if p["view"] == 0 or p["sender"] != proposer(instance, p["view"], self.n):
            raise Invalid("NEW_VIEW_PROPOSER")
        reports = p["reports"]
        if type(reports) is not list or not self.q <= len(reports) <= self.n:
            raise Invalid("REPORT_QUORUM")
        seen, choices = [], []
        for report in reports:
            r = self._view_change(report, instance, depth + 1)
            if r["view"] != p["view"]:
                raise Invalid("REPORT_TARGET")
            seen.append(r["sender"])
            if r["decision"] is not None:
                raise Invalid("REPORT_REQUIRES_DECISION_APPLICATION")
            if r["prepared"] is not None:
                old = r["prepared"]["proposal"]["payload"]
                choices.append((old["view"], old["value"]))
        if seen != sorted(set(seen)):
            raise Invalid("REPORT_IDENTITIES")
        if choices:
            highest = max(v for v, _ in choices)
            selected = {x for v, x in choices if v == highest}
            if len(selected) != 1:
                raise Invalid("CONFLICTING_PREPARED_PROOFS")
            if p["value"] not in selected:
                raise Invalid("HIGHEST_PREPARED_SELECTION")
        return p

    def _prepare_vote(self, obj, instance, depth):
        return self._signed(obj, "PREPARE", instance, depth)

    def _commit_vote(self, obj, instance, depth):
        return self._signed(obj, "COMMIT", instance, depth)
