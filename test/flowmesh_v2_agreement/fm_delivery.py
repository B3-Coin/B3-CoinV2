"""Bounded non-voting delivery; durable journals remain the safety authority.

STATUS is only a hint to fetch the receiver's *own* next sequence. GET_CERT
selects one exact durable record. Neither message changes a vote, view, timer,
lock, application state or quorum. All returned certificates follow the usual
proof/body/application checks. Restart reconstructs these disposable queues.
"""
from collections import OrderedDict
from fm_application import canonical
from fm_protocol import PROFILE, Invalid, integer, exact_id


class DeliveryMixin:
    def _delivery_reset(self):
        self.peer_status = {}
        self.history_requests = OrderedDict()
        self._history_sent = {}
        self._history_peer = 0
        self._delivery_iterators = {}
        self._signature_cursor = 0
        self._accepted_cursor = 0
        self._delivery_tick = None
        self._delivery_in_retry = False
        self.retry_work = self._empty_retry_work()

    @staticmethod
    def _empty_retry_work():
        return dict(records_inspected=0, objects_processed=0,
                    messages_scheduled=0, encoded_bytes=0,
                    scheduled_payload_bytes=0)

    def _delivery_charge(self, field, amount=1):
        limits = PROFILE["delivery"]
        caps = {"records_inspected": limits["records_per_retry"],
                "objects_processed": limits["objects_per_retry"],
                "messages_scheduled": limits["messages_per_retry"],
                "encoded_bytes": limits["payload_bytes_per_retry"],
                "scheduled_payload_bytes": limits["payload_bytes_per_retry"]}
        if self.retry_work[field] + amount > caps[field]:
            return False
        self.retry_work[field] += amount
        return True

    def _delivery_allow_send(self, kind, data, destination):
        # Nested sends caused by intent recovery share this retry interval's
        # budget. Normal receive handlers are outside retry scheduling work.
        if not self._delivery_in_retry or self._delivery_tick != self.clock():
            return True
        limits, work = PROFILE["delivery"], self.retry_work
        fanout = self.n if destination is None else 1
        if work["messages_scheduled"] + fanout > limits["messages_per_retry"]:
            return False
        # Reserve an envelope *before* encoding: a DATA wrapper can combine a
        # bounded proof and bounded body, then be refused for exceeding one
        # wire. Three wire limits cover those two components and the wrapper.
        # The refused encode still counts against the processing bound.
        maximum = PROFILE["limits"]["proof_bytes"]
        if work["encoded_bytes"] + 3 * maximum > limits["payload_bytes_per_retry"]:
            return False
        size = len(canonical(data))
        work["encoded_bytes"] += size
        if size > maximum:
            return False
        if work["scheduled_payload_bytes"] + size * fanout > limits["payload_bytes_per_retry"]:
            return False
        work["messages_scheduled"] += fanout
        work["scheduled_payload_bytes"] += size * fanout
        return True

    def _delivery_record(self, sequence):
        if not self._delivery_charge("records_inspected"):
            return None
        return self.d["records"].get(sequence)

    def _delivery_sample(self, name, mapping, count):
        """Consume bounded iterator steps, with no full-table copy or sorting.

        A table replacement or mutation resets its iterator. Active signatures
        use direct slots instead, so a durable write cannot starve old slots.
        """
        previous, iterator = self._delivery_iterators.get(name, (None, None))
        if previous is not mapping:
            iterator = iter(mapping)
            self._delivery_iterators[name] = (mapping, iterator)
        seen = set()
        for _ in range(min(count, len(mapping))):
            if not self._delivery_charge("objects_processed"):
                return
            try:
                key = next(iterator)
            except (StopIteration, RuntimeError):
                iterator = iter(mapping)
                self._delivery_iterators[name] = (mapping, iterator)
                try:
                    key = next(iterator)
                except StopIteration:
                    return
            if key not in seen:
                seen.add(key)
                yield key

    def _delivery_handle(self, kind, data, source):
        if kind not in ("STATUS", "GET_CERT"):
            return False
        integer(source, self.n - 1)
        if type(data) is not dict:
            raise Invalid("DELIVERY_SHAPE")
        field = "next_sequence" if kind == "STATUS" else "sequence"
        if set(data) != {"config", field}:
            raise Invalid("DELIVERY_SHAPE")
        exact_id(data["config"])
        if data["config"] != self.config:
            raise Invalid("DELIVERY_CONFIG")
        sequence = integer(data[field], PROFILE["limits"]["sequences"] - (kind == "GET_CERT"))
        if kind == "STATUS":
            # At most one integer per member; a forged hint grants no proof.
            self.peer_status[source] = sequence
            if sequence > self.d["sequence"]:
                self._request_history(source)
        elif sequence < self.d["sequence"] or (
                sequence == self.d["sequence"] and self.record
                and self.record["decision"] is not None):
            # One slot per source: repeated requests coalesce without allowing
            # one peer to displace all other peers or allocate an unbounded FIFO.
            old = self.history_requests.get(source)
            if old is not None:
                self.history_requests[source] = min(old, sequence)
            elif len(self.history_requests) < min(self.n, PROFILE["delivery"]["peer_requests"]):
                self.history_requests[source] = sequence
        return True

    def _request_history(self, source):
        integer(source, self.n - 1)
        sequence = self.d["sequence"]
        if source == self.index or sequence >= PROFILE["limits"]["sequences"]:
            return
        self.peer_status[source] = max(self.peer_status.get(source, 0), sequence + 1)
        stamp = (sequence, self.clock())
        if self._history_sent.get(source) == stamp:
            return
        if self._send("GET_CERT", {"config": self.config, "sequence": sequence}, source):
            self._history_sent[source] = stamp

    def _delivery_decision(self, rec, destination=None):
        if not rec or rec["decision"] is None:
            return False
        cert, body = rec["decision"], rec["body"]
        sent = False
        if self._delivery_charge("objects_processed"):
            sent = self._send("CERT", cert, destination)
        if body is not None and self._delivery_charge("objects_processed"):
            self._send("DATA", {"type": "body", "id": cert["prepared"]["proposal"]["payload"]["value"], "object": body,
                                "reference": {"kind": "CERT", "data": cert}}, destination)
        return sent

    def _retry_delivery(self):
        if not self.alive:
            return
        self._delivery_in_retry = True
        try:
            self._retry_delivery_work()
        finally:
            self._delivery_in_retry = False

    def _retry_delivery_work(self):
        if self._delivery_tick != self.clock():
            self._delivery_tick = self.clock()
            self.retry_work = self._empty_retry_work()
        limits = PROFILE["delivery"]

        # Reserve service for one queued historical request before active work
        # can consume the shared budget. FIFO rotation is fair across members.
        if self.history_requests:
            source = next(iter(self.history_requests))
            sequence = self.history_requests[source]
            rec = self._delivery_record(sequence)
            if self._delivery_decision(rec, source):
                self.history_requests.pop(source)
            else:
                self.history_requests.move_to_end(source)

        # Reserve small missing-data and history-discovery messages before
        # large current proofs can consume this interval's byte allowance.
        self._delivery_discovery()

        rec = self._delivery_record(self.d["sequence"])
        if rec:
            # Only the bounded current record is inspected by intent recovery;
            # it contains at most phases * views slots, never historical slots.
            if self._delivery_charge("objects_processed", len(rec["intents"])):
                self._resume_intents()
            if rec["decision"] is not None:
                self._delivery_decision(rec)
            else:
                phases = ("PROPOSE", "PREPARE", "COMMIT", "VIEW_CHANGE", "NEW_VIEW")
                current = [(phase, rec["view"]) for phase in phases]
                slots = PROFILE["limits"]["views"] * len(phases)
                extra = limits["signatures_per_retry"] - len(current)
                chosen = current + [(phases[(self._signature_cursor + offset) % len(phases)],
                                     (self._signature_cursor + offset) // len(phases) %
                                     PROFILE["limits"]["views"]) for offset in range(max(0, extra))]
                self._signature_cursor = (self._signature_cursor + max(0, extra)) % slots
                seen = set()
                for slot in chosen:
                    if not self._delivery_charge("objects_processed"):
                        break
                    signed = rec["signed"].get(slot)
                    if signed is not None and slot not in seen:
                        self._send("SIGNED", signed)
                    seen.add(slot)
                # Acceptance is bounded by views. Direct view lookup permits
                # fair replay even when durable writes replace the dictionary.
                accepted_views = [rec["view"]] + [
                    (self._accepted_cursor + offset) % PROFILE["limits"]["views"]
                    for offset in range(limits["accepted_per_retry"] - 1)]
                self._accepted_cursor = (self._accepted_cursor + limits["accepted_per_retry"] - 1) % PROFILE["limits"]["views"]
                seen_views = set()
                for view in accepted_views:
                    if not self._delivery_charge("objects_processed"):
                        break
                    if view in seen_views:
                        continue
                    seen_views.add(view)
                    accepted = rec["accepted"].get(view)
                    if accepted:
                        body = accepted["body"]
                        self._send("DATA", {"type": "body", "id": accepted["signed"]["payload"]["value"], "object": body,
                                            "reference": {"kind": "SIGNED", "data": accepted["signed"]}})

        for value in self._delivery_sample("offers", self.offers, limits["offers_per_retry"]):
            if value in self.bodies:
                self._send("OFFER", self.bodies[value])

        if self.d["sequence"] > 0:
            self._delivery_decision(self._delivery_record(self.d["sequence"] - 1))

        remaining = limits["objects_per_retry"] - self.retry_work["objects_processed"]
        pending = min(len(self.pending), limits["pending_per_retry"], remaining)
        if pending:
            self._delivery_charge("objects_processed", pending)
            self._retry_pending(limit=pending)
        self.event("retry_work", **self.retry_work)

    def _delivery_discovery(self):
        requests = getattr(self, "requests", {})
        for key in self._delivery_sample("requests", requests, PROFILE["delivery"]["requests_per_retry"]):
            request = requests[key]
            if request["sequence"] != self.d["sequence"] or request["expires"] <= self.clock():
                continue
            # Replay the already validated exact request. Pending-proof replay
            # or local intent recovery can renew its lease after revalidation;
            # selecting a retry does not rescan every admission table.
            self._send("GET", {"type": request["type"], "id": request["id"]})

        # A tiny status advert discovers missed history even after all prior
        # certificates and volatile request queues have disappeared on restart.
        if self._delivery_charge("objects_processed"):
            self._send("STATUS", {"config": self.config, "next_sequence": self.d["sequence"]})
        for _ in range(self.n):
            peer = self._history_peer
            self._history_peer = (self._history_peer + 1) % self.n
            if not self._delivery_charge("objects_processed"):
                break
            if self.peer_status.get(peer, 0) > self.d["sequence"] and peer != self.index:
                self._request_history(peer)
                break
