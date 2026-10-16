// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/amount.h>

#include <util/int128.h>
#include <util/overflow.h>

#include <limits>
#include <optional>

namespace flowmesh2 {
namespace {

constexpr uint64_t U64_MAX{std::numeric_limits<uint64_t>::max()};

Error OutOfRangeOperand() { return Error{ErrorCode::INVALID_INPUT, "amount outside [0, 2^62]"}; }

} // namespace

Result<Atoms> CheckAtoms(Atoms a)
{
    if (!InAtomRange(a)) return OutOfRangeOperand();
    return Ok(a);
}

Result<Atoms> AddAtoms(Atoms a, Atoms b)
{
    if (!InAtomRange(a) || !InAtomRange(b)) return OutOfRangeOperand();
    const std::optional<Atoms> sum{CheckedAdd(a, b)};
    if (!sum || *sum > MAX_ATOMS) return Error{ErrorCode::ARITHMETIC_OVERFLOW, "amount sum above 2^62"};
    return Ok(*sum);
}

Result<Atoms> SubAtoms(Atoms a, Atoms b)
{
    if (!InAtomRange(a) || !InAtomRange(b)) return OutOfRangeOperand();
    // -b cannot overflow: b is in [0, 2^62].
    const std::optional<Atoms> difference{CheckedAdd(a, Atoms{-b})};
    if (!difference || *difference < 0) return Error{ErrorCode::ARITHMETIC_OVERFLOW, "amount difference below zero"};
    return Ok(*difference);
}

Result<uint64_t> MulChecked(uint64_t a, uint64_t b)
{
    const util::Unsigned128 product{util::Unsigned128{a} * util::Unsigned128{b}};
    if (product > util::Unsigned128{U64_MAX}) return Error{ErrorCode::ARITHMETIC_OVERFLOW, "product above 2^64-1"};
    return Ok(static_cast<uint64_t>(product));
}

Result<uint64_t> MulDivFloor(uint64_t a, uint64_t b, uint64_t d)
{
    if (d == 0) return Error{ErrorCode::INVALID_INPUT, "zero divisor"};
    const util::Unsigned128 quotient{(util::Unsigned128{a} * util::Unsigned128{b}) / util::Unsigned128{d}};
    if (quotient > util::Unsigned128{U64_MAX}) return Error{ErrorCode::ARITHMETIC_OVERFLOW, "quotient above 2^64-1"};
    return Ok(static_cast<uint64_t>(quotient));
}

Result<uint64_t> MulDivCeil(uint64_t a, uint64_t b, uint64_t d)
{
    if (d == 0) return Error{ErrorCode::INVALID_INPUT, "zero divisor"};
    const util::Unsigned128 product{util::Unsigned128{a} * util::Unsigned128{b}};
    const util::Unsigned128 divisor{d};
    util::Unsigned128 quotient{product / divisor};
    if (product % divisor != 0) quotient += 1;
    if (quotient > util::Unsigned128{U64_MAX}) return Error{ErrorCode::ARITHMETIC_OVERFLOW, "quotient above 2^64-1"};
    return Ok(static_cast<uint64_t>(quotient));
}

} // namespace flowmesh2
