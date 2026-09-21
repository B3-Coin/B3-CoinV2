"""TEST-only disk acknowledgement adapter for the unchanged agreement replica.

Recovery validation is an input check, not an independent safety audit. It
reuses the model's proof and accounting validators. Volatile anchor ancestry
is deliberately not a condition for opening a valid protected journal; normal
Replica recovery still obtains it through GET/DATA before signing or applying.
"""
from copy import deepcopy

from fm_application import Application, NeedData, canonical, value_id
from fm_memory import update_durable
from fm_protocol import PROFILE, exact_id, integer, proposer
from fm_replica import Replica
from fm_disk_store import DiskStore, StorageError


_STATE = {"sequence", "snapshot", "anchor", "parent", "records", "retained_bodies",
          "fenced", "halt"}
_RECORD = {"instance", "view", "mode", "accepted", "new_views", "prepared",
           "highest", "intents", "intent_bodies", "signed", "decision", "body",
           "applied", "apply_count", "before", "anchor_before"}
_INSTANCE = {"profile", "domain", "config", "epoch", "set", "sequence", "parent"}
_EXTRAS = {"PROPOSE": {"new_view"}, "PREPARE": set(), "COMMIT": set(),
           "VIEW_CHANGE": {"prepared", "decision"}, "NEW_VIEW": {"reports"}}
_KINDS = {"PROPOSE": "proposal", "PREPARE": "prepare_vote", "COMMIT": "commit_vote",
          "VIEW_CHANGE": "view_change", "NEW_VIEW": "new_view"}


def _require(condition):
    if not condition:
        raise ValueError("RECOVERY_EVIDENCE_INVALID")


def _mapping(value, fields=None):
    _require(type(value) is dict and (fields is None or set(value) == fields))


