// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef B3_FLOWMESH2_STORAGE_CONTROL_TYPES_H
#define B3_FLOWMESH2_STORAGE_CONTROL_TYPES_H

#include <uint256.h>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <tuple>

namespace flowmesh2::storage {

//! Control signatures have their own typed keys, never a PREPARE/COMMIT key.
enum class ControlPhase : uint8_t { VIEW_CHANGE = 1, NEW_VIEW = 2 };
constexpr bool IsKnown(ControlPhase phase) { return phase == ControlPhase::VIEW_CHANGE || phase == ControlPhase::NEW_VIEW; }

//! Complete fixed-committee decision instance and one immutable control tuple.
//! Fixed encoding: 157 bytes, all integers little endian.
struct ControlKey {
    uint256 domain;
    uint256 profile_hash;
    uint256 seat_set_root;
    uint64_t epoch{0};
    uint64_t slot{0};
    uint256 parent_hash;
    uint32_t seat{0};
    uint64_t view{0};
    ControlPhase phase{ControlPhase::VIEW_CHANGE};

    friend bool operator==(const ControlKey&, const ControlKey&) = default;
    friend bool operator<(const ControlKey& a, const ControlKey& b)
    {
        return std::tie(a.domain, a.profile_hash, a.seat_set_root, a.epoch, a.slot, a.parent_hash, a.seat, a.view, a.phase) <
               std::tie(b.domain, b.profile_hash, b.seat_set_root, b.epoch, b.slot, b.parent_hash, b.seat, b.view, b.phase);
    }
    template <class Stream> void Serialize(Stream& s) const
    {
        s << domain << profile_hash << seat_set_root << epoch << slot << parent_hash << seat << view << static_cast<uint8_t>(phase);
    }
    template <class Stream> void Unserialize(Stream& s)
    {
        uint8_t phase_byte{0};
        s >> domain >> profile_hash >> seat_set_root >> epoch >> slot >> parent_hash >> seat >> view >> phase_byte;
        phase = static_cast<ControlPhase>(phase_byte);
        if (!IsKnown(phase)) throw std::ios_base::failure("unknown control phase");
    }
};
inline constexpr size_t CONTROL_KEY_BYTES{157};

} // namespace flowmesh2::storage
#endif
