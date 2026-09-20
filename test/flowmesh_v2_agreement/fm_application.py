"""Narrow TEST ONLY agreement hook around the frozen accounting model.

Every preview restores a separate accounting snapshot. Only apply_snapshot
installs a result; this module neither votes nor decides when installation is
authorized. Facts, risk, custody capacity and finalized headers are synthetic
test inputs. Header hashes check this deterministic fixture format, not B3
signatures, fork choice, import completeness or live custody proofs.
"""
from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys


_ACCOUNTING = Path(__file__).resolve().parents[1] / "flowmesh_v2_model"
if str(_ACCOUNTING) not in sys.path:
    sys.path.insert(0, str(_ACCOUNTING))
from model import Model, ModelError  # noqa: E402


ANCHOR_CONTEXT = "synthetic-b3-finalized/1"
INITIAL_ANCHOR_HEIGHT = 855499
_ARRAY_FIELDS = ("deposits", "settlements", "actions")
_MAP_FIELDS = ("capacities", "risk")
_BATCH_FIELDS = set(_ARRAY_FIELDS + _MAP_FIELDS)


class NeedData(Exception):
    """Exact referenced evidence is unavailable in this replica's cache."""

    def __init__(self, identity: str):
        self.identity = identity
        super().__init__(identity)


class InvalidValue(ValueError):
    """A value or its available evidence fails deterministic validation."""

    def __init__(self, reason: str):
        self.reason = reason
        super().__init__(reason)


