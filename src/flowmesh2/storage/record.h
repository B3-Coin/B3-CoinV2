// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_RECORD_H
#define B3_FLOWMESH2_STORAGE_RECORD_H

#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/** Journal records and their framing (slice R8; ARCHITECTURE.md section 7.1).
 *
 * A journal is a sequence of framed records. Each record is
 *
 *     [u32 len][u32 crc][u8 type][payload: len bytes][chain: 32 bytes]
 *
 * with integers little-endian and
 *
 *     chain = SHA256(prev || u8 type || payload)      (single SHA-256)
 *     crc   = CRC32C(u8 type || payload || chain)
 *
 * where `prev` is the previous record's chain and, for the first record of a
 * journal, JournalGenesisChain(role, instance_id) = Tagged(JOURNAL, u8 role
 * || u64 instance_id). The chain binds every record to all records before it
 * and to its journal instance, so a record copied from another instance (a
 * splice) fails the chain check even though its own CRC is intact.
 *
 * `len` counts the payload only; a record occupies RECORD_OVERHEAD_BYTES +
 * len bytes. A payload longer than MAX_RECORD_PAYLOAD_BYTES is refused by the
 * encoder (OVERSIZE) and by the scanner (DECODE, RECORD_LENGTH). Type 0 is
 * never a record type, so the all-zero header [0][0][0] marks the zero-filled
 * tail of a pre-zeroed segment (FileJournal).
 *
 * RecordType (APPEND-ONLY, owner-protected registry; pinned by record_tests):
 *   INTENT=1, SIGNED_VOTE=2, DECISION=3, APPLIED=4, SNAPSHOT_MARK=5,
 *   ACCEPTED_PROPOSAL=6, VIEW_STATE=7, NEW_VIEW_ACCEPTED=8, PREPARED_QC=9.
 * JournalRole (append-only): SIGNER=1 accepts {1, 2, 6, 7, 8}; COMMIT=2
 * accepts {3, 4, 5, 9}. The signer journal is never deleted (DUR-2).
 *
 * Everything here is deterministic: no I/O, clock, lock or allocation beyond
 * the returned vectors.
 */
namespace flowmesh2::storage {

//! Record types. APPEND-ONLY: never renumber, reuse or remove a value.
enum class RecordType : uint8_t {
    INTENT = 1,            //!< signer: a vote intent, durable before the first signature (B-INTENT)
    SIGNED_VOTE = 2,       //!< signer: the signed vote, durable before it leaves (B-VOTE)
    DECISION = 3,          //!< commit: header, certificate and body (B-DECISION; O-16)
    APPLIED = 4,           //!< commit: slot, header hash and state root (B-APPLIED)
    SNAPSHOT_MARK = 5,     //!< commit: a snapshot boundary (M4-STO-03)
    ACCEPTED_PROPOSAL = 6, //!< signer: the proposal the seat accepted, with its body (O-16)
    VIEW_STATE = 7,        //!< signer: durable PBFT view state (M4-STO-07)
    NEW_VIEW_ACCEPTED = 8, //!< signer: an accepted new-view message (M4-STO-07)
    PREPARED_QC = 9,       //!< commit: a prepared quorum certificate (M4-STO-07)
    CONTROL_INTENT = 10,   //!< control: immutable view/report intent, durable before signing
    SIGNED_CONTROL = 11,   //!< control: exact signature, durable before publication
};

struct RecordTypeInfo {
    RecordType type;
    std::string_view name;
};

//! The registry in ascending integer order, one entry per RecordType value.
std::span<const RecordTypeInfo> RecordTypeRegistry();
//! True only for a value listed in RecordTypeRegistry().
bool IsKnown(RecordType type);
//! Total: the registry name, or "UNKNOWN".
std::string_view ToString(RecordType type);

//! Which journal a record belongs to. APPEND-ONLY.
enum class JournalRole : uint8_t {
    SIGNER = 1, //!< ACCEPTED_PROPOSAL, INTENT, SIGNED_VOTE, VIEW_STATE, NEW_VIEW_ACCEPTED
    COMMIT = 2, //!< DECISION, APPLIED, SNAPSHOT_MARK, PREPARED_QC
};

bool IsKnown(JournalRole role);
//! Total: "SIGNER", "COMMIT" or "UNKNOWN".
std::string_view ToString(JournalRole role);
//! True iff `role` is known, `type` is known and the role's journal holds that type.
bool RoleAccepts(JournalRole role, RecordType type);

inline constexpr size_t RECORD_HEADER_BYTES{9};   //!< u32 len, u32 crc, u8 type
inline constexpr size_t RECORD_CHAIN_BYTES{32};
inline constexpr size_t RECORD_OVERHEAD_BYTES{RECORD_HEADER_BYTES + RECORD_CHAIN_BYTES};
/** Longest record payload. A DECISION of the provisional regtest profile
 * (366-byte header, certificate, body of at most 1,024 originals and 1,024
 * facts) stays far below it (record_tests pins that). PROVISIONAL.
 */
inline constexpr size_t MAX_RECORD_PAYLOAD_BYTES{16 * 1024 * 1024};

// Static error details of ScanRecords (ErrorCode in parentheses).
inline constexpr std::string_view RECORD_LENGTH{"RECORD_LENGTH"};  //!< (DECODE) torn header, len above the cap or beyond the input
inline constexpr std::string_view RECORD_CRC{"RECORD_CRC"};        //!< (VERIFY_FAILED) CRC32C mismatch
inline constexpr std::string_view RECORD_TYPE{"RECORD_TYPE"};      //!< (DECODE) unregistered type
inline constexpr std::string_view RECORD_ROLE{"RECORD_ROLE"};      //!< (ORDER_VIOLATION) type not held by this role
inline constexpr std::string_view RECORD_CHAIN{"RECORD_CHAIN"};    //!< (VERIFY_FAILED) chain break, reorder or splice
inline constexpr std::string_view RECORD_TAIL{"RECORD_TAIL"};      //!< (DECODE) non-zero bytes after the zero tail

//! Tagged(JOURNAL, u8 role || u64 instance_id): the `prev` of a journal's first record.
uint256 JournalGenesisChain(JournalRole role, uint64_t instance_id);
//! SHA256(prev || u8 type || payload).
uint256 NextChain(const uint256& prev, RecordType type, std::span<const unsigned char> payload);
//! CRC32C(u8 type || payload || chain).
uint32_t RecordCrc(RecordType type, std::span<const unsigned char> payload, const uint256& chain);

//! The framed bytes of one record and its chain value (the next record's prev).
struct EncodedRecord {
    std::vector<unsigned char> bytes;
    uint256 chain;
};

/** Frame one record after `prev`. INVALID_INPUT for an unregistered type,
 * OVERSIZE for a payload above MAX_RECORD_PAYLOAD_BYTES. Role checks are the
 * journal's (Journal::Append).
 */
Result<EncodedRecord> EncodeRecord(const uint256& prev, RecordType type, std::span<const unsigned char> payload);

//! One record as read back. `seq` is its 1-based position in the journal.
struct JournalRecord {
    uint64_t seq{0};
    RecordType type{RecordType::INTENT};
    std::vector<unsigned char> payload;
    uint256 chain;

