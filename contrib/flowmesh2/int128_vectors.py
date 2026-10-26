#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Shared 128-bit arithmetic vectors for FlowMesh V2 (task M2-LED-09).

Every fm2 function with a 128-bit intermediate is computed here with Python
integers, which have no width, over a seeded input stream biased to the
edges (powers of two, 2^62, 2^64 - 1, small values). The results are
digested and written to src/flowmesh2/test/util/int128_vectors.h. The test
int128_vector_tests.cpp draws the same stream, calls the C++ functions and
compares. A native __int128 build and a Boost fallback build (MSVC, or
-U__SIZEOF_INT128__) must both match, so the two never diverge silently.

Record of one case: u16 LE ErrorCode (0 = success), then each result value
as u64 LE (0 on error). A family's digest is SHA-256 over its records.

Usage:
  int128_vectors.py --write HEADER   regenerate the header
  int128_vectors.py --check HEADER   fail if HEADER differs from the output
"""

import argparse
import hashlib
import struct
import sys

U64 = (1 << 64) - 1
U64_MAX = U64
MAX_ATOMS = 1 << 62
OK, INVARIANT, INVALID_INPUT, ARITHMETIC_OVERFLOW = 0, 7, 17, 6

CASES = 100_000  # per family
SHOWN = 12       # explicit expected results per family, for diagnosis
MAX_VALUES = 6   # most result values in one record (AllocateProRata, six takers)


class Stream:
    """splitmix64 and the edge-biased picker; int128_vector_tests.cpp mirrors it."""

    def __init__(self, seed):
        self.state = seed & U64

    def next(self):
        self.state = (self.state + 0x9E3779B97F4A7C15) & U64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & U64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & U64
        return z ^ (z >> 31)

    def pick(self):
        kind = self.next() % 8
        if kind == 0:
            return self.next()
        if kind == 1:
            return self.next() >> (self.next() % 64)
        if kind == 2:
            k = self.next() % 64
            return ((1 << k) + self.next() % 5 - 2) & U64
        if kind == 3:
            return U64_MAX - self.next() % 4
        if kind == 4:
            return self.next() % 17
        if kind == 5:
            return (MAX_ATOMS + self.next() % 5 - 2) & U64
        if kind == 6:
            k = 1 + self.next() % 64
            return U64_MAX >> (64 - k)
        k = 29 + self.next() % 6  # around 2^31..2^34, where products cross 2^62 and 2^64
        return ((1 << k) + self.next() % 5 - 2) & U64


def record(code, values=()):
    return struct.pack('<H', code) + b''.join(struct.pack('<Q', v if code == OK else 0) for v in values)


def mul_checked(s):
    a, b = s.pick(), s.pick()
    p = a * b
    return (OK, (p,)) if p <= U64_MAX else (ARITHMETIC_OVERFLOW, (0,))


def mul_div(s, ceil):
    a, b, d = s.pick(), s.pick(), s.pick()
    if d == 0:
        return INVALID_INPUT, (0,)
    q = -(-(a * b) // d) if ceil else a * b // d
    return (OK, (q,)) if q <= U64_MAX else (ARITHMETIC_OVERFLOW, (0,))


def market(s):
    """(base_atoms_per_lot, quote_atoms_per_tick, valid)"""
    lot, tick = s.pick(), s.pick()
    return lot, tick, 1 <= lot <= MAX_ATOMS and 1 <= tick <= MAX_ATOMS


def base_atoms(s):
    lot, _tick, valid = market(s)
    lots = s.pick()
    if not valid:
        return INVALID_INPUT, (0,)
    atoms = lots * lot
    return (OK, (atoms,)) if atoms <= MAX_ATOMS else (ARITHMETIC_OVERFLOW, (0,))


def quote_atoms(s):
    _lot, tick, valid = market(s)
    lots, price = s.pick(), s.pick()
    if not valid:
        return INVALID_INPUT, (0,)
    atoms = lots * price * tick
    return (OK, (atoms,)) if atoms <= MAX_ATOMS else (ARITHMETIC_OVERFLOW, (0,))


def pro_rata(s):
    supply = s.pick()
    m = 1 + s.next() % 6
    demands = [s.pick() for _ in range(m)]
    ranks = [s.next() % 8 for _ in range(m)]  # small, so repeats occur
    zeros = [0] * m
    total, seen = 0, set()
    for d, r in zip(demands, ranks):
        if d == 0 or r in seen:
            return INVALID_INPUT, zeros
        seen.add(r)
        if d > U64_MAX - total:
            return ARITHMETIC_OVERFLOW, zeros
        total += d
    if total <= supply:
        return OK, demands
    alloc = [supply * d // total for d in demands]
    rem = [supply * d % total for d in demands]
    leftover = supply - sum(alloc)
    if leftover > m:
        return INVARIANT, zeros
    order = sorted(range(m), key=lambda i: (-rem[i], ranks[i]))
    for i in order[:leftover]:
        alloc[i] += 1
    return OK, alloc


# (name, seed, function); the seed is the family's index, fixed forever.
FAMILIES = [
    ('MulChecked', 1, mul_checked),
    ('MulDivFloor', 2, lambda s: mul_div(s, False)),
    ('MulDivCeil', 3, lambda s: mul_div(s, True)),
    ('BaseAtoms', 4, base_atoms),
    ('QuoteAtoms', 5, quote_atoms),
    ('AllocateProRata', 6, pro_rata),
]


def generate():
    out = []
    for name, seed, fn in FAMILIES:
        s = Stream(seed)
        h = hashlib.sha256()
        shown, codes = [], {}
        for i in range(CASES):
            code, values = fn(s)
            h.update(record(code, values))
            codes[code] = codes.get(code, 0) + 1
            if i < SHOWN:
                shown.append((code, list(values) if code == OK else [0] * len(values)))
        out.append((name, seed, h.hexdigest(), shown, codes))
    return out


def render(families):
    lines = [
        '// Generated by contrib/flowmesh2/int128_vectors.py; do not edit.',
        '// CTest flowmesh2_int128_vectors fails if this file and the generator differ.',
        '',
        '#ifndef B3_FLOWMESH2_TEST_UTIL_INT128_VECTORS_H',
        '#define B3_FLOWMESH2_TEST_UTIL_INT128_VECTORS_H',
        '',
        '#include <array>',
        '#include <cstddef>',
        '#include <cstdint>',
        '#include <string_view>',
        '',
        'namespace flowmesh2::int128_vectors {',
        '',
        f'inline constexpr size_t CASES{{{CASES}}};',
        f'inline constexpr size_t SHOWN{{{SHOWN}}};',
        f'inline constexpr size_t MAX_VALUES{{{MAX_VALUES}}};',
        '',
        'struct Shown {',
        '    uint16_t code;     //!< ErrorCode value, 0 = success',
        '    size_t count;      //!< result values in the record',
        '    std::array<uint64_t, MAX_VALUES> values;',
        '};',
        '',
        'struct Family {',
        '    std::string_view name;',
        '    uint64_t seed;',
        '    std::string_view digest; //!< SHA-256 hex over CASES records',
        '    std::array<Shown, SHOWN> shown;',
        '};',
        '',
    ]
    for name, seed, digest, shown, codes in families:
        tally = ', '.join(f'{c}: {n}' for c, n in sorted(codes.items()))
        lines.append(f'// {name}: result codes over {CASES} cases {{{tally}}}')
        lines.append(f'inline constexpr Family {name.upper()}{{"{name}", {seed}, "{digest}", {{{{')
        for code, values in shown:
            vals = ', '.join(f'{v}ULL' for v in values)
            lines.append(f'    {{{code}, {len(values)}, {{{vals}}}}},')
        lines.append('}}};')
        lines.append('')
    lines += ['} // namespace flowmesh2::int128_vectors', '', '#endif // B3_FLOWMESH2_TEST_UTIL_INT128_VECTORS_H', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--write', metavar='HEADER')
    mode.add_argument('--check', metavar='HEADER')
    args = parser.parse_args()
    text = render(generate())
    if args.write:
        with open(args.write, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        return 0
    with open(args.check, encoding='utf-8', newline='') as f:
        current = f.read().replace('\r\n', '\n')
    if current != text:
        print(f'{args.check} differs from the generator; rerun with --write', file=sys.stderr)
        return 1
    print(f'{args.check} matches the generator')
    return 0


if __name__ == '__main__':
    sys.exit(main())
