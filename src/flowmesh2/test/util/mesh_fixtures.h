// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_MESH_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_MESH_FIXTURES_H

#include <crypto/bls.h>
#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/agreement/outbound.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/net/seat_mesh.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

/** TEST fixtures of the seat mesh suite (slice R13).
 *
 * RecordingMesh is a SeatMesh whose transport records every frame the base
 * class lets through (or answers with `refuse`) and whose Poll hands out
 * the frames a test put into `inbound`. No real transport exists in the
 * outline (M4-SIM-01, M4-NET-01). DurableVoteFixture walks the strict path
 * of section 7.3 to a real OutboundVote: ACCEPTED_PROPOSAL and INTENT,
 * B-INTENT, Sign, SIGNED_VOTE, B-VOTE, Confirm.
 */
namespace flowmesh2::test {

class RecordingMesh final : public net::SeatMesh
{
public:
    struct Sent {
        uint32_t seat{0};
        net::Lane lane{net::Lane::BULK};
        std::vector<unsigned char> frame;
    };

    static std::unique_ptr<RecordingMesh> Make(const net::MeshConfig& config)
    {
        RequireOk(CheckConfig(config), "SeatMesh::CheckConfig");
        return std::unique_ptr<RecordingMesh>{new RecordingMesh{config}};
    }

    std::vector<Sent> sent;
    std::deque<net::InboundFrame> inbound;
    std::optional<Error> refuse; //!< the transport's answer instead of queueing

    Result<std::optional<net::InboundFrame>> Poll() override
    {
        if (inbound.empty()) return Ok(std::optional<net::InboundFrame>{});
        net::InboundFrame frame{std::move(inbound.front())};
        inbound.pop_front();
        return Ok(std::optional<net::InboundFrame>{std::move(frame)});
    }

protected:
    Result<void> Transmit(uint32_t seat, net::Lane lane, std::span<const unsigned char> frame) override
    {
        if (refuse) return *refuse;
        sent.push_back(Sent{seat, lane, std::vector<unsigned char>{frame.begin(), frame.end()}});
        return Ok();
    }

private:
    explicit RecordingMesh(const net::MeshConfig& config) : net::SeatMesh{config} {}
};

//! A mesh config of `seat_count` seats for seat `self`, with frames up to the agreement maximum.
inline net::MeshConfig TestMeshConfig(uint32_t self, uint32_t seat_count)
{
    const ProfileParams profile{RegtestProfile()};
    return net::MeshConfig{self, seat_count, agreement::MaxMessageBytes(profile), profile};
}

//! A real OutboundVote of one storage slot: the strict signing path, every barrier passed.
struct DurableVoteFixture {
    std::unique_ptr<storage::MemoryJournal> journal;
    storage::SignerLog log;
    storage::SeatSigner signer;
    StorageSlot slot;

    explicit DurableVoteFixture(uint64_t height = 3)
        : journal{NewMemoryJournal(storage::JournalRole::SIGNER, 41)},
          log{storage::SignerLog::Create(*journal).value()},
          signer{storage::SeatSigner::FromIKM(Ikm(), log).value()},
          slot{MakeStorageSlot(height, 0, "mesh")}
    {
    }

    static std::span<const unsigned char> Ikm()
    {
        static const uint256 ikm{LabelHash("fm2/test/mesh/seat0")};
        return std::span<const unsigned char>{ikm.data(), ikm.size()};
    }

    agreement::OutboundVote Vote()
    {
        const storage::PendingProposal proposal{
            log.AppendAcceptedProposal(storage::ProposalKey{slot.header.authority_epoch, slot.header.height, 0}, slot.header, slot.body)
                .value()};
        const storage::PendingIntent intent{log.AppendIntent(CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), proposal).value()};
        RequireOk(log.Barrier(), "B-INTENT");
        const DurableIntent durable_intent{log.Confirm(intent).value()};
        const storage::VoteSignature sig{signer.Sign(durable_intent).value()};
        const storage::PendingSignedVote signed_vote{log.AppendSignedVote(durable_intent, sig).value()};
        RequireOk(log.Barrier(), "B-VOTE");
        return agreement::OutboundVote::From(log.Confirm(signed_vote).value());
    }
};

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_MESH_FIXTURES_H
