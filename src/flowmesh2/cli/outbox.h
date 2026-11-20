// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CLI_OUTBOX_H
#define B3_FLOWMESH2_CLI_OUTBOX_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/file_journal.h>
#include <util/fs.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** The client's durable outbox (M5-CLI-01, DUR-4; ARCHITECTURE.md O-106).
 *
 * Every action is signed ONCE and its exact bytes are durable here BEFORE
 * they leave the client, so a crash between signing and sending, or between
 * sending and the reply, loses nothing: `fm2-cli resend` posts the same
 * bytes again and a seat that holds the action answers DUPLICATE. The outbox
 * never re-signs. It is a `storage::FileJournal` of role OUTBOX (instance
 * OUTBOX_INSTANCE, 1 MiB segments) in the client's directory, so it has the
 * journal's framing, CRC and chain and its recovery after a crash
 * (OpenExisting zeroes a torn tail). PROVISIONAL: TODO.md M5-CLI-01 names a
 * SQLite WAL outbox; the file journal is the outline's choice until the
 * owner rules (the stage 2 report asks).
 *
 * Records (storage/record.h, append-only):
 * - OUTBOX_ACTION (11): label (string) || bytes (the exact signed action).
 * - OUTBOX_RESULT (12): label || u8 outcome || u64 height || u32 index ||
 *   detail (string): what a certified reply proved about the action
 *   (INCLUDED or EXPIRED, verified by the client with b3verify before it is
 *   written) or a terminal status a seat reported (FAILED, not certified; the
 *   detail says what the seat said).
 *
 * The highest nonce of an account across every entry, with or without a
 * result, is the client's floor for the account's next nonce: a nonce once
 * signed is never signed again, whatever became of the action (the ledger's
 * nonce window, core/nonce_rule.h, would refuse the second one anyway).
 */
namespace flowmesh2::cli {

inline constexpr uint64_t OUTBOX_INSTANCE{1};
inline constexpr uint64_t OUTBOX_SEGMENT_BYTES{1024 * 1024};
//! Longest outbox record payload: a label and an action of the largest profile, with headroom.
inline constexpr size_t OUTBOX_MAX_PAYLOAD_BYTES{256 * 1024};

inline constexpr std::string_view OUTBOX_LABEL_TAKEN{"the outbox already holds this label"};    //!< (INVALID_INPUT)
inline constexpr std::string_view OUTBOX_LABEL_UNKNOWN{"no outbox entry with this label"};      //!< (INVALID_INPUT)
inline constexpr std::string_view OUTBOX_LABEL_EMPTY{"an outbox label is not empty"};           //!< (INVALID_INPUT)
inline constexpr std::string_view OUTBOX_RESULT_TWICE{"the outbox entry already has a result"}; //!< (ORDER_VIOLATION)
inline constexpr std::string_view OUTBOX_RECORD{"an outbox record does not decode"};            //!< (DECODE)
inline constexpr std::string_view OUTBOX_RESULT_ORPHAN{"an outbox result names no action"};     //!< (ORDER_VIOLATION)
inline constexpr std::string_view OUTBOX_DIR{"the outbox directory cannot be created"};         //!< (STORAGE_FATAL)

//! What became of an outbox action. APPEND-ONLY; zero is never a value.
enum class OutboxOutcome : uint8_t {
    INCLUDED = 1, //!< certified: the client verified the outcome and its proof
    EXPIRED = 2,  //!< certified: the client verified the EXPIRED proof
    FAILED = 3,   //!< a seat reported a terminal pool status; not certified
};
bool IsKnown(OutboxOutcome outcome);
std::string_view ToString(OutboxOutcome outcome);

struct OutboxResult {
    OutboxOutcome outcome{OutboxOutcome::FAILED};
    uint64_t height{0}; //!< INCLUDED: the including slot; EXPIRED: the proving head; FAILED: the seat's height, if any
    uint32_t index{0};  //!< INCLUDED: the outcome index
    std::string detail; //!< FAILED: what the seat said; else empty
    friend bool operator==(const OutboxResult&, const OutboxResult&) = default;
};

struct OutboxEntry {
    std::string label;
    std::vector<unsigned char> bytes; //!< the exact signed action
    ActionV2 action;                  //!< its decode (the outbox refuses bytes that do not decode)
    std::optional<OutboxResult> result;
};

class Outbox
{
public:
    //! A directory without a first segment starts a new outbox (it must be empty); one with it reopens the outbox (recovery like any journal).
    static Result<std::unique_ptr<Outbox>> Open(const fs::path& dir, const ProfileParams& profile);

    //! Make `bytes` durable under `label` (Append, then Barrier) before returning. The bytes must decode under the profile.
    Result<void> Add(const std::string& label, std::span<const unsigned char> bytes);
    //! Record what became of `label`, durable before returning. Once per label.
    Result<void> Record(const std::string& label, const OutboxResult& result);

    const std::vector<OutboxEntry>& Entries() const { return m_entries; }
    Result<OutboxEntry> Find(std::string_view label) const;
    //! The highest nonce this outbox ever signed for `account`, or nullopt.
    std::optional<uint64_t> HighestNonce(const AccountId& account) const;
    //! The journal's recovery report (zeros for a new outbox).
    const storage::RecoveryReport& Recovery() const { return m_journal->Recovery(); }

private:
    Outbox(std::unique_ptr<storage::FileJournal> journal, ProfileParams profile);
    Result<void> Load();

    std::unique_ptr<storage::FileJournal> m_journal;
    ProfileParams m_profile;
    std::vector<OutboxEntry> m_entries;
};

//! The record payloads (exposed for the tests).
std::vector<unsigned char> EncodeOutboxAction(std::string_view label, std::span<const unsigned char> bytes);
std::vector<unsigned char> EncodeOutboxResult(std::string_view label, const OutboxResult& result);

} // namespace flowmesh2::cli

#endif // B3_FLOWMESH2_CLI_OUTBOX_H
