// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_AGREEMENT_H
#define B3COIN_NODE_FLOWMESH_AGREEMENT_H

#include <dbwrapper.h>
#include <flowmesh/agreement_wire.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace node {

enum class FlowMeshAgreementCrashPoint {
    AFTER_INTENT_PERSIST,
    AFTER_SIGNATURE,
    AFTER_SIGNED_PERSIST,
    BEFORE_DECISION_PERSIST,
    AFTER_DECISION_PERSIST,
};

/** Local benchmark observation only; never journaled or sent to peers. */
struct FlowMeshAgreementTrace {
    const char* operation;
    flowmesh::PreagreementContext context;
    uint32_t view;
    uint256 candidate;
    uint64_t span_id, parent_span_id, started_us, completed_us;
    std::optional<uint32_t> agreement_stage, seat_index;
};

struct FlowMeshAgreementCallbacks {
    using Bytes = std::vector<unsigned char>;
    // Missing evidence, unavailable canonical anchors, or failed execution
    // return nullopt. The engine waits and never interprets absence as a vote.
    // A restored blob contains public execution/authentication evidence only.
    std::function<std::optional<Bytes>(std::span<const unsigned char>,
        std::optional<std::span<const unsigned char>>)> validate_candidate;
    std::function<std::vector<bls::SecretKey>()> local_keys;
    // Required when reopening journal history from an earlier roster epoch.
    // Must return the complete roster authenticated by validated chain state.
    std::function<std::optional<flowmesh::ActiveFnBlsSeatSet>(
        const flowmesh::PreagreementContext&)> seat_set;
    std::function<std::optional<uint32_t>(uint32_t)> leader;
    // False means admission failed. Exact durable bytes remain retryable.
    std::function<bool(const flowmesh::AgreementMessage&)> publish;
    // Deterministic crash injection for tests; omitted by production callers.
    std::function<void(FlowMeshAgreementCrashPoint)> crash;
    // Both are absent in normal operation. Callbacks must not throw. Nested
    // spans include child work; WriteBatch(true) is a DB-call bracket, not a
    // measurement of the storage device's fsync alone.
    std::function<uint64_t()> trace_clock;
    std::function<void(const FlowMeshAgreementTrace&)> trace;
};

/** Single-slot PBFT agreement with an independently fsynced signing journal.
 *
 * All calls must be externally serialized. Open never signs or publishes: the
 * production-store bootstrap handshake must complete before other operations.
 * There is no unlock/reset operation. A storage failure permanently halts this
 * instance. The caller must retain the old V1 signing journal as well.
 */
class FlowMeshAgreement
{
public:
    FlowMeshAgreement(DBParams params, FlowMeshAgreementCallbacks callbacks);
    ~FlowMeshAgreement();
    FlowMeshAgreement(const FlowMeshAgreement&) = delete;
    FlowMeshAgreement& operator=(const FlowMeshAgreement&) = delete;

    bool Open(const flowmesh::PreagreementContext& context,
              const flowmesh::ActiveFnBlsSeatSet& seats,
              const uint256& journal_identity, bool allow_bootstrap,
              std::string& error);
    // Trusted integration boundary: call only AFTER the production store has
    // synchronously appended certified V1 history. Allows certified catch-up
    // across multiple slots, retaining every existing agreement slot on disk.
    bool Advance(const flowmesh::PreagreementContext& context,
                 const flowmesh::ActiveFnBlsSeatSet& seats,
                 std::string& error);
    bool SubmitCandidate(std::span<const unsigned char> entry, std::string& error);
    bool Receive(const flowmesh::AgreementMessage& message, std::string& error);
    bool Timeout(std::string& error);
    bool Retry(std::string& error);

    uint32_t View() const;
    const flowmesh::PreagreementContext& Context() const;
    bool Halted() const;
    const std::string& LastError() const;
    std::optional<uint256> RequiredCandidateHash() const;
    std::optional<uint256> DecidedCandidate() const;
    std::optional<std::vector<unsigned char>> CandidateBytes(const uint256& hash) const;
    std::optional<std::vector<unsigned char>> RestoreCandidateBlob(const uint256& hash) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace node
#endif // B3COIN_NODE_FLOWMESH_AGREEMENT_H
