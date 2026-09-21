"""Isolated PBFT-style replica: stable-memory journal, not production storage."""
from collections import deque
from copy import deepcopy

from fm_application import Application, NeedData, InvalidValue, canonical, digest, value_id
from fm_protocol import PROFILE, Proofs, Invalid, Exhausted, message, proposer
from fm_admission import AdmissionMixin
from fm_delivery import DeliveryMixin
from fm_memory import update_durable


class Crash(Exception):
    pass


class Replica(AdmissionMixin, DeliveryMixin):
    def __init__(self, index, n, signer, authenticate, snapshot, anchor, emit, clock, trace):
        self.index, self.n, self.f = index, n, (n - 1) // 3
        self.q = 2 * self.f + 1
        self.signer = signer
        self.proofs = Proofs(n, authenticate)
        self.emit, self.clock, self.trace = emit, clock, trace
        self.alive, self.cut = True, None
        self.config = digest("TEST/V2/CONFIG/1", [PROFILE, list(range(n)), snapshot.hex(), anchor])
        self.set_id = digest("TEST/V2/SET/1", list(range(n)))
        self.d = {"sequence": 0, "snapshot": snapshot, "anchor": deepcopy(anchor),
                  "parent": digest("TEST/V2/GENESIS/1", [self.config, anchor]),
                  "records": {}, "retained_bodies": {}, "fenced": False, "halt": ""}
        self._install(self.d)
        self._volatile()

    def _install(self, d):
        seq = d["sequence"]
        if seq >= PROFILE["limits"]["sequences"]:
            d["halt"] = "SEQUENCE_EXHAUSTED"
            return
        i = {"profile": PROFILE["profile_id"], "domain": PROFILE["domain"],
             "config": self.config, "epoch": 0, "set": self.set_id,
             "sequence": seq, "parent": d["parent"]}
        d["records"][seq] = {"instance": i, "view": 0, "mode": "ACTIVE", "accepted": {},
            "new_views": {}, "prepared": {}, "highest": None, "intents": {}, "signed": {},
            "decision": None, "body": None, "applied": False, "apply_count": 0,
            "before": d["snapshot"], "anchor_before": deepcopy(d["anchor"])}

    @property
    def record(self):
        return self.d["records"].get(self.d["sequence"])

    @property
    def instance(self):
        return deepcopy(self.record["instance"])

    @property
    def application(self):
        return Application(self.d["snapshot"])

    def _volatile(self):
        self.inbox = deque()
        self.bodies, self.anchors, self.offers = {}, {self.d["anchor"]["hash"]: deepcopy(self.d["anchor"])}, set()
        self.votes, self.headers, self.reports, self.pending = {}, {}, {}, {}
        self.announced = set()
        self.deadline = None
        self.next_retry = self.clock() + PROFILE["timers"]["retry_ticks"]
        self.local_tip = self.d["anchor"]["height"]  # diagnostics only
        self.last_reason = ""
        self.bodies.update({vid: deepcopy(body) for vid, body in self.d["retained_bodies"].items()
                            if self.record and body["instance"] == self.record["instance"]})
        for rec in [self.record] if self.record else []:
            if rec["body"] is not None:
                self.bodies[value_id(rec["body"])] = deepcopy(rec["body"])
            for data in rec["accepted"].values():
                self.bodies[value_id(data["body"])] = deepcopy(data["body"])
        if self.record and self.record["decision"] is None:
            for vid, body in self.d["retained_bodies"].items():
                if body["instance"] == self.record["instance"]:
                    self.offers.add(vid)
            for data in self.record["accepted"].values():
                p = data["signed"]["payload"]
                self.headers[(p["view"], p["value"])] = deepcopy(data["signed"])
            for key, qc in self.record["prepared"].items():
                self.headers.setdefault(key, deepcopy(qc["proposal"]))
            if self.record["mode"] == "ACTIVE" and (self.offers or self.headers):
                self._timer()
        self._admission_reset()
        self._delivery_reset()

    def event(self, name, **info):
        self.trace(self.index, name, info)

    def _persist(self, mutate, reason, **evidence):
        self.d, self.last_memory_work = update_durable(self.d, mutate)
        self.event("durable", reason=reason, memory_work=self.last_memory_work, **evidence)

    def _crash_at(self, point, phase=""):
        if self.cut == (point, phase):
            self.cut = None
            self.alive = False
            self.event("crash", point=point, phase=phase)
            raise Crash(point)

    def crash(self):
        self.alive = False
        self.event("crash", point="scheduler")

    def restart(self, suspected_rollback=False):
        try:
            self._restart(suspected_rollback)
        except Exhausted as exc:
            self._halt(str(exc))

    def _restart(self, suspected_rollback=False):
        # Supported restart reuses the *same* acknowledged stable-memory state.
        # No untrusted old snapshot is made authoritative by this API.
        if suspected_rollback:
            self._persist(lambda d: d.update(fenced=True), "SIGNER_FRESHNESS_UNPROVEN")
        self.alive = True
        self._volatile()
        self.event("restart", fenced=self.d["fenced"])
        if self.record and self.record["decision"] is not None:
            # Recovery still verifies exact anchor evidence. A durable decision
            # is not permission to guess evidence lost from the volatile cache.
            try:
                self._apply()
            except NeedData as exc:
                self._need({"kind": "CERT", "data": self.record["decision"],
                            "source": self.index}, str(exc))
        self.retry()

    def _resume_intents(self):
        if (not self.alive or not self.record or self.record["decision"] is not None
                or self.d["fenced"] or self.d["halt"]):
            return
        r = self.record
        for slot, payload in list(r["intents"].items()):
            phase, view = slot
            if slot in self.record["signed"] or view != self.record["view"]:
                continue
            if phase in ("PROPOSE", "PREPARE", "COMMIT") and r["mode"] != "ACTIVE":
                continue
            if phase in ("PREPARE", "COMMIT"):
                accepted = r["accepted"].get(view)
                if not accepted or accepted["signed"]["payload"]["value"] != payload["value"]:
                    continue
                if phase == "COMMIT" and (view, payload["value"]) not in r["prepared"]:
                    continue
            if phase in ("VIEW_CHANGE", "NEW_VIEW") and r["mode"] != "CHANGING":
                continue
            if payload["value"] is not None:
                # An intent is not an issued signature or a substitute for
                # current local evidence. Keep its exact bytes while fetching
                # any ancestry lost with the volatile cache at restart.
                try:
                    self._body(payload["value"])
                except NeedData as exc:
                    self._request_missing(str(exc))
                    continue
            extra = {k: deepcopy(v) for k, v in payload.items()
                     if k not in ("phase", "sender", "instance", "view", "value")}
            self._sign(phase, payload["value"], **extra)

    def _halt(self, reason):
        if not self.d["halt"]:
            self._persist(lambda d: d.update(halt=reason), reason)
        self.last_reason = reason
        self.event("halt", reason=reason)

    def _send(self, kind, data, destination=None):
        if not self._delivery_allow_send(kind, data, destination):
            self.event("send_deferred", kind=kind, reason="RETRY_WORK_BUDGET")
            return False
        if kind == "SIGNED":
            p = data["payload"]
            self.event("publish", auth=data["auth"], phase=p["phase"],
                       view=p["view"], sequence=p["instance"]["sequence"])
        self.emit(self.index, kind, deepcopy(data), destination)
        return True

    def _sign(self, phase, value=None, **extra):
        r = self.record
        if not self.alive or self.d["fenced"] or self.d["halt"]:
            self.last_reason = "SIGNER_FRESHNESS_UNPROVEN" if self.d["fenced"] else "SIGNING_UNAVAILABLE"
            return None
        v, seq = r["view"], self.d["sequence"]
        slot = (phase, v)
        payload = message(phase, self.index, r["instance"], v, value, **extra)
        old = r["intents"].get(slot)
        if old is not None and canonical(old) != canonical(payload):
            self.event("refused", reason="DURABLE_SLOT_CONFLICT", phase=phase, view=v)
            return None
        if slot not in r["signed"]:
            self._crash_at("before_record", phase)
            self._persist(lambda d: d["records"][seq]["intents"].__setitem__(slot, payload),
                          "intent:" + phase, payload=payload)
            self._crash_at("after_intent", phase)
            r = self.record
            # Test-only history for the independent oracle, not a voting input.
            self.event("pre_sign", payload=payload, guard={
                "instance": r["instance"], "view": r["view"], "mode": r["mode"],
                "accepted": r["accepted"].get(v), "new_view": r["new_views"].get(v),
                "prepared": r["prepared"].get((v, value)), "highest": r["highest"],
                "body": self.bodies.get(value), "anchors": self.anchors,
                "fenced": self.d["fenced"], "halt": self.d["halt"], "decision": r["decision"]})
            signed = self.signer(payload)
            self._persist(lambda d: d["records"][seq]["signed"].__setitem__(slot, signed),
                          "signature:" + phase, auth=signed["auth"])
            self._crash_at("after_record", phase)
        signed = self.record["signed"][slot]
        self._send("SIGNED", signed)
        self._crash_at("after_publish", phase)
        return signed

    def _timer(self):
        if self.deadline is None and self.record:
            ticks = min(PROFILE["timers"]["maximum_ticks"],
                        PROFILE["timers"]["initial_ticks"] * (2 ** self.record["view"]))
            self.deadline = self.clock() + ticks
            self.event("timer", view=self.record["view"], deadline=self.deadline)

    def offer(self, body):
        try:
            self._offer(body)
            self._send("OFFER", body)
        except Exhausted as exc:
            self._halt(str(exc))
        except (Invalid, InvalidValue) as exc:
            self._refuse(str(exc), kind="OFFER")

    def _offer(self, body):
        if self.d["halt"]:
            return
        self._valid_body(body)
        self._store_body(body)
        if value_id(body) not in self.d["retained_bodies"]:
            if len(self.d["retained_bodies"]) >= PROFILE["limits"]["objects"]:
                raise Exhausted("RETAINED_BODY_LIMIT")
            self._persist(lambda d: d["retained_bodies"].__setitem__(value_id(body), deepcopy(body)), "offered_body")
        self.offers.add(value_id(body))
        self._timer()
        self._drive()

    def _valid_body(self, body, rec=None):
        rec = rec or self.record
        self._missing_value = value_id(body)
        try:
            return Application(rec["before"]).validate(body, rec["instance"], rec["anchor_before"], self.anchors)
        except NeedData as exc:
            raise NeedData("anchor:" + str(exc)) from exc

    def _body(self, vid, rec=None):
        self._missing_value = vid
        if vid not in self.bodies:
            retained = self._retained_body(vid, rec)
            if retained is None:
                raise NeedData("body:" + vid)
            self._valid_body(retained, rec)
            self._store_body(retained)
        body = self.bodies[vid]
        self._valid_body(body, rec)
        return body

    def receive(self, kind, data, source):
        if not self.alive:
            return
        if len(self.inbox) >= PROFILE["limits"]["inbox"]:
            self._refuse("INBOX_PRESSURE")
            return
        try:
            if len(canonical(data)) > PROFILE["limits"]["proof_bytes"]:
                self.event("refused", reason="WIRE_BYTES")
                return
        except (ValueError, TypeError, RecursionError):
            self.event("refused", reason="MALFORMED_WIRE")
            return
        self.inbox.append((kind, deepcopy(data), source))

    def pump(self):
        if not self.alive or not self.inbox:
            return
        kind, data, source = self.inbox.popleft()
        wire = {"kind": kind, "data": data, "source": source}
        try:
            self._handle(kind, data, source)
            self._drive()
        except NeedData as exc:
            try:
                self._need(wire, str(exc))
            except Exhausted as exhausted:
                self._halt(str(exhausted))
            except (Invalid, InvalidValue, KeyError, TypeError, ValueError) as refused:
                self._refuse(str(refused), kind=kind)
        except Exhausted as exc:
            self._halt(str(exc))
        except (Invalid, InvalidValue, KeyError, TypeError, ValueError, RecursionError) as exc:
            self.last_reason = str(exc)
            self.event("refused", reason=str(exc), kind=kind)
        except Crash:
            pass

    def _context(self, instance):
        if type(instance) is not dict or type(instance.get("sequence")) is not int:
            raise Invalid("INSTANCE_SHAPE")
        seq = instance["sequence"]
        if not 0 <= seq < PROFILE["limits"]["sequences"]:
            raise Invalid("SEQUENCE_BOUND")
        rec = self.d["records"].get(seq)
        if rec is None:
            raise NeedData("parent_sequence:" + str(seq - 1))
        if canonical(rec["instance"]) != canonical(instance):
            raise Invalid("INSTANCE_CONTEXT")
        return rec

    def _handle(self, kind, data, source):
        if self._delivery_handle(kind, data, source):
            return
        if kind == "OFFER":
            # Client ingress is synthetic. Each recipient independently checks
            # this exact body before retaining pending work; OFFER is not a vote.
            rec = self._context(data["instance"])
            if rec["applied"]:
                self._send("CERT", rec["decision"], source)
            else:
                self._offer(data)
            return
        if kind == "GET":
            if set(data) != {"type", "id"}:
                raise Invalid("GET_SHAPE")
            cache = self.bodies if data["type"] == "body" else self.anchors if data["type"] == "anchor" else {}
            if data["id"] in cache:
                self._send("DATA", {"type": data["type"], "id": data["id"], "object": cache[data["id"]]}, source)
            return
        if kind == "DATA":
            self._admit_data(data, source)
            return
        if kind == "SIGNED":
            if not self.proofs.authenticate(data):
                raise Invalid("AUTHENTICATION")
            p = data["payload"]
            kinds = {"PROPOSE": "proposal", "PREPARE": "prepare_vote", "COMMIT": "commit_vote",
                     "VIEW_CHANGE": "view_change", "NEW_VIEW": "new_view"}
            if p["phase"] not in kinds:
                raise Invalid("UNKNOWN_PHASE")
            # Validate before retaining an unknown parent or replying to old
            # traffic. Authentication does not prove context or quorum weight.
            try:
                self.proofs.check(kinds[p["phase"]], data, p["instance"])
            except Exhausted as exc:
                raise Invalid("REFERENCE_RESOURCE_LIMIT:" + str(exc)) from exc
            rec = self._context(p["instance"])
            phase = p["phase"]
            if rec["applied"]:
                # An old report/request cannot reopen a finalized sequence.
                self._send("CERT", rec["decision"], source)
                return
            if phase == "PROPOSE":
                self._reference(kind, data)
                self._proposal(data)
            elif phase in ("PREPARE", "COMMIT"):
                if not (self._reference_live(p["value"]) or (p["view"], p["value"]) in self.headers):
                    raise Invalid("UNREFERENCED_VOTE")
                self.proofs.check("prepare_vote" if phase == "PREPARE" else "commit_vote", data, rec["instance"])
                key = (p["view"], p["value"], phase)
                self.votes.setdefault(key, {})[p["sender"]] = deepcopy(data)
                self.event("vote_received", phase=phase, sender=p["sender"], value=p["value"], view=p["view"])
                self._aggregate()
            elif phase == "VIEW_CHANGE":
                self._view_change(data)
            elif phase == "NEW_VIEW":
                self._reference(kind, data)
                self._new_view(data)
            else:
                raise Invalid("UNKNOWN_PHASE")
        elif kind == "PREPARED":
            self._reference(kind, data)
            p = data["proposal"]["payload"]
            self.proofs.check("prepared", data, p["instance"])
            rec = self._context(p["instance"])
            if not rec["applied"]:
                self._prepared(data)
        elif kind == "CERT":
            self._reference(kind, data)
            self._certificate(data)
        else:
            raise Invalid("UNKNOWN_WIRE_KIND")

    def _proposal(self, signed):
        p = self.proofs.check("proposal", signed, self.instance)
        body = self._body(p["value"])
        self.headers[(p["view"], p["value"])] = deepcopy(signed)
        if p["view"] > 0 and p["view"] >= self.record["view"]:
            self._new_view(p["new_view"])
        r = self.record
        if p["view"] != r["view"] or r["mode"] != "ACTIVE":
            self.event("defer", reason="ABANDONED_OR_INACTIVE_VIEW", view=p["view"])
            self._aggregate()
            return
        if p["view"] and r["new_views"].get(p["view"]) != p["new_view"]:
            raise Invalid("CONFLICTING_NEW_VIEW")
        existing = r["accepted"].get(p["view"])
        if existing and existing["signed"]["payload"]["value"] != p["value"]:
            raise Invalid("CONFLICTING_ACCEPTED_PROPOSAL")
        if not existing:
            seq = self.d["sequence"]
            self._persist(lambda d: d["records"][seq]["accepted"].__setitem__(p["view"],
                          {"signed": signed, "body": body}), "accepted_proposal")
        self._timer()
        slot = ("PREPARE", p["view"])
        if slot not in self.record["signed"]:
            self._sign("PREPARE", p["value"])
        self._aggregate()

    def _prepared(self, qc):
        p = self.proofs.check("prepared", qc, self.instance)
        self._body(p["value"])
        self.headers.setdefault((p["view"], p["value"]), deepcopy(qc["proposal"]))
        if p["view"] > self.record["view"]:
            # Catch up only via the QC's complete valid NEW_VIEW, never its view integer.
            self._new_view(qc["proposal"]["payload"]["new_view"])
        old = self.record["highest"]
        if old is not None:
            oldp = old["proposal"]["payload"]
            if oldp["view"] == p["view"] and oldp["value"] != p["value"]:
                self._halt("CONFLICTING_PREPARED_PROOFS")
                return
        seq, key = self.d["sequence"], (p["view"], p["value"])
        if key not in self.record["prepared"]:
            def store(d):
                r = d["records"][seq]
                r["prepared"][key] = deepcopy(qc)
                if r["highest"] is None or p["view"] > r["highest"]["proposal"]["payload"]["view"]:
                    r["highest"] = deepcopy(qc)
            self._persist(store, "prepared_proof")
        r = self.record
        accepted = r["accepted"].get(p["view"])
        if (r["mode"] == "ACTIVE" and r["view"] == p["view"] and accepted
                and accepted["signed"]["payload"]["value"] == p["value"]
                and (p["view"] == 0 or p["view"] in r["new_views"])
                and ("COMMIT", p["view"]) not in r["signed"]):
            self._sign("COMMIT", p["value"])

    def _aggregate(self):
        # Only received messages and durable local evidence; never global vote state.
        for (v, value), proposal in list(self.headers.items()):
            if proposal["payload"]["instance"] != self.instance:
                continue
            prepares = self.votes.get((v, value, "PREPARE"), {})
            if len(prepares) >= self.q:
                qc = {"proposal": proposal, "prepares": [prepares[k] for k in sorted(prepares)]}
                self._prepared(qc)
                mark = ("PREPARED", self.d["sequence"], v, value)
                if mark not in self.announced:
                    self.announced.add(mark)
                    self._send("PREPARED", qc)
            qc = self.record["prepared"].get((v, value))
            commits = self.votes.get((v, value, "COMMIT"), {})
            if qc and len(commits) >= self.q:
                cert = {"prepared": qc, "commits": [commits[k] for k in sorted(commits)]}
                self._certificate(cert)
                self._send("CERT", cert)
                return

    def change_view(self, target, reason="timeout"):
        r = self.record
        if not r or self.d["halt"] or r["decision"] is not None:
            return
        if target <= r["view"]:
            return
        if target >= PROFILE["limits"]["views"]:
            self._halt("VIEW_EXHAUSTED")
            return
        if r["highest"] and r["highest"]["proposal"]["payload"]["view"] >= target:
            self.last_reason = "WAIT_NEW_VIEW_EVIDENCE_FOR_FUTURE_PREPARATION"
            self.event("defer", reason=self.last_reason)
            return
        seq = self.d["sequence"]
        self._persist(lambda d: d["records"][seq].update(view=target, mode="CHANGING"), "enter_view:" + reason)
        self.deadline = None
        self._sign("VIEW_CHANGE", prepared=self.record["highest"], decision=None)

    def _view_change(self, signed):
        p = self.proofs.check("view_change", signed, self.instance)
        if p["decision"] is not None:
            self._certificate(p["decision"])
            return
        reports = self.reports.setdefault(p["view"], {})
        # Keep first exact report for an identity/target. Equivocation is not extra weight.
        reports.setdefault(p["sender"], deepcopy(signed))
        if p["view"] > self.record["view"] and len(reports) >= self.f + 1:
            self.change_view(p["view"], "f_plus_one_reports")
        if p["view"] == self.record["view"] and len(reports) >= self.q:
            self._timer()

    def _new_view(self, signed):
        p = self.proofs.check("new_view", signed, self.instance)
        self._body(p["value"])
        if p["view"] < self.record["view"]:
            return
        old = self.record["new_views"].get(p["view"])
        if old:
            if old != signed:
                raise Invalid("CONFLICTING_NEW_VIEW")
            return
        if self.record["decision"] is not None:
            raise Invalid("ALREADY_DECIDED")
        seq = self.d["sequence"]
        def store(d):
            r = d["records"][seq]
            r["view"], r["mode"] = p["view"], "ACTIVE"
            r["new_views"][p["view"]] = deepcopy(signed)
        self._persist(store, "accepted_new_view")
        self.deadline = None
        self._timer()

    def _drive(self):
        if (not self.record or self.record["decision"] is not None
                or not self.alive or self.d["halt"] or self.d["fenced"]):
            return
        r, seq = self.record, self.d["sequence"]
        v = r["view"]
        if proposer(r["instance"], v, self.n) != self.index:
            return
        if r["mode"] == "CHANGING" and ("NEW_VIEW", v) not in r["intents"]:
            reports = self.reports.get(v, {})
            if len(reports) < self.q:
                return
            chosen = [reports[k] for k in sorted(reports)[:self.q]]
            prepared = [x["payload"]["prepared"] for x in chosen if x["payload"]["prepared"]]
            if prepared:
                best_view = max(x["proposal"]["payload"]["view"] for x in prepared)
                values = {x["proposal"]["payload"]["value"] for x in prepared
                          if x["proposal"]["payload"]["view"] == best_view}
                if len(values) != 1:
                    self._halt("CONFLICTING_PREPARED_PROOFS")
                    return
                value = next(iter(values))
            elif self.offers:
                value = min(self.offers)
            else:
                self.last_reason = "NO_VALID_LOCAL_VALUE"
                return
            self._body(value)
            self._sign("NEW_VIEW", value, reports=chosen)
        elif r["mode"] == "ACTIVE" and ("PROPOSE", v) not in r["intents"]:
            if v:
                nv = r["new_views"].get(v)
                if not nv:
                    return
                value = nv["payload"]["value"]
            elif self.offers:
                value, nv = min(self.offers), None
            else:
                return
            body = self._body(value)
            signed = self._sign("PROPOSE", value, new_view=nv)
            if signed is not None:
                self._send("DATA", {"type": "body", "id": value, "object": body,
                                    "reference": {"kind": "SIGNED", "data": signed}})

    def _certificate(self, cert):
        p = cert["prepared"]["proposal"]["payload"]
        self.proofs.check("commit", cert, p["instance"])
        rec = self._context(p["instance"])
        p = self.proofs.check("commit", cert, rec["instance"])
        if rec["applied"]:
            if rec["decision"]["prepared"]["proposal"]["payload"]["value"] != p["value"]:
                self._halt("CONFLICTING_DECISIONS")
            return
        body = self._body(p["value"], rec)
        if rec["decision"] is not None:
            old = rec["decision"]["prepared"]["proposal"]["payload"]["value"]
            if old != p["value"]:
                self._halt("CONFLICTING_DECISIONS")
            elif not rec["applied"]:
                self._apply()
            return
        seq = p["instance"]["sequence"]
        self._persist(lambda d: d["records"][seq].update(decision=deepcopy(cert), body=deepcopy(body), mode="DECIDED"),
                      "decision")
        self.event("certificate", sequence=seq, value=p["value"], view=p["view"])
        self._crash_at("after_decision")
        self._apply()

    def _apply(self):
        rec = self.record
        if not rec or rec["decision"] is None or rec["applied"]:
            return
        body, seq = rec["body"], self.d["sequence"]
        snapshot = self._valid_body(body, rec)
        value = value_id(body)
        def store(d):
            r = d["records"][seq]
            r["applied"], r["apply_count"] = True, 1
            d.update(snapshot=snapshot, anchor=deepcopy(body["anchor"]), parent=value, sequence=seq + 1)
            self._install(d)
        self._persist(store, "application_and_marker")
        self.event("applied", sequence=seq, value=value, result=body["result"])
        self._crash_at("after_application")
        self.votes, self.headers, self.reports, self.offers = {}, {}, {}, set()
        self.deadline = None
        self._retry_pending()

    def retry(self):
        self._retry_delivery()

    def tick(self):
        if not self.alive:
            return
        try:
            if self.clock() >= self.next_retry:
                self.next_retry = self.clock() + PROFILE["timers"]["retry_ticks"]
                self.retry()
            if self.record and self.deadline is not None and self.clock() >= self.deadline:
                self.deadline = None
                self.change_view(self.record["view"] + 1)
            self._drive()
        except NeedData as exc:
            self.last_reason = "NEED_DATA:" + str(exc)
        except Exhausted as exc:
            self._halt(str(exc))
        except Crash:
            pass
