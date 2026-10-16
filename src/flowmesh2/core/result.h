// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_RESULT_H
#define B3_FLOWMESH2_CORE_RESULT_H

#include <util/expected.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

/** Error model of the FlowMesh V2 clean core (rebirth outline, slice R1).
 *
 * Every fallible fm2 operation returns Result<T>. A Result is either a value
 * (made only by Ok() or Ok(v)) or an Error. It has no default constructor, so a
 * bare `return {};` cannot produce success, and it is [[nodiscard]], so an
 * ignored Result is a build error under the fm2 flag -Werror=unused-result.
 *
 * A stub is written exactly as
 *     return NotImplemented("M2-LED-05"); // TODO(rebirth:M2-LED-05)
 * or with Unsupported(...). TaskId only accepts a compile-time string in the
 * TODO.md ID format, so the ID in the error is the literal in the source.
 *
 * Everything here is deterministic: no allocation, I/O, clock or locale.
 */
namespace flowmesh2 {

/** Registry of fm2 error codes. APPEND-ONLY and owner-protected: never
 * renumber, reuse or remove a value; add new codes at the end with the next
 * integer. Zero is not a code. The integers are pinned by result_tests.
 */
enum class ErrorCode : uint16_t {
    NOT_IMPLEMENTED = 1,       //!< outline stub; detail is the TODO.md task id
    UNSUPPORTED = 2,           //!< deliberately refused feature; detail is the task id
    UNSUPPORTED_NETWORK = 3,   //!< chain type outside the profile (only REGTEST in the outline)
    DECODE = 4,                //!< malformed, non-canonical or trailing bytes
    OVERSIZE = 5,              //!< a cap was exceeded (checked before allocation)
    //! Checked arithmetic refused a result. Not named OVERFLOW: the macOS
    //! <math.h> defines OVERFLOW (and DOMAIN) as macros (ARCHITECTURE.md O-21).
    ARITHMETIC_OVERFLOW = 6,
    INVARIANT = 7,             //!< internal invariant broken (e.g. conservation); no candidate
    ORDER_VIOLATION = 8,       //!< call order or record role violated
    EQUIVOCATION_REFUSED = 9,  //!< a second digest for one VoteKey
    STORAGE_FATAL = 10,        //!< sticky durable-storage failure
    FENCED = 11,               //!< refused because the node is fenced after a fatal error
    AUTHORITY_CHANGED = 12,    //!< authority epoch moved underneath the operation
    VERIFY_FAILED = 13,        //!< signature, proof, root or token mismatch
    STALE_PARENT = 14,         //!< not built on the current head
    BUSY = 15,                 //!< try later; nothing was queued or written
    NEED_DATA = 16,            //!< the caller must supply more data first
    INVALID_INPUT = 17,        //!< inputs fail the validity predicate
};

struct ErrorCodeInfo {
    ErrorCode code;
    std::string_view name;
};

//! The registry in ascending integer order (one entry per code above).
std::span<const ErrorCodeInfo> ErrorCodeRegistry();
//! True only for a value listed in ErrorCodeRegistry().
bool IsKnown(ErrorCode code);
//! Total: the registry name, or "UNKNOWN" for any unregistered value.
std::string_view ToString(ErrorCode code);

/** True iff `id` has the TODO.md task ID format:
 * `R<n>` (outline slice), or `<Ms>-<AREA>-<nn>` with Ms in {M<n>, MB1, B0},
 * AREA an upper-case letter followed by upper-case letters or digits, and nn
 * exactly two digits. n has no leading zero (M0 is allowed).
 */
constexpr bool IsWellFormedTaskId(std::string_view id)
{
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto is_upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    // Parses a decimal number without a leading zero (a lone "0" is fine).
    const auto number_end = [&](size_t pos) -> size_t {
        if (pos >= id.size() || !is_digit(id[pos])) return 0;
        if (id[pos] == '0') return pos + 1;
        while (pos < id.size() && is_digit(id[pos])) ++pos;
        return pos;
    };
    if (id.size() >= 2 && id[0] == 'R') {
        if (id[1] == '0') return false;
        return number_end(1) == id.size();
    }
    size_t pos{0};
    if (id.starts_with("MB1-")) {
        pos = 3;
    } else if (id.starts_with("B0-")) {
        pos = 2;
    } else if (id.size() >= 2 && id[0] == 'M') {
        pos = number_end(1);
        if (pos == 0) return false;
    } else {
        return false;
    }
    if (pos >= id.size() || id[pos] != '-') return false;
    ++pos;
    if (pos >= id.size() || !is_upper(id[pos])) return false;
    while (pos < id.size() && (is_upper(id[pos]) || is_digit(id[pos]))) ++pos;
    if (pos >= id.size() || id[pos] != '-') return false;
    ++pos;
    return id.size() == pos + 2 && is_digit(id[pos]) && is_digit(id[pos + 1]);
}

namespace detail {
//! Deliberately not constexpr: reaching it during constant evaluation makes a
//! malformed TaskId a compile error. It is never defined or called at run time.
void TaskIdMustMatchTheTodoLedgerFormat();
} // namespace detail

/** A TODO.md task ID that is a compile-time constant. The constructor is
 * consteval, so `NotImplemented(runtime_string)` does not compile, and a
 * malformed literal is a compile error. The text has static storage.
 */
class TaskId
{
public:
    consteval TaskId(const char* id) : m_id{id}
    {
        if (!IsWellFormedTaskId(m_id)) detail::TaskIdMustMatchTheTodoLedgerFormat();
    }
    constexpr std::string_view View() const { return m_id; }

private:
    std::string_view m_id;
};

/** One fm2 error. `detail` must refer to static storage (a string literal or
 * a TaskId); an Error never owns memory, so it is cheap to copy and cannot
 * dangle when built that way. For NOT_IMPLEMENTED and UNSUPPORTED the detail
 * is exactly the task ID. There is no default constructor: an Error always
 * names its code.
 */
struct [[nodiscard]] Error {
    ErrorCode code;
    std::string_view detail;

