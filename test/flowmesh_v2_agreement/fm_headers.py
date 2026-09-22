"""Bounded volatile proposal headers; never an authority to forget a vote.

Budgets account for canonical slot+signed-header bytes, not Python heap size.
Durable signing/proof history and the independent checker remain separate.
"""
from copy import deepcopy

from fm_application import canonical
from fm_protocol import PROFILE, Invalid, Exhausted


class HeaderMixin:
    @staticmethod
    def _empty_header_work():
        return dict(headers_inspected=0, protected_entries_inspected=0,
                    recovery_entries_inspected=0, headers_removed=0,
                    bytes_removed=0, canonical_bytes_counted=0,
                    vote_slots_checked=0, references_inspected=0,
                    reference_liveness_checks=0,
                    requests_inspected=0, pending_inspected=0,
                    headers_retained=0, bytes_retained=0)

    def _headers_reset(self):
        self.headers, self.header_sizes = {}, {}
        self.header_bytes = 0
        self.last_header_work = self._empty_header_work()

    def _protected_header_slots(self, work):
        protected, inspected = self._protected_vote_slots()
        work["protected_entries_inspected"] += inspected
        if self.record and self.record["decision"] is not None:
            p = self.record["decision"]["prepared"]["proposal"]["payload"]
            protected.add((p["view"], p["value"]))
            work["protected_entries_inspected"] += 1
        # These wires entered pending only after _need authenticated the full
        # proof and checked its exact context. Raw peer priority/strong flags
        # and pending ordinary proposals/NEW_VIEWs do not grant protection.
        for wire in self.pending.values():
            work["recovery_entries_inspected"] += 1
            if wire["kind"] == "PREPARED":
                proposal = wire["data"]["proposal"]
            elif wire["kind"] == "CERT":
                proposal = wire["data"]["prepared"]["proposal"]
            else:
                continue
            p = proposal["payload"]
            if self.record and p["instance"] == self.record["instance"]:
                protected.add((p["view"], p["value"]))
        return protected

    def _drop_headers(self, keys, protected, work, reason):
        if not keys:
            return
        removed = set()
        for key in keys:
            if key in protected or key not in self.headers:
                continue
            size = self.header_sizes.pop(key)
            self.headers.pop(key)
            self.header_bytes -= size
            removed.add(key)
            work["headers_removed"] += 1
            work["bytes_removed"] += size
            # No scan of either vote history or announcement history.
            for phase in ("PREPARE", "COMMIT"):
                work["vote_slots_checked"] += 1
                self.votes.pop((*key, phase), None)
            self._header_removed(key)
        if not removed:
            return
        # One bounded pass per associated disposable table, even when a
        # single large header requires several evictions. Never touch the
        # issued-signature audit, durable records, retained bodies or offers.
        protected_values = {value for _, value in protected}
        live_values = {value for _, value in self.headers}
        work["headers_inspected"] += len(self.headers)
        retired_values = {value for _, value in removed} - protected_values - live_values
        for value, ref in list(self.references.items()):
            work["references_inspected"] += 1
            if value in retired_values and (ref["view"], value) in removed:
                self.references.pop(value)
        # Keep requests/pending ordinary traffic if another matching current
        # reference still makes it useful; complete verified proofs are never
        # discarded here, even if their header is absent from this cache.
        retired_without_reference = set()
        for value in retired_values:
            work["reference_liveness_checks"] += 1
            ref = self.references.get(value)
            if not ref or not self._vote_reference_live((ref["view"], value, "PREPARE")):
                retired_without_reference.add(value)
        retired_values = retired_without_reference
        for key, request in list(self.requests.items()):
            work["requests_inspected"] += 1
            if request["value"] in retired_values:
                self.requests.pop(key)
        for key, wire in list(self.pending.items()):
            work["pending_inspected"] += 1
            if wire["kind"] == "SIGNED":
                value = wire["data"]["payload"]["value"]
            elif wire["kind"] == "DATA" and wire["data"].get("type") == "body":
                value = wire["data"]["id"]
            else:
                continue
            if value in retired_values:
                self.pending.pop(key)
        self.event("header_cache_removed", reason=reason,
                   count=len(removed), remaining=len(self.headers),
                   canonical_bytes=self.header_bytes)

    def _finish_header_work(self, work):
        work["headers_retained"] = len(self.headers)
        work["bytes_retained"] = self.header_bytes
        self.last_header_work = work

    def _expire_headers_with_work(self, protected, work):
        expired = []
        for key in self.headers:
            work["headers_inspected"] += 1
            if key not in protected and not self._vote_reference_live((*key, "PREPARE")):
                expired.append(key)
        self._drop_headers(expired, protected, work, "expired_reference")

    def _expire_headers(self):
        work = self._empty_header_work()
        protected = self._protected_header_slots(work)
        self._expire_headers_with_work(protected, work)
        self._finish_header_work(work)

    def _retain_header(self, signed, source_kind):
        """Admit one verified current-instance header with bounded maintenance.

        source_kind is a diagnostic label, never a trust or priority input.
        The first exact header at a slot remains stable; a valid alternate
        envelope cannot rewrite an accepted NEW_VIEW or issued instruction.
        False means cache admission was refused, not proposal validation or
        signing success. Verified durable evidence remains separately usable.
        """
        work = self._empty_header_work()
        try:
            try:
                p = self.proofs.check("proposal", signed, self.instance)
            except Exhausted as exc:
                raise Invalid("HEADER_RESOURCE_LIMIT:" + str(exc)) from exc
            key = (p["view"], p["value"])
            protected = self._protected_header_slots(work)
            self._expire_headers_with_work(protected, work)
            if key in self.headers:
                return True
            size = len(canonical({"slot": list(key), "signed": signed}))
            work["canonical_bytes_counted"] += size
            count_cap = PROFILE["admission"]["headers"]
            byte_cap = PROFILE["admission"]["header_bytes"]
            if size > byte_cap:
                self._refuse("HEADER_BYTES_PRESSURE", kind="HEADER_CACHE", admitted=False,
                             source_kind=source_kind, view=p["view"], value=p["value"])
                return False
            remove, remaining_count, remaining_bytes = [], len(self.headers), self.header_bytes
            for old in self.headers:
                if remaining_count + 1 <= count_cap and remaining_bytes + size <= byte_cap:
                    break
                work["headers_inspected"] += 1
                if old in protected:
                    continue
                remove.append(old)
                remaining_count -= 1
                remaining_bytes -= self.header_sizes[old]
            if remaining_count + 1 > count_cap or remaining_bytes + size > byte_cap:
                # Refuse only this volatile cache admission. Complete durable
                # evidence remains available to the caller's exact lookup.
                self._refuse("PROTECTED_HEADER_CAPACITY", kind="HEADER_CACHE", admitted=False,
                             source_kind=source_kind, view=p["view"], value=p["value"])
                return False
            self._drop_headers(remove, protected, work, "cache_pressure")
            self.headers[key] = deepcopy(signed)
            self.header_sizes[key] = size
            self.header_bytes += size
            self.event("header_cache_admitted", source_kind=source_kind,
                       view=p["view"], value=p["value"], count=len(self.headers),
                       canonical_bytes=self.header_bytes)
            return True
        finally:
            self._finish_header_work(work)
