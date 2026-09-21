"""Model-only atomic stable-memory updates without copying historical records.

The mutation callback runs against private copies of the records it touches.
Only after it succeeds are those entries installed into the ordinary journal
maps, in one synchronous simulator step. Old aliases to those maps observe a
successful commit; callbacks must not expose their temporary overlay. This is
not a storage format, concurrency primitive, or real signing-journal migration.
"""
from collections.abc import MutableMapping
from copy import deepcopy


class _Changes(MutableMapping):
    def __init__(self, original):
        self.original = original
        self.changed = {}
        self.deleted = set()
        self.copies = 0

    def __getitem__(self, key):
        if key in self.deleted:
            raise KeyError(key)
        if key not in self.changed:
            self.changed[key] = deepcopy(self.original[key])
            self.copies += 1
        return self.changed[key]

    def __setitem__(self, key, value):
        self.deleted.discard(key)
        self.changed[key] = value

    def __delitem__(self, key):
        if key in self.deleted or key not in self.changed and key not in self.original:
            raise KeyError(key)
        self.changed.pop(key, None)
        self.deleted.add(key)

    def __contains__(self, key):
        return key not in self.deleted and (key in self.changed or key in self.original)

    def __iter__(self):
        for key in self.original:
            if key not in self.deleted:
                yield key
        for key in self.changed:
            if key not in self.original:
                yield key

    def __len__(self):
        return (len(self.original) - sum(key in self.original for key in self.deleted)
                + sum(key not in self.original for key in self.changed))

    def commit(self):
        for key in self.deleted:
            self.original.pop(key, None)
        self.original.update(self.changed)
        return self.original


def update_durable(old, mutate):
    """Return ordinary durable dictionaries and counts; failure publishes none.

    Historical decisions and issued signatures are never pruned. Current-record
    copies remain bounded by the profile's views/proof limits, independent of
    the number of already applied records. Snapshot bytes are immutable and
    anchor metadata is a single bounded object.
    """
    candidate = dict(old)
    candidate["anchor"] = deepcopy(old["anchor"])
    records = _Changes(old["records"])
    bodies = _Changes(old["retained_bodies"])
    candidate["records"], candidate["retained_bodies"] = records, bodies
    mutate(candidate)
    stats = {"records_copied": records.copies,
             "records_written": len(records.changed) + len(records.deleted),
             "bodies_copied": bodies.copies,
             "bodies_written": len(bodies.changed) + len(bodies.deleted)}
    candidate["records"], candidate["retained_bodies"] = records.commit(), bodies.commit()
    return candidate, stats