    constexpr Error(ErrorCode code_in, std::string_view detail_in = {}) : code{code_in}, detail{detail_in} {}
    friend constexpr bool operator==(const Error&, const Error&) = default;
};

//! The only stub return path for unfinished work. See TODO.md for the ID.
[[nodiscard]] constexpr Error NotImplemented(TaskId task) { return Error{ErrorCode::NOT_IMPLEMENTED, task.View()}; }
//! The stub return path for a feature that is refused on purpose for now.
[[nodiscard]] constexpr Error Unsupported(TaskId task) { return Error{ErrorCode::UNSUPPORTED, task.View()}; }

//! Success tag carrying a value; converts into Result<T>. Made only by Ok(v).
template <class T>
struct [[nodiscard]] OkValue {
    T value;
};
//! Success tag without a value; converts into Result<void>. Made only by Ok().
struct [[nodiscard]] OkVoid {
};

template <class T>
[[nodiscard]] constexpr OkValue<std::decay_t<T>> Ok(T&& value)
{
    return OkValue<std::decay_t<T>>{std::forward<T>(value)};
}
[[nodiscard]] constexpr OkVoid Ok() { return OkVoid{}; }

/** Value-or-Error over util::Expected<T, Error>.
 *
 * - No default constructor and no implicit conversion from T: success is
 *   spelled Ok(v), failure is an Error (implicit, so `return Error{...};` and
 *   `return NotImplemented("...");` work).
 * - value() on an error throws util::BadExpectedAccess; error() on a value
 *   fails an Assert. Callers test has_value() first.
 */
template <class T>
class [[nodiscard]] Result
{
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> is ambiguous");

public:
    using value_type = T;

    Result() = delete;
    Result(Error error) : m_inner{util::Unexpected<Error>{error}} {}
    template <class U>
        requires std::is_constructible_v<T, U&&>
    Result(OkValue<U>&& ok) : m_inner{T(std::move(ok.value))}
    {
    }

    bool has_value() const noexcept { return m_inner.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    const T& value() const& { return m_inner.value(); }
    T& value() & { return m_inner.value(); }
    T&& value() && { return std::move(m_inner).value(); }

    const T& operator*() const& { return value(); }
    T& operator*() & { return value(); }
    const T* operator->() const { return &value(); }
    T* operator->() { return &value(); }

    const Error& error() const& { return m_inner.error(); }

private:
    util::Expected<T, Error> m_inner;
};

template <>
class [[nodiscard]] Result<void>
{
public:
    using value_type = void;

    Result() = delete;
    Result(Error error) : m_inner{util::Unexpected<Error>{error}} {}
    Result(OkVoid) : m_inner{} {}

    bool has_value() const noexcept { return m_inner.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }
    //! Throws util::BadExpectedAccess on an error.
    void value() const { m_inner.value(); }

    const Error& error() const& { return m_inner.error(); }

private:
    util::Expected<void, Error> m_inner;
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_RESULT_H
