"""Deterministic discrete-event harness. No transport replacement or real clocks.

Only the scheduler/test driver can inspect the population. Replica receives a
bound output port, clock and trace sink; not the simulator or other replicas.
"""
from copy import deepcopy
from dataclasses import dataclass
import random

from fm_application import Application, anchor_chain, initial_anchor, value_id, canonical
from fm_protocol import Authentication, PROFILE, Exhausted
from fm_replica import Replica, Crash


@dataclass
class Event:
    at: int
    serial: int
    source: int
    destination: int
    kind: str
    data: dict


class Simulator:
    def __init__(self, snapshot, n=4, byzantine=(), anchors=None, policy=None):
        self.n, self.now, self.serial = n, 0, 0
        self.authentication = Authentication(n, byzantine)
        self.byzantine = frozenset(byzantine)
        self.events, self.trace, self.delivered, self.adversary_inbox = [], [], 0, []
        self.policy, self.partitions, self.exhausted = policy, None, ""
        self.initial_snapshot = snapshot
        self.initial_anchor = initial_anchor()
        # Explicit synthetic source evidence distributed independently per node.
        evidence = anchor_chain() if anchors is None else anchors
        self.initial_anchors = deepcopy(evidence)  # read-only external audit evidence
        self.nodes = []
        for index in range(n):
            node = Replica(index, n, self.authentication.signer(index), self.authentication.verify,
                           snapshot, self.initial_anchor, self.send, lambda: self.now, self.record)
            node.anchors.update(deepcopy(evidence))
            self.nodes.append(node)

    def record(self, node, event, info):
        if len(self.trace) >= PROFILE["limits"]["trace"]:
            self.exhausted = "TRACE_LIMIT"
            raise Exhausted(self.exhausted)
        self.trace.append({"ordinal": len(self.trace), "time": self.now, "node": node,
                           "event": event, **deepcopy(info)})

    def send(self, source, kind, data, destination=None):
        targets = range(self.n) if destination is None else [destination]
        for target in targets:
            if self.partitions and not any(source in group and target in group for group in self.partitions):
                self.record(source, "network_drop", {"destination": target, "kind": kind, "reason": "partition"})
                continue
            delays = 1 if self.policy is None else self.policy(source, target, kind, deepcopy(data))
            if delays is None:
                self.record(source, "network_drop", {"destination": target, "kind": kind, "reason": "policy"})
                continue
            if type(delays) is int:
                delays = [delays]
            for delay in delays:
                if type(delay) is not int or delay < 0:
                    raise ValueError("BAD_SCHEDULE_DELAY")
                if len(self.events) >= PROFILE["limits"]["events"]:
                    self.exhausted = "EVENT_QUEUE_LIMIT"
                    raise Exhausted(self.exhausted)
                self.serial += 1
                self.events.append(Event(self.now + delay, self.serial, source, target, kind, deepcopy(data)))

    def byzantine_message(self, payload, targets=None):
        signed = self.authentication.adversary_sign(payload)
        for target in (range(self.n) if targets is None else targets):
            self.send(payload["sender"], "SIGNED", signed, target)
        return signed

    def offer(self, batch=None, anchor=None, nodes=None, body=None):
        targets = list(range(self.n)) if nodes is None else list(nodes)
        origin = self.nodes[next(i for i in targets if i not in self.byzantine)]
        body = body or origin.application.build(origin.instance, anchor or origin.d["anchor"], batch or {})
        for i in targets:
            if i not in self.byzantine:
                try:
                    self.nodes[i].offer(body)
                except Crash:
                    pass
        return body

    def deliver(self, event):
        self.events.remove(event)
        self.now = max(self.now, event.at)
        self.delivered += 1
        if self.delivered > PROFILE["limits"]["events"]:
            self.exhausted = "TOTAL_DELIVERY_LIMIT"
            raise Exhausted(self.exhausted)
        self.record(event.destination, "delivered", {"source": event.source, "kind": event.kind})
        if event.destination in self.byzantine:
            self.adversary_inbox.append(deepcopy(event))
            return
        node = self.nodes[event.destination]
        node.receive(event.kind, event.data, event.source)
        node.pump()

    def deliver_where(self, predicate):
        matches = [e for e in self.events if predicate(e)]
        if not matches:
            return False
        self.deliver(min(matches, key=lambda e: (e.at, e.serial)))
        return True

    def run(self, ticks=200, stop=None, chooser=None):
        deadline = self.now + ticks
        steps = 0
        while self.now <= deadline:
            if stop and stop(self):
                return
            # Pump all *own* queued recovery work independently of wire delivery.
            for i, node in enumerate(self.nodes):
                if i not in self.byzantine:
                    node.pump()
            if stop and stop(self):
                return
            due = [e for e in self.events if e.at <= self.now]
            if due:
                event = min(due, key=lambda e: (e.at, e.serial)) if chooser is None else chooser(due)
                self.deliver(event)
            else:
                self.now += 1
                for i, node in enumerate(self.nodes):
                    if i not in self.byzantine:
                        node.tick()
            steps += 1
            if steps > PROFILE["limits"]["events"] * 2:
                self.exhausted = "RUN_WORK_LIMIT"
                raise Exhausted(self.exhausted)

    def settled(self, count=1, nodes=None):
        targets = [i for i in range(self.n) if i not in self.byzantine] if nodes is None else nodes
        return all(self.nodes[i].d["sequence"] >= count for i in targets)

    def published(self, phase=None, view=None, value=None):
        ids = {e["auth"] for e in self.trace if e["event"] == "publish"}
        return [s for s in self.authentication.issued
                if s["auth"] in ids and (phase is None or s["payload"]["phase"] == phase)
                and (view is None or s["payload"]["view"] == view)
                and (value is None or s["payload"]["value"] == value)]

    def retain_trace(self):
        return canonical({"profile": PROFILE["profile_id"], "events": self.trace,
                          "issued": self.authentication.issued}).decode("ascii")


def randomized_policy(seed, maximum_delay=5, drop_percent=10, duplicate_percent=15):
    rng = random.Random(seed)
    def policy(source, destination, kind, data):
        if rng.randrange(100) < drop_percent:
            return None
        first = rng.randrange(1, maximum_delay + 1)
        return [first, first + 1] if rng.randrange(100) < duplicate_percent else first
    return policy
