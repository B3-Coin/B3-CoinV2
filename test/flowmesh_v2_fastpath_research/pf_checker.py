"""Independent oracle for the finite, TEST-only P2-FV research model.

No replica, model validator, recovery selector, or cryptographic implementation
is imported.  Acknowledged storage and authentication are supplied audit facts,
not qualifications of real storage or signatures.  Unreleased computed votes
do not constitute a decision or a remote authentication fact.
"""
from collections import defaultdict


SIGNED = frozenset(("PREPARE", "COMMIT", "REPORT", "NEW_VIEW"))
KINDS = SIGNED | frozenset(("PROPOSE", "PC", "FAST", "SLOW", "V0", "EQ", "EMPTY"))


def _need(condition, reason):
    if not condition:
        raise AssertionError("PF_CHECKER_" + reason)


class _Audit:
    def __init__(self, world):
        self.world = world
        self.n = world.n
        _need(type(self.n) is int and self.n in (4, 5, 7), "N")
        self.f = (self.n - 1) // 3
        self.q = self.n - self.f
        self.t = min(self.f, (self.n - 3 * self.f + 1) // 2)
        self.fast = self.n - self.t
        for name, expected in (("f", self.f), ("q", self.q), ("t", self.t), ("F", self.fast)):
            if hasattr(world, name):
                _need(getattr(world, name) == expected, "PARAMETER_" + name)
        self.instance = world.instance
        _need(type(self.instance) is str and bool(self.instance), "INSTANCE")
        self.bad = set(world.byzantine)
        _need(all(type(i) is int and 0 <= i < self.n for i in self.bad)
              and len(self.bad) <= self.f, "FAULT_BUDGET")
        self.published = world.auth.published
        self.computed = world.auth.computed
        _need(self.published <= self.computed, "UNCOMPUTED_PUBLIC_SIGNATURE")
        self.cache = set()
        self.active = set()
        self.decisions = set()
        self.durable_signatures = {}
        self.guards = {}
        self.covered = set()
        self.acknowledged = {}
        self.first_signature_floor = {}

    def base(self, packet):
        _need(all(hasattr(packet, k) for k in ("kind", "view", "sender", "value", "items", "instance")),
              "PACKET_SHAPE")
        _need(packet.kind in KINDS and type(packet.items) is tuple, "PACKET_KIND_ITEMS")
        _need(packet.instance == self.instance, "PACKET_INSTANCE")
        _need(type(packet.view) is int and
              (packet.view == -1 if packet.kind == "EMPTY" else packet.view >= 0), "PACKET_VIEW")
        _need(type(packet.sender) is int, "PACKET_SENDER_TYPE")
        if packet.kind in SIGNED or packet.kind in ("PROPOSE", "V0"):
            _need(0 <= packet.sender < self.n, "PACKET_SENDER")
        if packet.kind in ("REPORT", "EMPTY", "EQ"):
            _need(packet.value is None, "EMPTY_VALUE")
        else:
            _need(type(packet.value) is str and bool(packet.value), "VALUE")

    def packet(self, packet, owner=None):
        """Validate a proof; only its local owner may use unpublished own bytes."""
        self.base(packet)
        key = (packet, owner)
        if key in self.cache:
            return
        _need(key not in self.active, "CYCLIC_PROOF")
        self.active.add(key)
        try:
            if packet.kind in SIGNED:
                _need(packet in self.published or
                      (owner == packet.sender and packet in self.computed), "AUTHENTICATION")
            kind, view, items = packet.kind, packet.view, packet.items
            if kind == "EMPTY":
                _need(not items and packet.sender == -1, "EMPTY_ITEMS_OR_SENDER")
            elif kind in ("PREPARE", "COMMIT"):
                _need(not items, "VOTE_ITEMS")
                if kind == "COMMIT":
                    _need(view > 0 or self.fast > self.q, "VIEW_ZERO_COMMIT_DISABLED")
            elif kind == "PROPOSE":
                _need(len(items) == 2 and packet.sender == view % self.n, "PROPOSER")
                vote, nv = items
                self.packet(vote, owner)
                self.packet(nv, owner)
                _need(vote.kind == "PREPARE" and vote.sender == packet.sender
                      and (vote.view, vote.value) == (view, packet.value), "PROPOSAL_VOTE")
                _need(nv.kind == "EMPTY" if view == 0 else
                      nv.kind == "NEW_VIEW" and (nv.view, nv.value) == (view, packet.value),
                      "PROPOSAL_NEW_VIEW")
            elif kind in ("PC", "FAST", "SLOW"):
                phase = "COMMIT" if kind == "SLOW" else "PREPARE"
                minimum = self.fast if kind == "FAST" else self.q
                _need(minimum <= len(items) <= self.n, "QUORUM_SIZE")
                identities = set()
                for vote in items:
                    self.packet(vote, owner)
                    _need(vote.kind == phase and (vote.view, vote.value) == (view, packet.value),
                          "QUORUM_VOTE_LINK")
                    _need(vote.sender not in identities, "QUORUM_DUPLICATE")
                    identities.add(vote.sender)
                if kind == "FAST":
                    _need(view == 0 and 0 in identities, "FAST_LEADER_OR_VIEW")
                if kind == "SLOW":
                    _need(view > 0 or self.fast > self.q, "VIEW_ZERO_SLOW_DISABLED")
            elif kind == "V0":
                _need(view == 0 and len(items) == 2, "V0_SHAPE")
                voter, leader = items
                for vote in items:
                    self.packet(vote, owner)
                    _need(vote.kind == "PREPARE" and vote.view == 0
                          and vote.value == packet.value, "V0_VOTE_LINK")
                _need(voter.sender == packet.sender and leader.sender == 0, "V0_ORIGINALS")
            elif kind == "EQ":
                _need(view == 0 and len(items) == 2, "EQ_SHAPE")
                for vote in items:
                    self.packet(vote, owner)
                    _need(vote.kind == "PREPARE" and vote.view == 0 and vote.sender == 0,
                          "EQ_ORIGINAL")
                _need(items[0].value != items[1].value, "EQ_CONFLICT")
            elif kind == "REPORT":
                _need(view > 0 and len(items) == 3, "REPORT_SHAPE")
                v0, pc, decision = items
                for item in items:
                    self.packet(item, owner)
                _need(v0.kind == "EMPTY" or v0.kind == "V0" and v0.sender == packet.sender,
                      "REPORT_V0")
                _need(pc.kind == "EMPTY" or pc.kind == "PC" and pc.view < view, "REPORT_PC")
                _need(decision.kind == "EMPTY" or decision.kind in ("FAST", "SLOW")
                      and decision.view < view, "REPORT_DECISION")
            elif kind == "NEW_VIEW":
                _need(view > 0 and packet.sender == view % self.n
                      and len(items) == self.q + 1, "NEW_VIEW_SHAPE")
                evidence, *reports = items
                self.packet(evidence, owner)
                _need(evidence.kind in ("EQ", "EMPTY"), "NEW_VIEW_EVIDENCE")
                identities = set()
                for report in reports:
                    self.packet(report, owner)
                    _need(report.kind == "REPORT" and report.view == view
                          and report.sender not in identities, "NEW_VIEW_REPORTS")
                    identities.add(report.sender)
                originals = {r.items[0].items[1].value for r in reports if r.items[0].kind == "V0"}
                if evidence.kind == "EQ":
                    originals.update(p.value for p in evidence.items)
                equivocation = len(originals) > 1
                _need(not equivocation or 0 not in identities, "NEW_VIEW_COUNTS_EQUIVOCATOR")
                _need(all(r.items[2].kind == "EMPTY" for r in reports), "NEW_VIEW_IGNORES_DECISION")
                pcs = [r.items[1] for r in reports if r.items[1].kind == "PC"]
                if pcs:
                    highest = max(pc.view for pc in pcs)
                    values = {pc.value for pc in pcs if pc.view == highest}
                    _need(len(values) == 1 and packet.value in values, "NEW_VIEW_HIGHEST_PC")
                else:
                    counts = defaultdict(int)
                    for report in reports:
                        if report.items[0].kind == "V0":
                            counts[report.items[0].value] += 1
                    if equivocation:
                        eligible = sorted(v for v, count in counts.items() if count >= self.f + self.t)
                        _need(not eligible or packet.value == eligible[0], "NEW_VIEW_FAST_SELECTION")
                    else:
                        _need(len(counts) <= 1 and (not counts or packet.value in counts),
                              "NEW_VIEW_SINGLE_SELECTION")
            self.cache.add(key)
        finally:
            self.active.remove(key)

    def descendants(self, packet):
        yield packet
        for item in packet.items:
            yield from self.descendants(item)

    def guard(self, sender, packet, durable):
        _need(durable["signed"].get((packet.kind, packet.view)) == packet, "PUBLISH_BEFORE_DURABLE")
        _need(packet in durable["guards"], "MISSING_SIGNING_GUARD")
        guard = durable["guards"][packet]
        fields = {"view", "pre_view", "mode", "proposal", "new_view", "prepared", "highest", "v0",
                  "decision", "fenced", "body_available"}
        _need(type(guard) is dict and fields <= guard.keys(), "GUARD_SHAPE")
        if packet in self.guards:
            _need(self.guards[packet] == guard, "REWRITTEN_GUARD")
        self.guards[packet] = guard
        _need(guard["fenced"] is False and guard["view"] == packet.view, "SIGNING_FENCE_OR_VIEW")
        floor = self.first_signature_floor.get(packet, 0)
        _need(packet.view >= floor, "FIRST_SIGN_BELOW_ACKNOWLEDGED_VIEW")
        _need(type(guard["pre_view"]) is int and floor <= guard["pre_view"] <= packet.view,
              "FIRST_SIGN_BELOW_PRE_SIGN_VIEW")
        _need(guard["pre_view"] < packet.view if packet.kind == "REPORT" else
              guard["pre_view"] == packet.view, "PRE_SIGN_VIEW_TRANSITION")
        for field, kinds in (("highest", ("PC",)), ("v0", ("V0",)),
                             ("decision", ("FAST", "SLOW"))):
            item = guard[field]
            if item is not None:
                self.packet(item, sender)
                _need(item.kind in kinds, "GUARD_" + field.upper())
        if packet.kind == "PREPARE":
            _need(guard["mode"] == "ACTIVE", "PREPARE_MODE")
            proposal = guard["proposal"]
            _need(proposal is not None and guard["body_available"] is True, "PREPARE_WITHOUT_BODY")
            self.packet(proposal, sender)
            _need(proposal.kind == "PROPOSE" and (proposal.view, proposal.value) ==
                  (packet.view, packet.value), "PREPARE_PROPOSAL")
            if packet.view > 0:
                _need(guard["new_view"] == proposal.items[1], "PREPARE_NEW_VIEW")
            _need(guard["decision"] is None or guard["decision"].value == packet.value,
                  "PREPARE_AFTER_CONFLICTING_DECISION")
        elif packet.kind == "COMMIT":
            _need(guard["mode"] == "ACTIVE", "COMMIT_MODE")
            proposal = guard["proposal"]
            _need(proposal is not None and guard["body_available"] is True, "COMMIT_WITHOUT_BODY")
            self.packet(proposal, sender)
            _need(proposal.kind == "PROPOSE" and (proposal.view, proposal.value) ==
                  (packet.view, packet.value), "COMMIT_PROPOSAL")
            if packet.view > 0:
                _need(guard["new_view"] == proposal.items[1], "COMMIT_NEW_VIEW")
            pc = guard["prepared"]
            _need(pc is not None, "COMMIT_WITHOUT_PC")
            self.packet(pc, sender)
            _need(pc.kind == "PC" and (pc.view, pc.value) == (packet.view, packet.value), "COMMIT_PC")
            _need(guard["decision"] is None or guard["decision"].value == packet.value,
                  "COMMIT_AFTER_CONFLICTING_DECISION")
        elif packet.kind == "REPORT":
            _need(guard["mode"] == "CHANGING", "REPORT_MODE")
            for field, item in zip(("v0", "highest", "decision"), packet.items):
                actual = None if item.kind == "EMPTY" else item
                _need(actual == guard[field], "UNTRUTHFUL_REPORT_" + field.upper())
        elif packet.kind == "NEW_VIEW":
            _need(guard["mode"] == "CHANGING", "NEW_VIEW_MODE")
            _need(guard["decision"] is None, "NEW_VIEW_AFTER_DECISION")
        self.covered.add(packet)

    def durable(self, sender, durable):
        _need(type(durable) is dict and type(durable["signed"]) is dict
              and type(durable["guards"]) is dict, "DURABLE_SHAPE")
        _need(type(durable["view"]) is int and durable["view"] >= 0
              and type(durable["fenced"]) is bool, "DURABLE_VIEW_FENCE")
        previous_view, previous_apply_count = self.acknowledged.get(sender, (0, 0))
        _need(durable["view"] >= previous_view, "ACKNOWLEDGED_VIEW_REGRESSION")
        for key, packet in durable["signed"].items():
            self.base(packet)
            _need(packet.kind in SIGNED and packet.sender == sender
                  and key == (packet.kind, packet.view) and packet in self.computed,
                  "DURABLE_SIGNATURE_RECORD")
            identity = (sender, packet.kind, packet.view)
            _need(identity not in self.durable_signatures or self.durable_signatures[identity] == packet,
                  "CONFLICTING_DURABLE_SIGNATURES")
            if identity not in self.durable_signatures:
                self.first_signature_floor[packet] = previous_view
            self.durable_signatures[identity] = packet
        _need(type(durable["apply_count"]) is int and 0 <= durable["apply_count"] <= 1,
              "DUPLICATE_APPLICATION")
        _need(durable["apply_count"] >= previous_apply_count, "ACKNOWLEDGED_APPLICATION_REGRESSION")
        self.acknowledged[sender] = (durable["view"], durable["apply_count"])
        decision, applied = durable["decision"], durable["applied"]
        _need((applied is None) == (durable["apply_count"] == 0), "APPLICATION_COUNT")
        if decision is not None:
            self.packet(decision, sender)
            _need(decision.kind in ("FAST", "SLOW"), "DURABLE_DECISION_KIND")
            self.decisions.add(decision.value)
        if applied is not None:
            _need(decision is not None and applied == decision.value, "APPLICATION_WITHOUT_DECISION")
        for view, proposal in durable["accepted"].items():
            self.packet(proposal, sender)
            _need(proposal.kind == "PROPOSE" and view == proposal.view, "ACCEPTED_PROPOSAL")
        for view, nv in durable["new_views"].items():
            self.packet(nv, sender)
            _need(nv.kind == "NEW_VIEW" and view == nv.view, "ACCEPTED_NEW_VIEW")
        for field, kind in (("highest", "PC"), ("v0", "V0")):
            item = durable[field]
            if item is not None:
                self.packet(item, sender)
                _need(item.kind == kind, "DURABLE_" + field.upper())
                if field == "v0":
                    _need(item.sender == sender, "DURABLE_V0_OWNER")
        original = durable['signed'].get(('PREPARE', 0))
        v0 = durable['v0']
        if original is not None:
            _need(v0 is not None and v0.items[0] == original, "LOST_ORIGINAL_VOTE")
            proposal = durable['accepted'].get(0)
            _need(proposal is not None and proposal.items[0] == v0.items[1],
                  "LOST_ORIGINAL_LEADER_EVIDENCE")
        else:
            _need(v0 is None, "V0_WITHOUT_LOCAL_SIGNING_RECORD")
        # A coherently rewritten entire history remains outside this oracle.
        # Within the supplied history, dropping a prior record is observable.
        for (author, kind, view), packet in self.durable_signatures.items():
            if author == sender:
                _need(durable['signed'].get((kind, view)) == packet, "LOST_SIGNING_RECORD")

    def run(self):
        # Malformed Byzantine traffic may be rejected by honest replicas.  It
        # is not an oracle failure unless an honest record carries/accepts it.
        for record in self.world.publications:
            sender, packet, durable = record["sender"], record["packet"], record["durable"]
            _need(type(sender) is int and 0 <= sender < self.n, "PUBLICATION_SENDER")
            if sender in self.bad:
                continue
            self.packet(packet)
            if packet.kind in SIGNED:
                _need(packet.sender == sender, "PUBLICATION_AUTHOR")
            self.durable(sender, durable)
            for item in self.descendants(packet):
                if item.kind in SIGNED and item.sender == sender:
                    self.guard(sender, item, durable)
                if item.kind in ("FAST", "SLOW"):
                    self.decisions.add(item.value)
        nodes = self.world.nodes.items() if isinstance(self.world.nodes, dict) else enumerate(self.world.nodes)
        for sender, node in nodes:
            if sender not in self.bad:
                self.durable(sender, node.d)
        votes = defaultdict(set)
        for packet in self.published:
            try:
                self.base(packet)
            except (AssertionError, AttributeError, TypeError):
                if getattr(packet, "sender", None) not in self.bad:
                    raise
                continue
            if packet.sender not in self.bad:
                _need(packet in self.covered, "UNBACKED_HONEST_PUBLICATION")
            if packet.kind in ("PREPARE", "COMMIT") and not packet.items:
                votes[(packet.kind, packet.view, packet.value)].add(packet.sender)
        for (kind, view, value), identities in votes.items():
            if kind == "PREPARE" and view == 0 and 0 in identities and len(identities) >= self.fast:
                self.decisions.add(value)
            if kind == "COMMIT" and (view > 0 or self.fast > self.q) and len(identities) >= self.q:
                self.decisions.add(value)
        _need(len(self.decisions) <= 1, "CONFLICTING_DECISIONS")


def check(world):
    """Raise AssertionError for a violated finite-model invariant; else None."""
    try:
        _Audit(world).run()
    except (AttributeError, KeyError, TypeError, ValueError, RecursionError) as exc:
        raise AssertionError("PF_CHECKER_MALFORMED_AUDIT_STATE") from exc
