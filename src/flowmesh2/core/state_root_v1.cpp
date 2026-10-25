// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/state_root_v1.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/slot.h>
#include <hash.h>
#include <serialize.h>
#include <streams.h>

#include <ios>
#include <utility>

namespace flowmesh2 {

uint256 StateRootsV1::Root() const
{
    HashWriter writer{TaggedWriter(HashTag::STATE)};
    writer << COMMITMENT_VERSION_SMT << accounts << orders << markets << nullifiers << totals << withdrawals;
    return writer.GetSHA256();
}

uint256 AccountPathV1(const AccountId& account)
{
    return SmtKeyPath(SmtComponent::ACCOUNTS, std::span<const unsigned char>{account.id.data(), 32});
}

size_t AccountProofV1::MaxEncodedSize(const ProfileParams& profile)
{
    const size_t leaf{AccountLeafV0::MaxEncodedSize(profile)};
    return 32 + 1 + GetSizeOfCompactSize(leaf) + leaf + GetSizeOfCompactSize(SmtProof::MAX_ENCODED_BYTES) + SmtProof::MAX_ENCODED_BYTES;
}

std::vector<unsigned char> AccountProofV1::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << account.id << static_cast<uint8_t>(Kind());
    if (leaf.has_value()) {
        const std::vector<unsigned char> bytes{leaf->Encode()};
        WriteCompactSize(writer, bytes.size());
        writer.write(std::as_bytes(std::span{bytes}));
    }
    const std::vector<unsigned char> bytes{proof.Encode()};
    WriteCompactSize(writer, bytes.size());
    writer.write(std::as_bytes(std::span{bytes}));
    return out;
}

Result<AccountProofV1> AccountProofV1::Decode(std::span<const unsigned char> bytes, const ProfileParams& profile)
{
    // A nested decoder's error, rethrown so DecodeExactWith keeps its kind.
    const auto rethrow = [](const Error& error) {
        if (error.code == ErrorCode::OVERSIZE) throw CapExceeded{"AccountProofV1: nested object exceeds its cap"};
        throw std::ios_base::failure("AccountProofV1: nested object malformed");
    };
    const auto read_bytes = [](SpanReader& reader, size_t cap) {
        const uint64_t size{ReadCompactSize(reader, /*range_check=*/false)};
        if (size > cap) throw CapExceeded{"AccountProofV1: length exceeds its cap"};
        std::vector<unsigned char> out(static_cast<size_t>(size));
        reader.read(MakeWritableByteSpan(out));
        return out;
    };
    return DecodeExactWith(bytes, MaxEncodedSize(profile), [&](SpanReader& reader) {
        AccountProofV1 out;
        uint8_t kind{0};
        reader >> out.account.id >> kind;
        if (kind == static_cast<uint8_t>(AccountProofKind::PRESENT)) {
            const std::vector<unsigned char> leaf_bytes{read_bytes(reader, AccountLeafV0::MaxEncodedSize(profile))};
            Result<AccountLeafV0> leaf{AccountLeafV0::Decode(leaf_bytes, profile)};
            if (!leaf.has_value()) rethrow(leaf.error());
            if (leaf->account != out.account) throw std::ios_base::failure("AccountProofV1: leaf of another account");
            out.leaf = std::move(leaf).value();
        } else if (kind != static_cast<uint8_t>(AccountProofKind::ABSENT)) {
            throw std::ios_base::failure("AccountProofV1: unknown kind");
        }
        const std::vector<unsigned char> proof_bytes{read_bytes(reader, SmtProof::MAX_ENCODED_BYTES)};
        Result<SmtProof> proof{SmtProof::Decode(proof_bytes)};
        if (!proof.has_value()) rethrow(proof.error());
        out.proof = std::move(proof).value();
        const bool own_leaf{out.proof.terminal == SmtTerminal::LEAF && out.proof.leaf.path == AccountPathV1(out.account)};
        if (out.leaf.has_value() != own_leaf) throw std::ios_base::failure("AccountProofV1: kind differs from the proof's terminal");
        return out;
    });
}

bool AccountProofV1::VerifiesUnder(const uint256& accounts_root) const
{
    const uint256 path{AccountPathV1(account)};
    if (leaf.has_value()) return leaf->account == account && SmtVerifyMember(accounts_root, path, leaf->LeafValue(), proof);
    return SmtVerifyAbsent(accounts_root, path, proof);
}

} // namespace flowmesh2