class DiskReplica(Replica):
    """A generated test identity owns one locked store for this process lifetime."""

    def __init__(self, index, n, signer, authenticate, snapshot, anchor, emit, clock,
                 trace, *, directory, create=False, trusted_head=None, fault=None,
                 cut_hook=None):
        self.store = None
        self.storage_error = None
        self._closed = False
        self.cut_hook = cut_hook
        super().__init__(index, n, signer, authenticate, snapshot, anchor, emit, clock, trace)
        self.initial_snapshot = snapshot
        self.initial_anchor = deepcopy(anchor)
        self.genesis = self.d["parent"]
        identity = {"config": self.config, "index": index, "n": n, "genesis": self.genesis}
        try:
            self.store = DiskStore(directory, identity, initial=self.d if create else None,
                                   trusted_head=trusted_head, fault=fault)
            loaded = self.store.load()
            self._validate_recovery(loaded)
            self.d = loaded
            self._volatile()
        except StorageError as exc:
            self._poison(exc)
            if self.store is not None:
                self.store.close()
            raise
        # A kill immediately after SQL COMMIT can precede the runtime callback.
        # These are observations of signatures present now, never reconstructed
        # pre_sign guards or claims about a callback in a previous process.
        for rec in self.d["records"].values():
            for (phase, _), signed in rec["signed"].items():
                self.event("durable", reason="signature:" + phase,
                           auth=signed["auth"], recovered_from_disk=True)

    def _poison(self, exc):
        self.storage_error = exc.code
        self.alive = False
        self.last_reason = exc.code

    def _available(self):
        if self.storage_error is not None or self._closed:
            raise StorageError(self.storage_error or "STORAGE_CLOSED")

    def _persist(self, mutate, reason, **evidence):
        try:
            self._available()
            candidate, stats = update_durable(
                self.d, mutate,
                before_commit=lambda old, new: self.store.commit(old, new, reason))
        except StorageError as exc:
            self._poison(exc)
            raise
        self.d, self.last_memory_work = candidate, stats
        self.event("durable", reason=reason, memory_work=stats, **evidence)

    def _crash_at(self, point, phase=""):
        if self.cut_hook is not None:
            self.cut_hook(point, phase)
        super()._crash_at(point, phase)

    def restart(self, suspected_rollback=False):
        self._available()
        return super().restart(suspected_rollback)

    def _send(self, kind, data, destination=None):
        self._available()
        return super()._send(kind, data, destination)

    def close(self):
        self.alive = False
        self._closed = True
        if self.store is not None:
            self.store.close()

    def _body_evidence(self, body, rec, expected_value=None):
        """Check accounting even when the volatile ancestry chain is absent."""
        _mapping(body, {"instance", "anchor", "batch", "result"})
        app = Application(rec["before"])
        # build uses the original shape, header, normalized-batch, fact-height,
        # accounting and result rules; no substitute accounting path is added.
        expected = app.build(rec["instance"], body["anchor"], body["batch"])
        _require(canonical(body) == canonical(expected))
        if expected_value is not None:
            _require(value_id(body) == expected_value)
        known = dict(self._recovery_anchors)
        known[body["anchor"]["hash"]] = body["anchor"]
        try:
            return app.validate(body, rec["instance"], rec["anchor_before"],
                                known)
        except NeedData:
            # A header gap is not permission to erase a signed obligation.
            # Application/signing must independently revalidate after GET/DATA.
            return app.preview(body["batch"])

    def _slot(self, slot):
        _require(type(slot) is tuple and len(slot) == 2)
        phase, view = slot
        _require(type(phase) is str and phase in _EXTRAS)
        integer(view, PROFILE["limits"]["views"] - 1)
        return phase, view

    def _intent(self, payload, slot, rec):
        phase, view = self._slot(slot)
        _mapping(payload, {"phase", "sender", "instance", "view", "value"} | _EXTRAS[phase])
        _require(payload["phase"] == phase and type(payload["sender"]) is int
                 and payload["sender"] == self.index and type(payload["view"]) is int
                 and payload["view"] == view and view <= rec["view"]
                 and canonical(payload["instance"]) == canonical(rec["instance"]))
        _require(len(canonical(payload)) <= PROFILE["limits"]["proof_bytes"])
        value, instance = payload["value"], rec["instance"]
        if phase == "VIEW_CHANGE":
            _require(view > 0 and value is None and payload["decision"] is None)
            if payload["prepared"] is not None:
                old = self.proofs.check("prepared", payload["prepared"], instance)
                _require(old["view"] < view
                         and rec["prepared"].get((old["view"], old["value"])) == payload["prepared"])
            # An existing local COMMIT cannot be omitted from a later report.
            committed = [v for (p, v) in rec["intents"] if p == "COMMIT" and v < view]
            if committed:
                _require(payload["prepared"] is not None
                         and payload["prepared"]["proposal"]["payload"]["view"] >= max(committed))
            return

        exact_id(value)
        _require(value in rec["intent_bodies"])
        if phase in ("PREPARE", "COMMIT"):
            accepted = rec["accepted"].get(view)
            _require(accepted is not None and accepted["signed"]["payload"]["value"] == value
                     and accepted["body"] == rec["intent_bodies"][value])
            if phase == "COMMIT":
                _require((view, value) in rec["prepared"])
        elif phase == "PROPOSE":
            _require(self.index == proposer(instance, view, self.n))
            if view:
                _require(payload["new_view"] is not None
                         and rec["new_views"].get(view) == payload["new_view"])
                p = self.proofs.check("new_view", payload["new_view"], instance)
                _require((p["view"], p["value"]) == (view, value))
            else:
                _require(payload["new_view"] is None)
        elif phase == "NEW_VIEW":
            _require(view > 0 and self.index == proposer(instance, view, self.n))
            reports = payload["reports"]
            _require(type(reports) is list and self.q <= len(reports) <= self.n)
            seen, prepared = [], []
            for signed in reports:
                p = self.proofs.check("view_change", signed, instance)
                _require(p["view"] == view and p["decision"] is None)
                seen.append(p["sender"])
                if p["prepared"] is not None:
                    prepared.append(p["prepared"]["proposal"]["payload"])
            _require(seen == sorted(set(seen)))
            if prepared:
                highest = max(p["view"] for p in prepared)
                _require({p["value"] for p in prepared if p["view"] == highest} == {value})

    def _record_evidence(self, rec):
        instance = rec["instance"]
        integer(rec["view"], PROFILE["limits"]["views"] - 1)
        _require(rec["mode"] in ("ACTIVE", "CHANGING", "DECIDED"))
        for field in ("accepted", "new_views", "prepared", "intents", "intent_bodies", "signed"):
            _mapping(rec[field])
        _require(type(rec["applied"]) is bool and type(rec["apply_count"]) is int
                 and rec["apply_count"] == int(rec["applied"]))
        _require((rec["mode"] == "DECIDED") == (rec["decision"] is not None))
        if rec["mode"] == "CHANGING":
            _require(rec["view"] > 0 and rec["view"] not in rec["new_views"])
        if rec["mode"] == "ACTIVE" and rec["view"]:
            _require(rec["view"] in rec["new_views"])
        for view, accepted in rec["accepted"].items():
            integer(view, rec["view"])
            _mapping(accepted, {"signed", "body"})
            p = self.proofs.check("proposal", accepted["signed"], instance)
            _require(p["view"] == view)
            self._body_evidence(accepted["body"], rec, p["value"])
            if view:
                _require(rec["new_views"].get(view) == p["new_view"])
        for view, signed in rec["new_views"].items():
            integer(view, rec["view"], minimum=1)
            _require(self.proofs.check("new_view", signed, instance)["view"] == view)
        prepared_views = []
        for key, qc in rec["prepared"].items():
            _require(type(key) is tuple and len(key) == 2)
            integer(key[0], rec["view"])
            exact_id(key[1])
            p = self.proofs.check("prepared", qc, instance)
            _require(key == (p["view"], p["value"]))
            prepared_views.append(p["view"])
        if prepared_views:
            _require(rec["highest"] is not None)
            p = self.proofs.check("prepared", rec["highest"], instance)
            _require(p["view"] == max(prepared_views)
                     and rec["prepared"].get((p["view"], p["value"])) == rec["highest"])
        else:
            _require(rec["highest"] is None)
        for slot in rec["intents"]:
            self._slot(slot)
        for slot, payload in rec["intents"].items():
            self._intent(payload, slot, rec)
        needed_bodies = {p["value"] for p in rec["intents"].values() if p["value"] is not None}
        _require(set(rec["intent_bodies"]) == needed_bodies)
        for value, body in rec["intent_bodies"].items():
            exact_id(value)
            self._body_evidence(body, rec, value)
        for slot, signed in rec["signed"].items():
            phase, _ = self._slot(slot)
            _require(slot in rec["intents"])
            p = self.proofs.check(_KINDS[phase], signed, instance)
            _require(p == rec["intents"][slot])
        if rec["decision"] is None:
            _require(rec["body"] is None and not rec["applied"])
            return None
        p = self.proofs.check("commit", rec["decision"], instance)
        return self._body_evidence(rec["body"], rec, p["value"])

    def _validate_recovery(self, state):
        try:
            self._validate_state(state)
        except StorageError:
            raise
        except (ValueError, TypeError, KeyError, AttributeError, RecursionError, OverflowError) as exc:
            raise StorageError("RECOVERY_EVIDENCE_INVALID") from exc
        finally:
            self.__dict__.pop("_recovery_anchors", None)

    def _validate_state(self, state):
        _mapping(state, _STATE)
        integer(state["sequence"], PROFILE["limits"]["sequences"])
        _require(type(state["snapshot"]) is bytes and type(state["fenced"]) is bool
                 and type(state["halt"]) is str)
        _require(Application(state["snapshot"]).snapshot == state["snapshot"])
        _mapping(state["records"])
        _mapping(state["retained_bodies"])
        expected_count = min(state["sequence"] + 1, PROFILE["limits"]["sequences"])
        _require(all(type(seq) is int for seq in state["records"])
                 and sorted(state["records"]) == list(range(expected_count)))
        _require(len(state["retained_bodies"]) <= PROFILE["limits"]["objects"])
        # Only protected headers help validate protected bodies. This temporary
        # collection never becomes the runtime's network DATA/OFFER cache.
        self._recovery_anchors = {self.initial_anchor["hash"]: self.initial_anchor}
        for rec in state["records"].values():
            _mapping(rec, _RECORD)
            _mapping(rec["anchor_before"], {"height", "hash", "parent", "context"})
            self._recovery_anchors[rec["anchor_before"]["hash"]] = rec["anchor_before"]
            if rec["body"] is not None:
                _mapping(rec["body"], {"instance", "anchor", "batch", "result"})
                _mapping(rec["body"]["anchor"], {"height", "hash", "parent", "context"})
                self._recovery_anchors[rec["body"]["anchor"]["hash"]] = rec["body"]["anchor"]
        snapshot, parent, anchor = self.initial_snapshot, self.genesis, self.initial_anchor
        for sequence, rec in sorted(state["records"].items()):
            instance = rec["instance"]
            _mapping(instance, _INSTANCE)
            _require(instance == {"profile": PROFILE["profile_id"], "domain": PROFILE["domain"],
                                  "config": self.config, "epoch": 0, "set": self.set_id,
                                  "sequence": sequence, "parent": parent}
                     and type(instance["sequence"]) is int and type(instance["epoch"]) is int)
            _require(type(rec["before"]) is bytes and rec["before"] == snapshot
                     and rec["anchor_before"] == anchor)
            resulting = self._record_evidence(rec)
            _require(rec["applied"] == (sequence < state["sequence"]))
            if rec["applied"]:
                snapshot, parent, anchor = resulting, value_id(rec["body"]), rec["body"]["anchor"]
        for value, body in state["retained_bodies"].items():
            exact_id(value)
            _mapping(body, {"instance", "anchor", "batch", "result"})
            _mapping(body["instance"], _INSTANCE)
            seq = body["instance"]["sequence"]
            integer(seq, PROFILE["limits"]["sequences"] - 1)
            _require(seq in state["records"])
            self._body_evidence(body, state["records"][seq], value)
        _require(state["snapshot"] == snapshot and state["parent"] == parent
                 and state["anchor"] == anchor)
        if state["sequence"] == PROFILE["limits"]["sequences"]:
            _require(state["halt"] == "SEQUENCE_EXHAUSTED")
