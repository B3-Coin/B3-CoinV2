// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_AMOUNT_H
#define B3_FLOWMESH2_CORE_AMOUNT_H

#include <consensus/amount.h>
#include <flowmesh2/core/result.h>

#include <cstdint>
#include <type_traits>

/** Checked money arithmetic of the FlowMesh V2 clean core (slice R3).
 *
 * An amount of any asset is a count of atoms, an int64 (Bitcoin's CAmount) in
 * the closed range [0, MAX_ATOMS] with MAX_ATOMS = 2^62. The headroom below
 * INT64_MAX lets a sum of two valid amounts never wrap, and lets a Merkle-sum
 * tree (M2-LED-03) check that every partial sum stays in [0, 2^62].
 *
 * Every function here is total and deterministic. Nothing wraps, saturates or
 * rounds silently:
 * - an operand outside its domain is INVALID_INPUT;
 * - a result outside its range is ARITHMETIC_OVERFLOW (also below zero);
 * - a zero divisor is INVALID_INPUT.
 * Products use util::Unsigned128 (util/int128.h), which falls back to Boost's
 * fixed-width integer where the compiler has no native 128-bit type.
 */
namespace flowmesh2 {

using Atoms = CAmount;
static_assert(std::is_same_v<Atoms, int64_t>);

//! Largest valid amount: 2^62 atoms.
inline constexpr Atoms MAX_ATOMS{Atoms{1} << 62};

//! True iff 0 <= a <= MAX_ATOMS.
constexpr bool InAtomRange(Atoms a) noexcept { return a >= 0 && a <= MAX_ATOMS; }

//! a itself if it is a valid amount, else INVALID_INPUT.
Result<Atoms> CheckAtoms(Atoms a);
//! a + b through CheckedAdd; operands must be valid; the sum must be <= MAX_ATOMS.
Result<Atoms> AddAtoms(Atoms a, Atoms b);
//! a - b through CheckedAdd; operands must be valid; the difference must be >= 0.
Result<Atoms> SubAtoms(Atoms a, Atoms b);

//! a * b, refused if the exact product exceeds UINT64_MAX.
Result<uint64_t> MulChecked(uint64_t a, uint64_t b);
//! floor(a * b / d) with an exact 128-bit intermediate. d == 0 is INVALID_INPUT;
//! a quotient above UINT64_MAX is ARITHMETIC_OVERFLOW.
Result<uint64_t> MulDivFloor(uint64_t a, uint64_t b, uint64_t d);
//! ceil(a * b / d) with an exact 128-bit intermediate; same errors as MulDivFloor.
Result<uint64_t> MulDivCeil(uint64_t a, uint64_t b, uint64_t d);

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_AMOUNT_H
