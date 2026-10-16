// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_CODEC_H
#define B3_FLOWMESH2_CORE_CODEC_H

#include <flowmesh2/core/result.h>
#include <serialize.h>
#include <streams.h>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

/** Canonical bounded codec of the FlowMesh V2 clean core (slice R3).
 *
 * fm2 objects use Bitcoin serialization (serialize.h, streams.h) with three
 * extra rules:
 * 1. Exact decode. A decoder consumes the whole input; trailing bytes are a
 *    DECODE error. Input longer than the caller's max_bytes is OVERSIZE before
 *    any parsing.
 * 2. Capped counts. Every CompactSize count is read canonically (Bitcoin's
 *    ReadCompactSize refuses non-minimal forms) and compared with its cap
 *    BEFORE anything is allocated. A count above the cap is OVERSIZE.
 * 3. Typed errors. Inside Unserialize, a malformed field throws
 *    std::ios_base::failure (Bitcoin's convention) and a cap breach throws
 *    CapExceeded. DecodeExact turns the first into DECODE and the second into
 *    OVERSIZE; no exception leaves DecodeExact.
 * Encoders never produce bytes their decoder refuses: WriteCappedVector
 * refuses (throws CapExceeded) instead of writing an over-cap count.
 */
namespace flowmesh2 {

//! Thrown inside Unserialize/Serialize when a count exceeds its cap.
class CapExceeded : public std::ios_base::failure
{
public:
    explicit CapExceeded(const char* what) : std::ios_base::failure{what} {}
};

/** Decode exactly one object from `bytes` with `read(SpanReader&) -> T`.
 * OVERSIZE if bytes.size() > max_bytes or a cap is exceeded; DECODE if the
 * reader throws std::ios_base::failure or bytes remain afterwards.
 */
template <class Reader>
auto DecodeExactWith(std::span<const unsigned char> bytes, size_t max_bytes, Reader&& read)
    -> Result<std::remove_cvref_t<decltype(read(std::declval<SpanReader&>()))>>
{
    using T = std::remove_cvref_t<decltype(read(std::declval<SpanReader&>()))>;
    if (bytes.size() > max_bytes) return Error{ErrorCode::OVERSIZE, "input longer than its maximum encoding"};
    SpanReader reader{bytes};
    try {
        T value(read(reader));
        if (!reader.empty()) return Error{ErrorCode::DECODE, "trailing bytes"};
        return Ok(std::move(value));
    } catch (const CapExceeded&) {
        return Error{ErrorCode::OVERSIZE, "count exceeds its cap"};
    } catch (const std::ios_base::failure&) {
        return Error{ErrorCode::DECODE, "malformed or non-canonical encoding"};
    }
}

//! DecodeExactWith for a default-constructible type with Unserialize.
template <class T>
Result<T> DecodeExact(std::span<const unsigned char> bytes, size_t max_bytes)
{
    return DecodeExactWith(bytes, max_bytes, [](SpanReader& reader) {
        T value{};
        reader >> value;
        return value;
    });
}

//! The Bitcoin serialization of `obj` as bytes.
template <class T>
std::vector<unsigned char> EncodeBytes(const T& obj)
{
    std::vector<unsigned char> out;
    VectorWriter{out, 0, obj};
    return out;
}

/** Read a CompactSize count, canonically, and refuse it above `max_items`
 * before any allocation, then read that many items into `out` (replacing its
 * contents). Throws CapExceeded or std::ios_base::failure (see file comment).
 */
template <class Stream, class T>
void ReadCappedVector(Stream& s, std::vector<T>& out, size_t max_items)
{
    // range_check=false: the cap below is the only size limit, so an
    // over-cap count is always OVERSIZE, never Bitcoin's generic MAX_SIZE error.
    const uint64_t count{ReadCompactSize(s, /*range_check=*/false)};
    if (count > max_items) throw CapExceeded{"ReadCappedVector: count exceeds cap"};
    out.clear();
    if constexpr (std::is_same_v<T, unsigned char>) {
        out.resize(static_cast<size_t>(count));
        s.read(MakeWritableByteSpan(out));
    } else {
        out.reserve(static_cast<size_t>(count));
        for (uint64_t i{0}; i < count; ++i) {
            T item{};
            s >> item;
            out.push_back(std::move(item));
        }
    }
}

//! Write a CompactSize count and the items; throws CapExceeded above `max_items`.
template <class Stream, class T>
void WriteCappedVector(Stream& s, const std::vector<T>& items, size_t max_items)
{
    if (items.size() > max_items) throw CapExceeded{"WriteCappedVector: count exceeds cap"};
    WriteCompactSize(s, items.size());
    if constexpr (std::is_same_v<T, unsigned char>) {
        s.write(MakeByteSpan(items));
    } else {
        for (const auto& item : items) s << item;
    }
}

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_CODEC_H
