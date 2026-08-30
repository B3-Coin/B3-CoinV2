// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_UTIL_INT128_H
#define BITCOIN_UTIL_INT128_H

// GCC and Clang expose native 128-bit integers on the 64-bit release
// targets, but 32-bit MinGW does not. Keep the native implementation where
// it exists and use Boost's fixed-width, header-only integer on that target.
#ifdef __SIZEOF_INT128__
namespace util {
using Signed128 = __int128;
using Unsigned128 = unsigned __int128;
} // namespace util
#else
#include <boost/multiprecision/cpp_int.hpp>

namespace util {
using Signed128 = boost::multiprecision::int128_t;
using Unsigned128 = boost::multiprecision::uint128_t;
} // namespace util
#endif

#endif // BITCOIN_UTIL_INT128_H
