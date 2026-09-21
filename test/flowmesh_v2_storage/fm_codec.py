"""Bounded, canonical typed JSON for the TEST disk store. Never use pickle.

Every node has an explicit tag. Dictionaries are ordered by encoded key, so
integer and tuple keys survive without stringification or order ambiguity.
Only the model's data vocabulary is accepted; this is not a network codec.
"""
import base64
import binascii
import json


CODEC_VERSION = 1
MAX_ENCODED_BYTES = 16 * 1024 * 1024
MAX_ATOM_BYTES = 8 * 1024 * 1024
MAX_DEPTH = 64
MAX_NODES = 250000
MAX_CONTAINER_ITEMS = 20000
MAX_INTEGER_BITS = 256


class CodecError(ValueError):
    def __init__(self, code="CODEC_FORMAT"):
        self.code = self.reason = code
        super().__init__(code)


def _json(value):
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"),
                      allow_nan=False).encode("ascii")


def _key(value):
    return (type(value) in (str, int) or
            type(value) is tuple and all(_key(part) for part in value))


def _walk_budget(depth, budget):
    budget[0] += 1
    if depth > MAX_DEPTH or budget[0] > MAX_NODES:
        raise CodecError("CODEC_BOUND")


def _pack(value, depth, budget):
    _walk_budget(depth, budget)
    kind = type(value)
    if value is None:
        return ["n"]
    if kind is bool:
        return ["z", value]
    if kind is int:
        if value.bit_length() > MAX_INTEGER_BITS:
            raise CodecError("CODEC_BOUND")
        return ["i", str(value)]
    if kind is str:
        if len(value.encode("utf-8")) > MAX_ATOM_BYTES:
            raise CodecError("CODEC_BOUND")
        return ["s", value]
    if kind is bytes:
        if len(value) > MAX_ATOM_BYTES:
            raise CodecError("CODEC_BOUND")
        return ["b", base64.b64encode(value).decode("ascii")]
    if kind in (tuple, list, dict):
        if len(value) > MAX_CONTAINER_ITEMS:
            raise CodecError("CODEC_BOUND")
        if kind is dict:
            pairs = []
            for key, item in value.items():
                if not _key(key):
                    raise CodecError()
                pairs.append([_pack(key, depth + 1, budget),
                              _pack(item, depth + 1, budget)])
            pairs.sort(key=lambda pair: _json(pair[0]))
            return ["d", pairs]
        return ["t" if kind is tuple else "l",
                [_pack(item, depth + 1, budget) for item in value]]
    raise CodecError()


def encode(value):
    """Return canonical bytes, rejecting unsupported types and explicit caps."""
    try:
        result = _json(["flowmesh-test-json", CODEC_VERSION, _pack(value, 0, [0])])
        if len(result) > MAX_ENCODED_BYTES:
            raise CodecError("CODEC_BOUND")
        return result
    except (RecursionError, UnicodeError, OverflowError) as exc:
        raise CodecError("CODEC_BOUND") from exc


def _unpack(node, depth, budget):
    _walk_budget(depth, budget)
    if type(node) is not list or not node or type(node[0]) is not str:
        raise CodecError()
    tag = node[0]
    if tag == "n" and len(node) == 1:
        return None
    if len(node) != 2:
        raise CodecError()
    value = node[1]
    if tag == "z" and type(value) is bool:
        return value
    if tag == "i" and type(value) is str:
        if len(value) > 80:
            raise CodecError("CODEC_BOUND")
        try:
            result = int(value)
        except ValueError as exc:
            raise CodecError() from exc
        if str(result) != value or result.bit_length() > MAX_INTEGER_BITS:
            raise CodecError()
        return result
    if tag == "s" and type(value) is str:
        if len(value.encode("utf-8")) > MAX_ATOM_BYTES:
            raise CodecError("CODEC_BOUND")
        return value
    if tag == "b" and type(value) is str:
        if len(value) > 4 * ((MAX_ATOM_BYTES + 2) // 3):
            raise CodecError("CODEC_BOUND")
        try:
            result = base64.b64decode(value, validate=True)
        except (ValueError, binascii.Error) as exc:
            raise CodecError() from exc
        if len(result) > MAX_ATOM_BYTES:
            raise CodecError("CODEC_BOUND")
        if base64.b64encode(result).decode("ascii") != value:
            raise CodecError()
        return result
    if tag in ("t", "l", "d") and type(value) is list:
        if len(value) > MAX_CONTAINER_ITEMS:
            raise CodecError("CODEC_BOUND")
        if tag != "d":
            items = [_unpack(item, depth + 1, budget) for item in value]
            return tuple(items) if tag == "t" else items
        result, previous = {}, None
        for pair in value:
            if type(pair) is not list or len(pair) != 2:
                raise CodecError()
            key = _unpack(pair[0], depth + 1, budget)
            order = _json(pair[0])
            if (not _key(key) or key in result or
                    previous is not None and order <= previous):
                raise CodecError()
            result[key] = _unpack(pair[1], depth + 1, budget)
            previous = order
        return result
    raise CodecError()


def decode(raw):
    """Decode canonical typed JSON with size, nesting and allocation bounds."""
    if type(raw) is not bytes:
        raise CodecError()
    if len(raw) > MAX_ENCODED_BYTES:
        raise CodecError("CODEC_BOUND")
    try:
        packed = json.loads(raw)
        if (type(packed) is not list or len(packed) != 3 or
                packed[0] != "flowmesh-test-json" or
                type(packed[1]) is not int or packed[1] != CODEC_VERSION):
            raise CodecError()
        value = _unpack(packed[2], 0, [0])
        if encode(value) != raw:
            raise CodecError()
        return value
    except (ValueError, TypeError, RecursionError, UnicodeError, OverflowError) as exc:
        if isinstance(exc, CodecError):
            raise
        raise CodecError() from exc