def canonical(obj) -> bytes:
    """Strict JSON test codec; bool/int and absent/empty remain distinct."""
    def inspect(value):
        if value is None or type(value) in (str, int, bool):
            return
        if type(value) is list:
            for item in value:
                inspect(item)
            return
        if type(value) is dict and all(type(key) is str for key in value):
            for item in value.values():
                inspect(item)
            return
        raise InvalidValue("NON_CANONICAL_TYPE")

    inspect(obj)
    return json.dumps(obj, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("ascii")


def digest(tag, obj) -> str:
    return hashlib.sha256(canonical([tag, obj])).hexdigest()


def value_id(body) -> str:
    return digest("TEST/V2/VALUE/1", body)


def _exact_hash(value, reason="BAD_EXACT_HASH"):
    if (type(value) is not str or len(value) != 64
            or any(char not in "0123456789abcdef" for char in value)):
        raise InvalidValue(reason)
    return value


def make_anchor(height: int, parenthash: str) -> dict:
    """Mint a deterministic synthetic header; this is not authentication."""
    if type(height) is not int or not 0 <= height <= 0xFFFFFFFF:
        raise InvalidValue("BAD_ANCHOR_HEIGHT")
    _exact_hash(parenthash, "BAD_ANCHOR_PARENT")
    fields = {"height": height, "parent": parenthash,
              "context": ANCHOR_CONTEXT}
    return dict(fields, hash=digest("TEST/V2/ANCHOR/1", fields))


def initial_anchor() -> dict:
    return make_anchor(INITIAL_ANCHOR_HEIGHT, "0" * 64)


def anchor_chain(height: int = 855501) -> dict:
    """Return hash -> header fixture evidence, including the trusted start."""
    if type(height) is not int or not INITIAL_ANCHOR_HEIGHT <= height <= 0xFFFFFFFF:
        raise InvalidValue("BAD_ANCHOR_HEIGHT")
    current = initial_anchor()
    chain = {current["hash"]: current}
    for next_height in range(INITIAL_ANCHOR_HEIGHT + 1, height + 1):
        current = make_anchor(next_height, current["hash"])
        chain[current["hash"]] = current
    return chain


def _check_anchor(header):
    if type(header) is not dict or set(header) != {"height", "hash", "parent", "context"}:
        raise InvalidValue("BAD_ANCHOR_SHAPE")
    _exact_hash(header["hash"], "BAD_ANCHOR_HASH")
    if header["context"] != ANCHOR_CONTEXT:
        raise InvalidValue("BAD_ANCHOR_CONTEXT")
    expected = make_anchor(header["height"], header["parent"])
    if canonical(header) != canonical(expected):
        raise InvalidValue("BAD_SYNTHETIC_ANCHOR_HASH")


def normalize_batch(batch: dict) -> dict:
    """Normalize only the five frozen-model public batch input fields."""
    if type(batch) is not dict or set(batch) - _BATCH_FIELDS:
        raise InvalidValue("BAD_BATCH_FIELDS")
    canonical(batch)
    result = {}
    for field in _ARRAY_FIELDS:
        items = batch.get(field, [])
        if type(items) is not list or any(type(item) is not dict for item in items):
            raise InvalidValue("BAD_BATCH_ARRAY")
        unique = {canonical(item): deepcopy(item) for item in items}
        result[field] = [unique[key] for key in sorted(unique)]
    for field in _MAP_FIELDS:
        items = batch.get(field, {})
        if type(items) is not dict:
            raise InvalidValue("BAD_BATCH_MAP")
        result[field] = deepcopy(items)
    if any(type(fact) is not dict for fact in result["risk"].values()):
        raise InvalidValue("BAD_SYNTHETIC_RISK_FACT")
    return result


def _check_fact_heights(batch, anchor):
    # The accounting model checks complete fact schemas and synthetic flags.
    # This hook additionally prevents importing facts beyond the bound anchor.
    for field in ("deposits", "settlements"):
        for fact in batch[field]:
            height = fact.get("height")
            if type(height) is not int or not 0 <= height <= anchor["height"]:
                raise InvalidValue("FACT_BEYOND_ANCHOR")


def _check_descendant(anchor, parent_anchor, known_anchors):
    _check_anchor(anchor)
    _check_anchor(parent_anchor)
    if type(known_anchors) is not dict:
        raise InvalidValue("BAD_ANCHOR_CACHE")
    if anchor["height"] < parent_anchor["height"]:
        raise InvalidValue("ANCHOR_REGRESSION")
    current = anchor
    while current["height"] > parent_anchor["height"]:
        identity = current["hash"]
        if identity not in known_anchors:
            raise NeedData(identity)
        evidence = known_anchors[identity]
        _check_anchor(evidence)
        if canonical(evidence) != canonical(current):
            raise InvalidValue("ANCHOR_EVIDENCE_MISMATCH")
        predecessor = current["parent"]
        if current["height"] == parent_anchor["height"] + 1:
            if predecessor != parent_anchor["hash"]:
                raise InvalidValue("ANCHOR_NOT_DESCENDANT")
            current = parent_anchor
        else:
            if predecessor not in known_anchors:
                raise NeedData(predecessor)
            earlier = known_anchors[predecessor]
            _check_anchor(earlier)
            if earlier["hash"] != predecessor or earlier["height"] != current["height"] - 1:
                raise InvalidValue("BROKEN_ANCHOR_CHAIN")
            current = earlier
    if canonical(current) != canonical(parent_anchor):
        raise InvalidValue("ANCHOR_NOT_DESCENDANT")


class Application:
    """One replica's immutable committed snapshot and nonmutating previews."""

    def __init__(self, snapshot: bytes):
        self.apply_snapshot(snapshot)

    @property
    def snapshot(self) -> bytes:
        return self._snapshot

    @property
    def root(self) -> str:
        return self._root

    def _draft(self, batch):
        normalized = normalize_batch(batch)
        try:
            draft = Model.restore(self._snapshot)
            draft.apply_batch(**normalized)
            return draft
        except (ModelError, TypeError, KeyError, AttributeError) as exc:
            raise InvalidValue("ACCOUNTING_REJECTED:" + str(exc)) from exc

    def preview(self, batch: dict) -> bytes:
        return self._draft(batch).snapshot()

    def apply_snapshot(self, snapshot: bytes) -> None:
        try:
            restored = Model.restore(snapshot)
            encoded, root = restored.snapshot(), restored.digest()
        except (ModelError, TypeError, KeyError, AttributeError) as exc:
            raise InvalidValue("INVALID_ACCOUNTING_SNAPSHOT:" + str(exc)) from exc
        self._snapshot, self._root = encoded, root

    def build(self, instance: dict, anchor: dict, batch: dict) -> dict:
        if type(instance) is not dict:
            raise InvalidValue("BAD_INSTANCE")
        canonical(instance)
        _check_anchor(anchor)
        normalized = normalize_batch(batch)
        _check_fact_heights(normalized, anchor)
        draft = self._draft(normalized)
        return {"instance": deepcopy(instance), "anchor": deepcopy(anchor),
                "batch": normalized, "result": draft.digest()}

    def validate(self, body: dict, instance: dict, parent_anchor: dict,
                 known_anchors: dict) -> bytes:
        if type(body) is not dict or set(body) != {"instance", "anchor", "batch", "result"}:
            raise InvalidValue("BAD_VALUE_SHAPE")
        if (type(instance) is not dict or type(body["instance"]) is not dict
                or canonical(body["instance"]) != canonical(instance)):
            raise InvalidValue("WRONG_INSTANCE")
        _exact_hash(body["result"], "BAD_RESULT_ROOT")
        normalized = normalize_batch(body["batch"])
        if canonical(normalized) != canonical(body["batch"]):
            raise InvalidValue("NON_CANONICAL_BATCH")
        _check_descendant(body["anchor"], parent_anchor, known_anchors)
        _check_fact_heights(normalized, body["anchor"])
        draft = self._draft(normalized)
        if draft.digest() != body["result"]:
            raise InvalidValue("RESULT_ROOT_MISMATCH")
        return draft.snapshot()