    friend bool operator==(const JournalRecord&, const JournalRecord&) = default;
};

struct ScanResult {
    std::vector<JournalRecord> records;
    //! The chain value after the last record (the genesis chain if there is none).
    uint256 last_chain;
    //! Offset just past the last record: where the next record goes.
    size_t end_offset{0};
};

/** Read every record of a journal image of `role` and `instance_id`.
 *
 * Accepts the image if it is a sequence of valid records followed by nothing
 * or by a zero tail (every remaining byte zero). Refuses, with the first
 * problem in byte order: a header cut short by the end of the input or a
 * length above the cap or beyond the input (DECODE, RECORD_LENGTH); a CRC
 * mismatch (VERIFY_FAILED, RECORD_CRC); an unregistered type (DECODE,
 * RECORD_TYPE); a type the role does not hold (ORDER_VIOLATION, RECORD_ROLE);
 * a chain value that does not continue the chain from JournalGenesisChain
 * (VERIFY_FAILED, RECORD_CHAIN), which is how a flipped payload with a
 * recomputed CRC, a reordering, a dropped record and a splice from another
 * instance show up; and a non-zero byte after an all-zero header (DECODE,
 * RECORD_TAIL). INVALID_INPUT for an unknown role.
 *
 * The outline does not tell a torn last record apart from corruption in the
 * middle; both are refusals. Torn-tail recovery and key retirement on a gap
 * or CRC break are M4-STO-01.
 */
Result<ScanResult> ScanRecords(std::span<const unsigned char> bytes, JournalRole role, uint64_t instance_id);

/** ScanRecords of a segment after the first (M4-STO-06): the image's first
 * record must continue `prev` (the chain after the previous segment's last
 * record, which also binds the instance) and is numbered `first_seq`.
 * `end_offset` is relative to `bytes`; `last_chain` is `prev` when the image
 * holds no record. ScanRecords is ScanRecordsFrom(bytes, role,
 * JournalGenesisChain(role, instance_id), 1). INVALID_INPUT for an unknown
 * role or a zero first_seq.
 */
Result<ScanResult> ScanRecordsFrom(std::span<const unsigned char> bytes, JournalRole role, const uint256& prev, uint64_t first_seq);

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_RECORD_H
