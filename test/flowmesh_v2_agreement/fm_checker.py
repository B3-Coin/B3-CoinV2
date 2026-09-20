"""External safety oracle for the isolated agreement simulator.

This checker never calls Replica or Proofs validation/acceptance methods. It
reads the trusted authentication audit (including unpublished signatures),
independently counts distinct signers, checks nested evidence, and reconciles
all honest durable histories. Application preview is the separately tested,
unchanged accounting hook, not an agreement oracle. This is a finite-state
audit, not a theorem or production cryptographic/storage qualification.
"""
from collections import defaultdict
import json

from fm_application import Application, canonical, digest, normalize_batch, value_id
from fm_protocol import PROFILE


_EXTRAS = {"PROPOSE": {"new_view"}, "PREPARE": set(), "COMMIT": set(),
           "VIEW_CHANGE": {"prepared", "decision"}, "NEW_VIEW": {"reports"}}
_BASE = {"phase", "sender", "instance", "view", "value"}
_INSTANCE = {"profile", "domain", "config", "epoch", "set", "sequence", "parent"}


def _require(condition, reason):
    if not condition:
        raise AssertionError(reason)


def _hash(value):
    return (type(value) is str and len(value) == 64
            and all(c in "0123456789abcdef" for c in value))


class _Audit:
    def __init__(self, sim):
        self.sim, self.n = sim, sim.n
        self.q = 2 * ((self.n - 1) // 3) + 1
        self.config = digest("TEST/V2/CONFIG/1", [PROFILE, list(range(self.n)),
                             sim.initial_snapshot.hex(), sim.initial_anchor])
        self.set_id = digest("TEST/V2/SET/1", list(range(self.n)))
        self.genesis = digest("TEST/V2/GENESIS/1", [self.config, sim.initial_anchor])
        self.tokens = {}
        self.proof_cache = {}
        # Historical anchor evidence belongs to the external audit. In
        # particular, restarting every node may erase all volatile ancestors.
        self.anchor_evidence = {}
        evidence = [sim.initial_anchor]
        evidence.extend(getattr(sim, "initial_anchors", {}).values())
        for node in sim.nodes:
            evidence.extend(node.anchors.values())
            evidence.append(node.d["anchor"])
            evidence.extend(body.get("anchor") for body in node.d["retained_bodies"].values())
            for rec in node.d["records"].values():
                evidence.append(rec["anchor_before"])
                if rec["body"] is not None:
                    evidence.append(rec["body"].get("anchor"))
                evidence.extend(item["body"].get("anchor") for item in rec["accepted"].values())
        for anchor in evidence:
            try:
                self.anchor_header(anchor)
            except (AssertionError, KeyError, TypeError, ValueError):
                continue  # Unused malformed Byzantine/cache traffic has no authority.
            self.anchor_evidence[anchor["hash"]] = anchor

    def instance(self, instance):
        _require(type(instance) is dict and set(instance) == _INSTANCE,
                 "CHECKER_INSTANCE_SHAPE")
        _require(instance["profile"] == PROFILE["profile_id"]
                 and instance["domain"] == PROFILE["domain"]
                 and instance["config"] == self.config
                 and instance["set"] == self.set_id
                 and type(instance["epoch"]) is int and instance["epoch"] == 0,
                 "CHECKER_INSTANCE_DOMAIN")
        _require(type(instance["sequence"]) is int
                 and 0 <= instance["sequence"] < PROFILE["limits"]["sequences"]
                 and _hash(instance["parent"]), "CHECKER_INSTANCE_BOUND")
        return canonical(instance)

    def signed(self, obj, phase=None, instance=None):
        _require(type(obj) is dict and set(obj) == {"payload", "auth"}
                 and type(obj["auth"]) is str
                 and self.tokens.get(obj["auth"]) == obj,
                 "CHECKER_AUTHENTICATION")
        return self.payload(obj["payload"], phase, instance)

    def payload(self, p, phase=None, instance=None):
        _require(type(p) is dict and p.get("phase") in _EXTRAS,
                 "CHECKER_PHASE")
        _require(set(p) == _BASE | _EXTRAS[p["phase"]], "CHECKER_MESSAGE_SHAPE")
        _require(phase is None or p["phase"] == phase, "CHECKER_PHASE_LINK")
        _require(type(p["sender"]) is int and 0 <= p["sender"] < self.n,
                 "CHECKER_SENDER")
        own_instance = self.instance(p["instance"])
        _require(instance is None or own_instance == canonical(instance),
                 "CHECKER_INSTANCE_LINK")
        _require(type(p["view"]) is int
                 and 0 <= p["view"] < PROFILE["limits"]["views"], "CHECKER_VIEW")
        if p["phase"] == "VIEW_CHANGE":
            _require(p["value"] is None and p["view"] > 0, "CHECKER_REPORT_VALUE")
        else:
            _require(_hash(p["value"]), "CHECKER_VALUE")
        return p

    def anchor_header(self, anchor):
        _require(type(anchor) is dict and set(anchor) == {"height", "hash", "parent", "context"}
                 and type(anchor["height"]) is int and 0 <= anchor["height"] <= 0xFFFFFFFF
                 and _hash(anchor["parent"])
                 and anchor["context"] == self.sim.initial_anchor["context"],
                 "CHECKER_ANCHOR_SHAPE")
        _require(anchor["hash"] == digest("TEST/V2/ANCHOR/1",
                 {k: anchor[k] for k in ("height", "parent", "context")}), "CHECKER_ANCHOR_HASH")

    def ancestry(self, anchor, before, local_evidence=None):
        self.anchor_header(anchor)
        self.anchor_header(before)
        _require(anchor["height"] >= before["height"], "CHECKER_ANCHOR_REGRESSION")
        local = local_evidence is not None
        evidence = local_evidence if local else self.anchor_evidence
        current = anchor
        while current["height"] > before["height"]:
            if local:
                # Availability is a property of this replica at first signing,
                # including the candidate header itself. Global/history caches
                # must never supply evidence missing from the copied guard.
                cached = evidence.get(current["hash"])
                _require(cached is not None, "CHECKER_SIGN_WITHOUT_LOCAL_ANCHOR")
                self.anchor_header(cached)
                _require(cached == current, "CHECKER_SIGN_LOCAL_ANCHOR_MISMATCH")
            if current["height"] == before["height"] + 1:
                _require(current["parent"] == before["hash"], "CHECKER_ANCHOR_NOT_DESCENDANT")
                current = before
            else:
                parent = evidence.get(current["parent"])
                _require(parent is not None, "CHECKER_SIGN_WITHOUT_LOCAL_ANCHOR" if local
                         else "CHECKER_ANCHOR_EVIDENCE_MISSING")
                self.anchor_header(parent)
                _require(parent["hash"] == current["parent"],
                         "CHECKER_SIGN_LOCAL_ANCHOR_MISMATCH" if local else "CHECKER_ANCHOR_HASH")
                _require(parent["height"] + 1 == current["height"], "CHECKER_ANCHOR_BROKEN_CHAIN")
                current = parent
        _require(current == before, "CHECKER_ANCHOR_SAME_HEIGHT_FORK")

    def votes(self, votes, phase, proposal):
        _require(type(votes) is list and self.q <= len(votes) <= self.n,
                 "CHECKER_QUORUM_COUNT")
        signers = []
        for vote in votes:
            p = self.signed(vote, phase, proposal["instance"])
            _require((p["view"], p["value"]) == (proposal["view"], proposal["value"]),
                     "CHECKER_VOTE_LINK")
            signers.append(p["sender"])
        _require(signers == sorted(set(signers)), "CHECKER_QUORUM_IDENTITIES")

    def proof(self, kind, obj, instance, depth=0):
        _require(depth <= PROFILE["limits"]["proof_depth"], "CHECKER_PROOF_DEPTH")
        key = (kind, canonical(instance), canonical(obj))
        if key in self.proof_cache:
            return self.proof_cache[key]
        if kind == "prepared":
            _require(type(obj) is dict and set(obj) == {"proposal", "prepares"},
                     "CHECKER_PREPARED_SHAPE")
            p = self.proof("proposal", obj["proposal"], instance, depth + 1)
            self.votes(obj["prepares"], "PREPARE", p)
        elif kind == "commit":
            _require(type(obj) is dict and set(obj) == {"prepared", "commits"},
                     "CHECKER_COMMIT_SHAPE")
            p = self.proof("prepared", obj["prepared"], instance, depth + 1)
            self.votes(obj["commits"], "COMMIT", p)
        elif kind == "proposal":
            p = self.signed(obj, "PROPOSE", instance)
            _require(p["sender"] == (instance["sequence"] + p["view"]) % self.n,
                     "CHECKER_PROPOSER")
            if p["view"] == 0:
                _require(p["new_view"] is None, "CHECKER_UNEXPECTED_NEW_VIEW")
            else:
                nv = self.proof("new_view", p["new_view"], instance, depth + 1)
                _require((nv["view"], nv["value"]) == (p["view"], p["value"]),
                         "CHECKER_NEW_VIEW_LINK")
        elif kind == "view_change":
            p = self.signed(obj, "VIEW_CHANGE", instance)
            for field, nested in (("prepared", "prepared"), ("decision", "commit")):
                if p[field] is not None:
                    old = self.proof(nested, p[field], instance, depth + 1)
                    _require(old["view"] < p["view"], "CHECKER_REPORT_FUTURE_PROOF")
        elif kind == "new_view":
            p = self.signed(obj, "NEW_VIEW", instance)
            _require(p["view"] > 0
                     and p["sender"] == (instance["sequence"] + p["view"]) % self.n,
                     "CHECKER_NEW_VIEW_PROPOSER")
            reports = p["reports"]
            _require(type(reports) is list and self.q <= len(reports) <= self.n,
                     "CHECKER_REPORT_QUORUM")
            signers, prepared = [], []
            for report in reports:
                rp = self.proof("view_change", report, instance, depth + 1)
                _require(rp["view"] == p["view"], "CHECKER_REPORT_TARGET")
                _require(rp["decision"] is None, "CHECKER_NEW_VIEW_IGNORES_DECISION")
                signers.append(rp["sender"])
                if rp["prepared"] is not None:
                    prepared.append(self.proof("prepared", rp["prepared"], instance, depth + 1))
            _require(signers == sorted(set(signers)), "CHECKER_REPORT_IDENTITIES")
            if prepared:
                highest = max(old["view"] for old in prepared)
                values = {old["value"] for old in prepared if old["view"] == highest}
                _require(values == {p["value"]}, "CHECKER_HIGHEST_PREPARED_SELECTION")
        else:
            raise AssertionError("CHECKER_UNKNOWN_PROOF")
        self.proof_cache[key] = p
        return p

    def body(self, body, rec, value, local_anchors=None):
        _require(type(body) is dict and set(body) == {"instance", "anchor", "batch", "result"},
                 "CHECKER_BODY_SHAPE")
        _require(canonical(body["instance"]) == canonical(rec["instance"])
                 and value_id(body) == value, "CHECKER_BODY_IDENTITY")
        _require(normalize_batch(body["batch"]) == body["batch"], "CHECKER_BODY_CANONICAL")
        anchor = body["anchor"]
        self.ancestry(anchor, rec["anchor_before"], local_anchors)
        for field in ("deposits", "settlements"):
            for fact in body["batch"][field]:
                _require(type(fact.get("height")) is int and 0 <= fact["height"] <= anchor["height"],
                         "CHECKER_FACT_BEYOND_ANCHOR")
        snapshot = Application(rec["before"]).preview(body["batch"])
        _require(Application(snapshot).root == body["result"], "CHECKER_RESULT_ROOT")
        return snapshot

    def signing_guard(self, p, guard):
        """Validate copied historical objects, never implementation booleans."""
        _require(canonical(guard["instance"]) == canonical(p["instance"])
                 and guard["view"] == p["view"], "CHECKER_SIGNING_CONTEXT")
        _require(not guard["fenced"] and not guard["halt"] and guard["decision"] is None,
                 "CHECKER_SIGNING_UNAVAILABLE")
        phase, view = p["phase"], p["view"]
        expected_mode = "CHANGING" if phase in ("VIEW_CHANGE", "NEW_VIEW") else "ACTIVE"
        _require(guard["mode"] == expected_mode, "CHECKER_SIGNING_MODE")
        if phase in ("PROPOSE", "PREPARE", "COMMIT", "NEW_VIEW"):
            _require(guard.get("body") is not None, "CHECKER_SIGN_WITHOUT_LOCAL_BODY")
            anchors = guard.get("anchors")
            _require(type(anchors) is dict and len(anchors) <= PROFILE["limits"]["objects"],
                     "CHECKER_SIGN_LOCAL_ANCHOR_CACHE")
            rec = self.sim.nodes[p["sender"]].d["records"][p["instance"]["sequence"]]
            self.body(guard["body"], rec, p["value"], local_anchors=anchors)
        if phase in ("PREPARE", "COMMIT"):
            accepted = guard["accepted"]
            _require(accepted is not None, "CHECKER_SIGN_BEFORE_ACCEPTED_PROPOSAL")
            prop = self.proof("proposal", accepted["signed"], p["instance"])
            _require((prop["view"], prop["value"]) == (view, p["value"]),
                     "CHECKER_SIGN_ACCEPTED_LINK")
            _require(accepted["body"] == guard["body"], "CHECKER_SIGN_BODY_ACCEPTED_LINK")
            if view:
                _require(guard["new_view"] is not None
                         and guard["new_view"] == prop["new_view"],
                         "CHECKER_SIGN_BEFORE_NEW_VIEW")
                self.proof("new_view", guard["new_view"], p["instance"])
            if phase == "COMMIT":
                _require(guard["prepared"] is not None, "CHECKER_SIGN_BEFORE_PREPARED_QUORUM")
                prepared = self.proof("prepared", guard["prepared"], p["instance"])
                _require((prepared["view"], prepared["value"]) == (view, p["value"]),
                         "CHECKER_SIGN_PREPARED_LINK")
        elif phase == "VIEW_CHANGE":
            _require(guard["highest"] == p["prepared"], "CHECKER_SIGN_REPORT_OMITS_HIGHEST")
            if guard["highest"] is not None:
                prepared = self.proof("prepared", guard["highest"], p["instance"])
                _require(prepared["view"] < view, "CHECKER_SIGN_REPORT_FUTURE_PREPARATION")
        elif phase == "PROPOSE" and view:
            _require(guard["new_view"] is not None and guard["new_view"] == p["new_view"],
                     "CHECKER_SIGN_BEFORE_NEW_VIEW")

    def temporal(self, honest):
        """Audit actual persistence/signing/publication order, including retries."""
        intents, guarded, durable, published = {}, set(), set(), set()
        for ordinal, event in enumerate(self.sim.trace):
            _require(event["ordinal"] == ordinal, "CHECKER_TRACE_ORDER")
            node = event["node"]
            if node in self.sim.byzantine:
                continue
            kind = event["event"]
            reason = event.get("reason", "")
            if kind == "durable" and reason.startswith("intent:"):
                p = self.payload(event["payload"])
                _require(p["sender"] == node and reason == "intent:" + p["phase"],
                         "CHECKER_INTENT_EVENT_LINK")
                key = (node, canonical(p["instance"]), p["phase"], p["view"])
                encoded = canonical(p)
                _require(key not in intents or intents[key] == encoded, "CHECKER_HISTORICAL_INTENT_CONFLICT")
                intents[key] = encoded
            elif kind == "pre_sign":
                p = self.payload(event["payload"])
                _require(p["sender"] == node, "CHECKER_SIGN_EVENT_IDENTITY")
                encoded = canonical(p)
                key = (node, canonical(p["instance"]), p["phase"], p["view"])
                _require(intents.get(key) == encoded, "CHECKER_SIGN_BEFORE_DURABLE_INTENT")
                self.signing_guard(p, event["guard"])
                guarded.add((node, encoded))
            elif kind == "durable" and reason.startswith("signature:"):
                signed = self.tokens.get(event["auth"])
                _require(signed is not None and signed["payload"]["sender"] == node,
                         "CHECKER_SIGNATURE_EVENT_IDENTITY")
                p = signed["payload"]
                _require(reason == "signature:" + p["phase"], "CHECKER_SIGNATURE_EVENT_PHASE")
                _require((node, canonical(p)) in guarded, "CHECKER_SIGNATURE_BEFORE_GUARD")
                durable.add(event["auth"])
            elif kind == "publish":
                token = event["auth"]
                signed = self.tokens.get(token)
                _require(signed is not None and signed["payload"]["sender"] == node,
                         "CHECKER_PUBLISHED_UNAUTHENTICATED")
                p = signed["payload"]
                _require((p["phase"], p["view"], p["instance"]["sequence"])
                         == (event["phase"], event["view"], event["sequence"]),
                         "CHECKER_PUBLICATION_LINK")
                _require(token in durable, "CHECKER_PUBLISH_BEFORE_DURABLE_SIGNATURE")
                published.add(token)
        for signed, p in honest:
            _require((p["sender"], canonical(p)) in guarded, "CHECKER_ISSUED_WITHOUT_HISTORICAL_GUARD")
            _require(signed["auth"] in durable, "CHECKER_ISSUED_WITHOUT_DURABLE_EVENT")
        return published

    def run(self):
        sim = self.sim
        _require(self.n in (4, 7) and len(sim.byzantine) <= (self.n - 1) // 3,
                 "CHECKER_FAULT_MODEL")
        # Authentication is the only trusted simulator facility used here. Its
        # audit includes signatures generated before a crash or never delivered.
        for signed in sim.authentication.issued:
            _require(sim.authentication.verify(signed), "CHECKER_AUDIT_AUTHENTICATION")
            token = signed["auth"]
            _require(token not in self.tokens or self.tokens[token] == signed,
                     "CHECKER_AUTH_TOKEN_COLLISION")
            self.tokens[token] = signed
        groups, honest, slots = defaultdict(set), [], {}
        ignored = 0
        for signed in sim.authentication.issued:
            raw = signed.get("payload", {})
            try:
                p = self.signed(signed)
            except (AssertionError, KeyError, TypeError, ValueError):
                _require(raw.get("sender") in sim.byzantine, "CHECKER_MALFORMED_HONEST_SIGNATURE")
                ignored += 1
                continue
            ikey = canonical(p["instance"])
            if p["phase"] in ("PREPARE", "COMMIT"):
                groups[(p["phase"], ikey, p["view"], p["value"])].add(p["sender"])
            if p["sender"] not in sim.byzantine:
                slot = (p["sender"], ikey, p["phase"], p["view"])
                _require(slot not in slots or slots[slot] == canonical(p),
                         "CHECKER_HONEST_SLOT_EQUIVOCATION")
                slots[slot] = canonical(p)
                honest.append((signed, p))

        possible, prepared_slots = {}, {}
        counts = {"PREPARE": 0, "COMMIT": 0}
        for (phase, ikey, view, value), signers in groups.items():
            if len(signers) < self.q:
                continue
            counts[phase] += 1
            if phase == "PREPARE":
                slot = (ikey, view)
                _require(slot not in prepared_slots or prepared_slots[slot] == value,
                         "CHECKER_CONFLICTING_PREPARE_QUORUMS")
                prepared_slots[slot] = value
            else:
                # Deliberately conservative: q authenticated COMMIT votes count
                # even if nobody assembled/published a full certificate. Honest
                # signer guards are checked separately below. Grouping by the
                # configured sequence ALSO catches conflicting parent contexts.
                sequence = json.loads(ikey)["sequence"]
                candidate = (ikey, value)
                _require(sequence not in possible or possible[sequence] == candidate,
                         "CHECKER_CONFLICTING_POSSIBLE_COMMIT_QUORUMS")
                possible[sequence] = candidate

        last_view, own_commits = {}, {}
        for signed, p in honest:
            node = sim.nodes[p["sender"]]
            seq, view = p["instance"]["sequence"], p["view"]
            rec = node.d["records"].get(seq)
            slot = (p["phase"], view)
            _require(rec is not None and canonical(rec["instance"]) == canonical(p["instance"]),
                     "CHECKER_SIGNED_HISTORY_MISSING")
            _require(rec["signed"].get(slot) == signed, "CHECKER_SIGNATURE_NOT_DURABLE")
            _require(rec["intents"].get(slot) == p, "CHECKER_INTENT_NOT_DURABLE")
            key = (p["sender"], canonical(p["instance"]))
            _require(view >= last_view.get(key, 0), "CHECKER_FRESH_SIGNATURE_IN_ABANDONED_VIEW")
            last_view[key] = view
            if p["phase"] in ("PREPARE", "COMMIT"):
                accepted = rec["accepted"].get(view)
                _require(accepted is not None, "CHECKER_VOTE_WITHOUT_ACCEPTED_PROPOSAL")
                proposal = self.proof("proposal", accepted["signed"], rec["instance"])
                _require((proposal["view"], proposal["value"]) == (view, p["value"]),
                         "CHECKER_VOTE_ACCEPTED_LINK")
                self.body(accepted["body"], rec, p["value"])
                if view:
                    nv = rec["new_views"].get(view)
                    _require(nv is not None and nv == proposal["new_view"],
                             "CHECKER_VOTE_WITHOUT_EXACT_DURABLE_NEW_VIEW")
                    self.proof("new_view", nv, rec["instance"])
                if p["phase"] == "COMMIT":
                    qc = rec["prepared"].get((view, p["value"]))
                    _require(qc is not None, "CHECKER_COMMIT_WITHOUT_DURABLE_PREPARED")
                    qp = self.proof("prepared", qc, rec["instance"])
                    _require((qp["view"], qp["value"]) == (view, p["value"]),
                             "CHECKER_COMMIT_PREPARED_LINK")
                    own_commits[key] = view
            elif p["phase"] == "VIEW_CHANGE":
                self.proof("view_change", signed, rec["instance"])
                if key in own_commits:
                    evidence = p["prepared"]
                    _require(evidence is not None or p["decision"] is not None,
                             "CHECKER_VIEW_CHANGE_FORGETS_COMMIT_PREPARATION")
                    if evidence is not None:
                        _require(evidence["proposal"]["payload"]["view"] >= own_commits[key],
                                 "CHECKER_VIEW_CHANGE_DOWNGRADES_COMMIT_PREPARATION")
            else:
                self.proof("proposal" if p["phase"] == "PROPOSE" else "new_view",
                           signed, rec["instance"])

        published = self.temporal(honest)

        decisions, prefixes, applied = {}, {}, 0
        for node in sim.nodes:
            if node.index in sim.byzantine:
                continue
            d = node.d
            _require(type(d["sequence"]) is int
                     and 0 <= d["sequence"] <= PROFILE["limits"]["sequences"],
                     "CHECKER_SEQUENCE_BOUND")
            snapshot, parent, anchor = sim.initial_snapshot, self.genesis, sim.initial_anchor
            expected = min(d["sequence"] + 1, PROFILE["limits"]["sequences"])
            _require(sorted(d["records"]) == list(range(expected)), "CHECKER_DURABLE_HISTORY_GAP")
            for seq, rec in sorted(d["records"].items()):
                instance = rec["instance"]
                ikey = self.instance(instance)
                _require(instance["sequence"] == seq and instance["parent"] == parent,
                         "CHECKER_PARENT_CONTINUITY")
                _require(rec["before"] == snapshot and rec["anchor_before"] == anchor,
                         "CHECKER_SNAPSHOT_CONTINUITY")
                prefix = (ikey, rec["before"], canonical(rec["anchor_before"]))
                _require(seq not in prefixes or prefixes[seq] == prefix,
                         "CHECKER_HONEST_PREFIX_DIVERGENCE")
                prefixes[seq] = prefix
                _require(type(rec["applied"]) is bool and type(rec["apply_count"]) is int
                         and rec["apply_count"] == int(rec["applied"]), "CHECKER_APPLY_ONCE")
                for view, accepted in rec["accepted"].items():
                    p = self.proof("proposal", accepted["signed"], instance)
                    _require(p["view"] == view, "CHECKER_ACCEPTED_VIEW_KEY")
                    self.body(accepted["body"], rec, p["value"])
                for view, nv in rec["new_views"].items():
                    _require(self.proof("new_view", nv, instance)["view"] == view,
                             "CHECKER_NEW_VIEW_KEY")
                prepared_views = []
                for key, qc in rec["prepared"].items():
                    p = self.proof("prepared", qc, instance)
                    _require(key == (p["view"], p["value"]), "CHECKER_PREPARED_KEY")
                    prepared_views.append(p["view"])
                if prepared_views:
                    _require(rec["highest"] is not None, "CHECKER_HIGHEST_MISSING")
                    highest = self.proof("prepared", rec["highest"], instance)
                    _require(highest["view"] == max(prepared_views), "CHECKER_HIGHEST_DOWNGRADE")
                else:
                    _require(rec["highest"] is None, "CHECKER_HIGHEST_WITHOUT_RETENTION")
                decision = rec["decision"]
                if decision is not None:
                    p = self.proof("commit", decision, instance)
                    candidate = (ikey, p["value"])
                    _require(seq not in decisions or decisions[seq] == candidate,
                             "CHECKER_CONFLICTING_HONEST_DECISIONS")
                    _require(possible.get(seq) == candidate, "CHECKER_DECISION_NOT_IN_AUDIT_QUORUMS")
                    decisions[seq] = candidate
                    resulting = self.body(rec["body"], rec, p["value"])
                if rec["applied"]:
                    _require(decision is not None, "CHECKER_APPLIED_WITHOUT_DECISION")
                    applied += 1
                    snapshot, parent, anchor = resulting, p["value"], rec["body"]["anchor"]
                else:
                    _require(seq == d["sequence"], "CHECKER_UNAPPLIED_PREFIX")
            _require(d["snapshot"] == snapshot and d["parent"] == parent and d["anchor"] == anchor,
                     "CHECKER_LIVE_STATE_NOT_APPLIED_PREFIX")
            _require(sum(int(r["applied"]) for r in d["records"].values()) == d["sequence"],
                     "CHECKER_SEQUENCE_APPLICATION_COUNT")
        return {"issued_signatures": len(self.tokens),
                "withheld_signatures": len(set(self.tokens) - published),
                "ignored_byzantine_malformed": ignored,
                "possible_prepared_quorums": counts["PREPARE"],
                "possible_commit_quorums": counts["COMMIT"],
                "applied_records": applied, "decisions": len(decisions),
                "honest_nodes": self.n - len(sim.byzantine)}


def check(sim):
    """Return an audit summary or raise AssertionError with a concrete reason."""
    try:
        return _Audit(sim).run()
    except AssertionError:
        raise
    except (KeyError, TypeError, ValueError, AttributeError, RecursionError) as exc:
        raise AssertionError("CHECKER_MALFORMED_STATE: " + str(exc)) from exc
