"""Bounded disposable network state; never substitutes for durable evidence."""
from copy import deepcopy

from fm_application import (NeedData, InvalidValue, canonical, value_id,
                            normalize_batch, _check_anchor)
from fm_protocol import PROFILE, Invalid, Exhausted, exact_id


class AdmissionMixin:
    def _admission_reset(self):
        self.requests, self.references = {}, {}
        self._missing_value = None

    def _refuse(self, reason, **info):
        self.last_reason = reason
        self.event("refused", reason=reason, **info)

    def _protected_values(self):
        r = self.record
        if r is None:
            return set()
        values = {entry["signed"]["payload"]["value"] for entry in r["accepted"].values()}
        values.update(value for _, value in r["prepared"])
        values.update(p["value"] for p in r["intents"].values() if p["value"] is not None)
        values.update(nv["payload"]["value"] for nv in r["new_views"].values())
        if r["body"]:
            values.add(value_id(r["body"]))
        return values

    def _retained_body(self, vid, rec=None):
        r = rec or self.record
        if r:
            if r["body"] is not None and value_id(r["body"]) == vid:
                return r["body"]
            for item in r["accepted"].values():
                if item["signed"]["payload"]["value"] == vid:
                    return item["body"]
        return self.d["retained_bodies"].get(vid)

    def _body_shape(self, body):
        if type(body) is not dict or set(body) != {"instance", "anchor", "batch", "result"}:
            raise Invalid("DATA_BODY_SHAPE")
        exact_id(body["result"])
        _check_anchor(body["anchor"])
        if normalize_batch(body["batch"]) != body["batch"]:
            raise Invalid("DATA_BODY_NONCANONICAL")
        if len(canonical(body)) > PROFILE["limits"]["proof_bytes"]:
            raise Invalid("DATA_BODY_BYTES")
        return self._context(body["instance"])

    def _reference(self, kind, data):
        """Authenticate and bound a reference before it can allocate requests."""
        if kind == "SIGNED":
            if not self.proofs.authenticate(data):
                raise Invalid("AUTHENTICATION")
            p = data["payload"]
            proof_kind = {"PROPOSE": "proposal", "NEW_VIEW": "new_view"}.get(p["phase"])
            if proof_kind is None:
                return None
        elif kind == "PREPARED":
            p, proof_kind = data["proposal"]["payload"], "prepared"
        elif kind == "CERT":
            p, proof_kind = data["prepared"]["proposal"]["payload"], "commit"
        else:
            raise Invalid("DATA_REFERENCE_KIND")
        try:
            self.proofs.check(proof_kind, data, p["instance"])
        except Exhausted as exc:
            raise Invalid("REFERENCE_RESOURCE_LIMIT:" + str(exc)) from exc
        rec = self._context(p["instance"])
        if rec["applied"]:
            return p
        value = p["value"]
        protected = value in self._protected_values()
        strong = kind in ("PREPARED", "CERT")
        if p["view"] < self.record["view"] and not (protected or strong):
            raise Invalid("STALE_DATA_REFERENCE")
        self._expire_admission()
        slot = (p["instance"]["sequence"], p["view"], proof_kind)
        if value not in self.references:
            # A Byzantine scheduled leader may sign arbitrary alternate hashes,
            # but cannot grow a table without limit by calling them references.
            count = sum(r["slot"] == slot for r in self.references.values())
            if count >= PROFILE["admission"]["references_per_slot"]:
                raise Invalid("REFERENCE_SLOT_PRESSURE")
            if len(self.references) >= PROFILE["admission"]["references"]:
                disposable = next((vid for vid in self.references
                                   if vid not in self._protected_values()), None)
                if strong and disposable is not None:
                    self.references.pop(disposable)
                else:
                    raise Invalid("REFERENCE_PRESSURE")
        self.references[value] = {"slot": slot, "instance": deepcopy(p["instance"]),
                                  "view": p["view"], "strong": strong,
                                  "expires": self.clock() + PROFILE["admission"]["lease_ticks"]}
        return p

    def _reference_live(self, value):
        if value in self._protected_values():
            return True
        ref = self.references.get(value)
        return bool(ref and ref["expires"] > self.clock()
                    and ref["instance"] == self.instance
                    and (ref["view"] >= self.record["view"] or ref["strong"]))

    def _expire_admission(self):
        # These are fixed-size disposable tables, not a walk of durable history.
        for key, req in list(self.requests.items()):
            if req["expires"] <= self.clock() or req["sequence"] != self.d["sequence"]:
                self.requests.pop(key)
        for vid, ref in list(self.references.items()):
            if ref["instance"]["sequence"] != self.d["sequence"] or (
                    ref["expires"] <= self.clock() and vid not in self._protected_values()):
                self.references.pop(vid)

    def _store_body(self, body):
        vid = value_id(body)
        if vid in self.bodies:
            return vid
        if len(self.bodies) >= PROFILE["limits"]["objects"]:
            protected = self._protected_values() | self.offers
            disposable = next((key for key in self.bodies if key not in protected), None)
            if disposable is None:
                raise Invalid("BODY_CACHE_PRESSURE")
            self.bodies.pop(disposable)
            self.event("cache_evicted", type="body", identity=disposable)
        self.bodies[vid] = deepcopy(body)
        self.event("cache_admitted", type="body", identity=vid, count=len(self.bodies))
        return vid

    def _protected_anchors(self):
        protected = {self.d["anchor"]["hash"]}
        for vid in self._protected_values():
            body = self.bodies.get(vid) or self._retained_body(vid)
            current = body["anchor"] if body else None
            for _ in range(PROFILE["limits"]["objects"]):
                if current is None or current["hash"] in protected:
                    break
                protected.add(current["hash"])
                current = self.anchors.get(current["parent"])
        return protected

    def _admit_data(self, data, source):
        if type(data) is not dict or set(data) not in (
                {"type", "id", "object"}, {"type", "id", "object", "reference"}):
            raise Invalid("DATA_SHAPE")
        kind, identity, obj = data["type"], data["id"], data["object"]
        exact_id(identity)
        reference = data.get("reference")
        if kind == "body":
            if value_id(obj) != identity:
                raise Invalid("BODY_HASH")
            rec = self._body_shape(obj)
            if reference is not None:
                if type(reference) is not dict or set(reference) != {"kind", "data"}:
                    raise Invalid("DATA_REFERENCE_SHAPE")
                p = self._reference(reference["kind"], reference["data"])
                if p is None or p["value"] != identity or p["instance"] != obj["instance"]:
                    raise Invalid("DATA_REFERENCE_LINK")
            if rec["applied"]:
                raise Invalid("STALE_BODY_DATA")
            req = self.requests.get((kind, identity))
            requested = req and req["expires"] > self.clock() and req["sequence"] == self.d["sequence"]
            if not requested and not self._reference_live(identity):
                raise Invalid("UNREQUESTED_BODY_DATA")
            self._missing_value = identity
            self._valid_body(obj, rec)  # clone/preview only; never applies the ledger
            self._store_body(obj)
        elif kind == "anchor":
            if reference is not None:
                raise Invalid("ANCHOR_REFERENCE_SHAPE")
            _check_anchor(obj)
            if obj["hash"] != identity:
                raise Invalid("SYNTHETIC_ANCHOR_HASH")
            req = self.requests.get((kind, identity))
            if identity not in self.anchors and not (req and req["expires"] > self.clock()
                    and req["sequence"] == self.d["sequence"]
                    and self._reference_live(req["value"])):
                raise Invalid("UNREQUESTED_ANCHOR_DATA")
            if identity not in self.anchors:
                if len(self.anchors) >= PROFILE["limits"]["objects"]:
                    protected = self._protected_anchors()
                    disposable = next((key for key in self.anchors if key not in protected), None)
                    if disposable is None:
                        raise Invalid("ANCHOR_CACHE_PRESSURE")
                    self.anchors.pop(disposable)
                    self.event("cache_evicted", type="anchor", identity=disposable)
                self.anchors[identity] = deepcopy(obj)
                self.event("cache_admitted", type="anchor", identity=identity, count=len(self.anchors))
        else:
            raise Invalid("DATA_TYPE")
        self.requests.pop((kind, identity), None)
        self._retry_pending(limit=PROFILE["admission"]["pending"])
        self._resume_intents()
        if reference is not None:
            self._handle(reference["kind"], reference["data"], source)

    def _need(self, wire, identity):
        if identity.startswith("parent_sequence:"):
            self._refuse("MISSING_PARENT_NOT_RETAINED")
            self._request_history(wire["source"])
            return
        if wire["kind"] == "DATA":
            data = wire["data"]
            # A well-shaped, context-matched body awaiting exact ancestry is
            # disposable pending data. It has not entered the validated cache.
            if data.get("type") != "body" or not self._reference_live(data["id"]):
                self._refuse("UNREFERENCED_PENDING_DATA")
                return
            key = ("DATA", data["id"])
        elif wire["kind"] in ("SIGNED", "PREPARED", "CERT"):
            p = self._reference(wire["kind"], wire["data"])
            if p is None:
                # Individual votes cannot manufacture a body reference.
                self._refuse("VOTE_CANNOT_REQUEST_DATA")
                return
            key = (wire["kind"], p["phase"], p["view"], p["sender"], p["value"])
        else:
            self._refuse("UNREFERENCED_PENDING_DATA")
            return
        if key not in self.pending and len(self.pending) >= PROFILE["admission"]["pending"]:
            self._refuse("PENDING_PRESSURE")
            return
        self.pending[key] = deepcopy(wire)
        self._request_missing(identity)

    def _request_missing(self, identity):
        self.last_reason = "NEED_DATA:" + str(identity)
        self.event("defer", reason=self.last_reason)
        kind, _, key = identity.partition(":")
        if kind not in ("body", "anchor"):
            return
        value = key if kind == "body" else self._missing_value
        if not self._reference_live(value):
            return
        exact_id(key)
        self._expire_admission()
        slot = (kind, key)
        if slot not in self.requests and len(self.requests) >= PROFILE["admission"]["requests"]:
            self._refuse("REQUEST_PRESSURE")
            return
        self.requests[slot] = {"type": kind, "id": key, "value": value,
                               "sequence": self.d["sequence"],
                               "expires": self.clock() + PROFILE["admission"]["lease_ticks"]}
        self._send("GET", {"type": kind, "id": key})

    def _retry_pending(self, limit=None):
        count = len(self.pending) if limit is None else min(limit, len(self.pending))
        for _ in range(count):
            if len(self.inbox) >= PROFILE["limits"]["inbox"]:
                self._refuse("INBOX_PRESSURE")
                return
            key = next(iter(self.pending))
            wire = self.pending.pop(key)
            # Transfer our already size-checked, privately owned pending wire;
            # do not copy/encode every payload just to schedule revalidation.
            self.inbox.append((wire["kind"], wire["data"], wire["source"]))
