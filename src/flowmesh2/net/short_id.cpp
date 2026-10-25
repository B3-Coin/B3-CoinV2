// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/short_id.h>

#include <crypto/common.h>
#include <crypto/siphash.h>
#include <flowmesh2/core/hashes.h>

#include <algorithm>
#include <array>
#include <map>
#include <utility>

namespace flowmesh2::net {

ShortIdCodec::ShortIdCodec(const uint256& header_hash, uint64_t nonce) : m_header_hash{header_hash}, m_nonce{nonce}
{
    std::array<unsigned char, 40> message{};
    std::copy(header_hash.begin(), header_hash.end(), message.begin());
    WriteLE64(message.data() + 32, nonce);
    const uint256 keys{Tagged(HashTag::SHORT_ID_KEY, message)};
    m_k0 = ReadLE64(keys.data());
    m_k1 = ReadLE64(keys.data() + 8);
}

agreement::ShortId ShortIdCodec::ShortOf(const ActionId& id) const
{
    const uint64_t hash{CSipHasher{m_k0, m_k1}.Write(std::span<const unsigned char>{id.id.data(), id.id.size()}).Finalize()};
    agreement::ShortId out;
    for (size_t i{0}; i < out.size(); ++i) out[i] = static_cast<unsigned char>(hash >> (8 * i));
    return out;
}

Result<agreement::ShortId> ShortIdCodec::Encode(const ActionId& id) const { return Ok(ShortOf(id)); }

Result<std::vector<ActionId>> ShortIdCodec::Decode(const agreement::ShortId& short_id, std::span<const ActionId> known) const
{
    std::vector<ActionId> out;
    for (const ActionId& id : known) {
        if (ShortOf(id) == short_id) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return Ok(std::move(out));
}

ShortIdResolution ShortIdCodec::Resolve(std::span<const agreement::ShortId> shorts, std::span<const ActionId> held) const
{
    // Short ID -> the distinct held actions with it (at most two kept: two means AMBIGUOUS).
    std::map<agreement::ShortId, std::vector<ActionId>> candidates;
    for (const agreement::ShortId& s : shorts) candidates.try_emplace(s);
    for (const ActionId& id : held) {
        const auto it{candidates.find(ShortOf(id))};
        if (it == candidates.end()) continue;
        std::vector<ActionId>& ids{it->second};
        if (std::find(ids.begin(), ids.end(), id) == ids.end() && ids.size() < 2) ids.push_back(id);
    }
    std::map<agreement::ShortId, uint32_t> uses;
    for (const agreement::ShortId& s : shorts) ++uses[s];

    ShortIdResolution out;
    out.shorts.assign(shorts.begin(), shorts.end());
    out.ids.resize(shorts.size());
    out.open.resize(shorts.size());
    for (size_t i{0}; i < shorts.size(); ++i) {
        const std::vector<ActionId>& ids{candidates.at(shorts[i])};
        if (ids.size() == 1 && uses.at(shorts[i]) == 1) {
            out.ids[i] = ids.front();
        } else {
            out.open[i] = ids.empty() && uses.at(shorts[i]) == 1 ? ShortIdOpen::MISSING : ShortIdOpen::AMBIGUOUS;
        }
    }
    return out;
}

Result<void> ShortIdCodec::Fill(ShortIdResolution& resolution, uint32_t index, const ActionId& fetched) const
{
    if (index >= resolution.shorts.size() || resolution.ids.size() != resolution.shorts.size() ||
        resolution.open.size() != resolution.shorts.size()) {
        return Error{ErrorCode::INVALID_INPUT, SHORT_ID_FILL_INDEX};
    }
    if (!resolution.open[index].has_value()) return Error{ErrorCode::INVALID_INPUT, SHORT_ID_FILL_RESOLVED};
    if (ShortOf(fetched) != resolution.shorts[index]) return Error{ErrorCode::INVALID_INPUT, SHORT_ID_FILL_MISMATCH};
    resolution.ids[index] = fetched;
    resolution.open[index].reset();
    return Ok();
}

std::vector<uint32_t> ShortIdResolution::Open() const
{
    std::vector<uint32_t> out;
    for (uint32_t i{0}; i < open.size(); ++i) {
        if (open[i].has_value()) out.push_back(i);
    }
    return out;
}

bool ShortIdResolution::Complete() const
{
    return ids.size() == shorts.size() && std::all_of(ids.begin(), ids.end(), [](const auto& id) { return id.has_value(); });
}

std::optional<std::vector<ActionId>> ShortIdResolution::Ids() const
{
    if (!Complete()) return std::nullopt;
    std::vector<ActionId> out;
    out.reserve(ids.size());
    for (const auto& id : ids) out.push_back(*id);
    return out;
}

} // namespace flowmesh2::net
