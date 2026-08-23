// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_TRANSACTION_H
#define BITCOIN_PRIMITIVES_TRANSACTION_H

#include <attributes.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <primitives/transaction_identifier.h> // IWYU pragma: export
#include <script/script.h>
#include <serialize.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

/** An outpoint - a combination of a transaction hash and an index n into its vout */
class COutPoint
{
public:
    Txid hash;
    uint32_t n;

    static constexpr uint32_t NULL_INDEX = std::numeric_limits<uint32_t>::max();

    COutPoint(): n(NULL_INDEX) { }
    COutPoint(const Txid& hashIn, uint32_t nIn): hash(hashIn), n(nIn) { }

    SERIALIZE_METHODS(COutPoint, obj) { READWRITE(obj.hash, obj.n); }

    void SetNull() { hash.SetNull(); n = NULL_INDEX; }
    bool IsNull() const { return (hash.IsNull() && n == NULL_INDEX); }

    friend bool operator<(const COutPoint& a, const COutPoint& b)
    {
        return std::tie(a.hash, a.n) < std::tie(b.hash, b.n);
    }

    friend bool operator==(const COutPoint& a, const COutPoint& b)
    {
        return (a.hash == b.hash && a.n == b.n);
    }

    std::string ToString() const;
};

/** An input of a transaction.  It contains the location of the previous
 * transaction's output that it claims and a signature that matches the
 * output's public key.
 */
class CTxIn
{
public:
    COutPoint prevout;
    CScript scriptSig;
    uint32_t nSequence;
    CScriptWitness scriptWitness; //!< Only serialized through CTransaction

    /**
     * Setting nSequence to this value for every input in a transaction
     * disables nLockTime/IsFinalTx().
     * It fails OP_CHECKLOCKTIMEVERIFY/CheckLockTime() for any input that has
     * it set (BIP 65).
     * It has SEQUENCE_LOCKTIME_DISABLE_FLAG set (BIP 68/112).
     */
    static const uint32_t SEQUENCE_FINAL = 0xffffffff;
    /**
     * This is the maximum sequence number that enables both nLockTime and
     * OP_CHECKLOCKTIMEVERIFY (BIP 65).
     * It has SEQUENCE_LOCKTIME_DISABLE_FLAG set (BIP 68/112).
     */
    static const uint32_t MAX_SEQUENCE_NONFINAL{SEQUENCE_FINAL - 1};

    // Below flags apply in the context of BIP 68. BIP 68 requires the tx
    // version to be set to 2, or higher.
    /**
     * If this flag is set, CTxIn::nSequence is NOT interpreted as a
     * relative lock-time.
     * It skips SequenceLocks() for any input that has it set (BIP 68).
     * It fails OP_CHECKSEQUENCEVERIFY/CheckSequence() for any input that has
     * it set (BIP 112).
     */
    static const uint32_t SEQUENCE_LOCKTIME_DISABLE_FLAG = (1U << 31);

    /**
     * If CTxIn::nSequence encodes a relative lock-time and this flag
     * is set, the relative lock-time has units of 512 seconds,
     * otherwise it specifies blocks with a granularity of 1. */
    static const uint32_t SEQUENCE_LOCKTIME_TYPE_FLAG = (1 << 22);

    /**
     * If CTxIn::nSequence encodes a relative lock-time, this mask is
     * applied to extract that lock-time from the sequence field. */
    static const uint32_t SEQUENCE_LOCKTIME_MASK = 0x0000ffff;

    /**
     * In order to use the same number of bits to encode roughly the
     * same wall-clock duration, and because blocks are naturally
     * limited to occur every 600s on average, the minimum granularity
     * for time-based relative lock-time is fixed at 512 seconds.
     * Converting from CTxIn::nSequence to seconds is performed by
     * multiplying by 512 = 2^9, or equivalently shifting up by
     * 9 bits. */
    static const int SEQUENCE_LOCKTIME_GRANULARITY = 9;

    CTxIn()
    {
        nSequence = SEQUENCE_FINAL;
    }

    explicit CTxIn(COutPoint prevoutIn, CScript scriptSigIn=CScript(), uint32_t nSequenceIn=SEQUENCE_FINAL);
    CTxIn(Txid hashPrevTx, uint32_t nOut, CScript scriptSigIn=CScript(), uint32_t nSequenceIn=SEQUENCE_FINAL);

    SERIALIZE_METHODS(CTxIn, obj) { READWRITE(obj.prevout, obj.scriptSig, obj.nSequence); }

    friend bool operator==(const CTxIn& a, const CTxIn& b)
    {
        return (a.prevout   == b.prevout &&
                a.scriptSig == b.scriptSig &&
                a.nSequence == b.nSequence);
    }

    std::string ToString() const;
};

/** An output of a transaction.  It contains the public key that the next input
 * must be able to sign with to claim it.
 */
class CTxOut
{
public:
    CAmount nValue;
    CScript scriptPubKey;

    CTxOut()
    {
        SetNull();
    }

    CTxOut(const CAmount& nValueIn, CScript scriptPubKeyIn);

    SERIALIZE_METHODS(CTxOut, obj) { READWRITE(obj.nValue, obj.scriptPubKey); }

    void SetNull()
    {
        nValue = -1;
        scriptPubKey.clear();
    }

    bool IsNull() const
    {
        return (nValue == -1);
    }

    friend bool operator==(const CTxOut& a, const CTxOut& b)
    {
        return (a.nValue       == b.nValue &&
                a.scriptPubKey == b.scriptPubKey);
    }

    std::string ToString() const;
};

struct CMutableTransaction;

/**
 * One record of the B3 Modern Payload Area (MPA): a typed, versioned,
 * bounded evidence payload (BLS finality certificates, FINALITY_KEY
 * evidence, future proofs). Frame = payload_type u16 LE || payload_version
 * u16 LE || CompactSize-prefixed payload — byte-identical to the modern
 * creation-action frame. Records are NOT policy state (policy_params stays
 * <= 80 bytes); the MPA is segregated historical payload data: excluded from
 * the txid, carried only under a Modern serialization context (see
 * TransactionSerParams::allow_mpa) and committed into the block by the
 * MODERN_PAYLOAD_ROOT coinbase cell (plan Commit 7). Registry, activation
 * and per-type grammar live in modern/mpa.h — parsing a record never implies
 * consensus activation.
 */
struct CMpaRecord {
    uint16_t payload_type{0};
    uint16_t payload_version{0};
    std::vector<unsigned char> payload{};

    friend bool operator==(const CMpaRecord& a, const CMpaRecord& b)
    {
        return a.payload_type == b.payload_type && a.payload_version == b.payload_version && a.payload == b.payload;
    }
};

struct TransactionSerParams {
    const bool allow_witness;
    /**
     * Historical B3Coin transaction encoding: an nTime field follows the
     * version, there is no witness, and (for blocks) a trailing block
     * signature follows the transactions. Never set in the modern default
     * params; select it explicitly via legacy::TX_LEGACY (legacy/codec.h).
     */
    const bool legacy_time{false};
    /**
     * B3 Modern Payload Area: when true, BIP144 flag bit 0x02 is written for
     * a transaction that carries MPA records and accepted on read; when
     * false the MPA is neither written nor accepted (flag 0x02 then fails
     * closed like any unknown optional-data bit). This is a B3 Modern
     * serialization extension, NOT SegWit activation: bit 0x01 keeps its
     * witness meaning, the base (txid) form is unchanged, and only the
     * Modern decoding contexts (the marker-modern block codec, and future
     * modern relay/RPC contexts) select it. Never set for legacy_time.
     */
    const bool allow_mpa{false};
    SER_PARAMS_OPFUNC
};
static constexpr TransactionSerParams TX_WITH_WITNESS{.allow_witness = true};
static constexpr TransactionSerParams TX_NO_WITNESS{.allow_witness = false};
/** Canonical legacy-B3 params. Use through legacy::TX_LEGACY, not directly. */
static constexpr TransactionSerParams TX_LEGACY_B3{.allow_witness = false, .legacy_time = true};
/**
 * The B3 Modern full form: witness (if any) AND the Modern Payload Area.
 * Selected by the marker-modern block codec (primitives/block.h) and, later,
 * by the modern relay/RPC contexts. TX_WITH_WITNESS deliberately excludes
 * the MPA (it remains the wtxid form); the normative full-form identifier
 * (ptxid) arrives in plan Commit 6.
 */
static constexpr TransactionSerParams TX_MODERN{.allow_witness = true, .allow_mpa = true};

/**
 * Basic transaction serialization format:
 * - uint32_t version
 * - std::vector<CTxIn> vin
 * - std::vector<CTxOut> vout
 * - uint32_t nLockTime
 *
 * Extended transaction serialization format:
 * - uint32_t version
 * - unsigned char dummy = 0x00
 * - unsigned char flags (!= 0)
 * - std::vector<CTxIn> vin
 * - std::vector<CTxOut> vout
 * - if (flags & 1):
 *   - CScriptWitness scriptWitness; (deserialized into CTxIn)
 * - uint32_t nLockTime
 *
 * Historical B3Coin format (params.legacy_time; see legacy/codec.h):
 * - uint32_t version
 * - uint32_t nTime
 * - std::vector<CTxIn> vin
 * - std::vector<CTxOut> vout
 * - uint32_t nLockTime
 */
/**
 * Strict Modern Payload Area section codec (flag 0x02):
 *   count (CompactSize, 1..MAX_PAYLOAD_RECORDS_PER_TX)
 *   count x { payload_type u16 LE, payload_version u16 LE,
 *             payload_len CompactSize (<= MAX_PAYLOAD_RECORD_SIZE), payload bytes }
 * Every CompactSize must be minimal (ReadCompactSize enforces it); lengths
 * are checked BEFORE bytes are read; the running serialized section size
 * is bounded by MAX_PAYLOAD_SECTION_SIZE. Any violation throws, so a
 * transaction with a non-canonical section cannot be decoded at all.
 */
template<typename Stream>
void UnserializeMpaSection(Stream& s, std::vector<CMpaRecord>& out)
{
    out.clear();
    const uint64_t count{ReadCompactSize(s)};
    if (count == 0) throw std::ios_base::failure("Superfluous MPA record"); // flag 0x02 with no records
    if (count > MAX_PAYLOAD_RECORDS_PER_TX) throw std::ios_base::failure("MPA record count exceeds limit");
    size_t section_size{GetSizeOfCompactSize(count)};
    out.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
        CMpaRecord rec;
        s >> rec.payload_type >> rec.payload_version;
        const uint64_t len{ReadCompactSize(s)};
        if (len > MAX_PAYLOAD_RECORD_SIZE) throw std::ios_base::failure("MPA record exceeds limit");
        section_size += 4 + GetSizeOfCompactSize(len) + len;
        if (section_size > MAX_PAYLOAD_SECTION_SIZE) throw std::ios_base::failure("MPA section exceeds limit");
        rec.payload.resize(len);
        if (len) s.read(std::as_writable_bytes(std::span{rec.payload}));
        out.push_back(std::move(rec));
    }
}

template<typename Stream>
void SerializeMpaSection(Stream& s, const std::vector<CMpaRecord>& mpa)
{
    WriteCompactSize(s, mpa.size());
    for (const CMpaRecord& rec : mpa) {
        s << rec.payload_type << rec.payload_version;
        WriteCompactSize(s, rec.payload.size());
        if (!rec.payload.empty()) s.write(std::as_bytes(std::span{rec.payload}));
    }
}

template<typename Stream, typename TxType>
void UnserializeTransaction(TxType& tx, Stream& s, const TransactionSerParams& params)
{
    const bool fAllowWitness = params.allow_witness;
    const bool fAllowMpa = params.allow_mpa && !params.legacy_time;
    const bool fExtended = fAllowWitness || fAllowMpa; // may read the dummy/flags prefix

    s >> tx.version;
    if (params.legacy_time) {
        s >> tx.nTime;
    } else {
        tx.nTime = 0;
    }
    tx.m_legacy_encoding = params.legacy_time;
    unsigned char flags = 0;
    tx.vin.clear();
    tx.vout.clear();
    tx.mpa.clear();
    /* Try to read the vin. In case the dummy is there, this will be read as an empty vector. */
    s >> tx.vin;
    if (tx.vin.size() == 0 && fExtended) {
        /* We read a dummy or an empty vin. */
        s >> flags;
        if (flags != 0) {
            s >> tx.vin;
            s >> tx.vout;
        }
    } else {
        /* We read a non-empty vin. Assume a normal vout follows. */
        s >> tx.vout;
    }
    if ((flags & 1) && fAllowWitness) {
        /* The witness flag is present, and we support witnesses. */
        flags ^= 1;
        for (size_t i = 0; i < tx.vin.size(); i++) {
            s >> tx.vin[i].scriptWitness.stack;
        }
        if (!tx.HasWitness()) {
            /* It's illegal to encode witnesses when all witness stacks are empty. */
            throw std::ios_base::failure("Superfluous witness record");
        }
    }
    if ((flags & 2) && fAllowMpa) {
        /* The B3 Modern Payload Area is present and this context accepts it. */
        flags ^= 2;
        UnserializeMpaSection(s, tx.mpa);
    }
    if (flags) {
        /* Unknown flag in the serialization (incl. 0x02 outside a Modern context) */
        throw std::ios_base::failure("Unknown transaction optional data");
    }
    s >> tx.nLockTime;
}

template<typename Stream, typename TxType>
void SerializeTransaction(const TxType& tx, Stream& s, const TransactionSerParams& params)
{
    const bool fAllowWitness = params.allow_witness;
    const bool fAllowMpa = params.allow_mpa && !params.legacy_time;

    s << tx.version;
    if (params.legacy_time) {
        s << tx.nTime;
    }
    unsigned char flags = 0;
    // Consistency check
    if (fAllowWitness) {
        /* Check whether witnesses need to be serialized. */
        if (tx.HasWitness()) {
            flags |= 1;
        }
    }
    if (fAllowMpa && !tx.mpa.empty()) {
        /* The Modern Payload Area is serialized only under a Modern context;
         * the base (txid) and witness (wtxid) forms exclude it. */
        flags |= 2;
    }
    if (flags) {
        /* Use extended format in case witnesses / MPA are to be serialized. */
        std::vector<CTxIn> vinDummy;
        s << vinDummy;
        s << flags;
    }
    s << tx.vin;
    s << tx.vout;
    if (flags & 1) {
        for (size_t i = 0; i < tx.vin.size(); i++) {
            s << tx.vin[i].scriptWitness.stack;
        }
    }
    if (flags & 2) {
        SerializeMpaSection(s, tx.mpa);
    }
    s << tx.nLockTime;
}

template<typename TxType>
inline CAmount CalculateOutputValue(const TxType& tx)
{
    return std::accumulate(tx.vout.cbegin(), tx.vout.cend(), CAmount{0}, [](CAmount sum, const auto& txout) { return sum + txout.nValue; });
}


/** The basic transaction that is broadcasted on the network and contained in
 * blocks.  A transaction can contain multiple inputs and outputs.
 */
class CTransaction
{
public:
    // Default transaction version.
    static const uint32_t CURRENT_VERSION{2};

    // The local variables are made const to prevent unintended modification
    // without updating the cached hash value. However, CTransaction is not
    // actually immutable; deserialization and assignment are implemented,
    // and bypass the constness. This is safe, as they update the entire
    // structure, including the hash.
    const std::vector<CTxIn> vin;
    const std::vector<CTxOut> vout;
    const uint32_t version;
    /**
     * B3Coin legacy transaction timestamp. Zero and unserialized for modern
     * transactions; carried on the wire — and committed to by the txid —
     * only for transactions decoded through the explicit legacy codec.
     */
    const uint32_t nTime;
    const uint32_t nLockTime;
    /**
     * B3 Modern Payload Area records (empty for every legacy transaction and
     * for modern transactions without evidence). Excluded from the txid and
     * from the witness hash; serialized only under TX_MODERN.
     */
    const std::vector<CMpaRecord> mpa;

private:
    /** Memory only. */
    const bool m_has_witness;
    /**
     * Provenance: true when this transaction was constructed through the
     * legacy B3 codec, making the legacy encoding its identity — GetHash()
     * and GetWitnessHash() are then computed over the legacy bytes.
     */
    const bool m_legacy_encoding;
    const Txid hash;
    const Wtxid m_witness_hash;

    Txid ComputeHash() const;
    Wtxid ComputeWitnessHash() const;

    bool ComputeHasWitness() const;

public:
    /** Convert a CMutableTransaction into a CTransaction. */
    explicit CTransaction(const CMutableTransaction& tx);
    explicit CTransaction(CMutableTransaction&& tx);

    template <typename Stream>
    inline void Serialize(Stream& s) const {
        SerializeTransaction(*this, s, s.template GetParams<TransactionSerParams>());
    }

    /** This deserializing constructor is provided instead of an Unserialize method.
     *  Unserialize is not possible, since it would require overwriting const fields. */
    template <typename Stream>
    CTransaction(deserialize_type, const TransactionSerParams& params, Stream& s) : CTransaction(CMutableTransaction(deserialize, params, s)) {}
    template <typename Stream>
    CTransaction(deserialize_type, Stream& s) : CTransaction(CMutableTransaction(deserialize, s)) {}

    bool IsNull() const {
        return vin.empty() && vout.empty();
    }

    const Txid& GetHash() const LIFETIMEBOUND { return hash; }
    const Wtxid& GetWitnessHash() const LIFETIMEBOUND { return m_witness_hash; };

    // Return sum of txouts.
    CAmount GetValueOut() const;

    /**
     * Calculate the total transaction size in bytes, including witness data.
     * "Total Size" defined in BIP141 and BIP144.
     * @return Total transaction size in bytes
     */
    unsigned int ComputeTotalSize() const;

    bool IsCoinBase() const
    {
        return (vin.size() == 1 && vin[0].prevout.IsNull());
    }

    /** Historical B3Coin proof-of-stake transaction marker. */
    bool IsCoinStake() const
    {
        return !vin.empty() && !vin[0].prevout.IsNull() && vout.size() >= 2 &&
               vout[0].nValue == 0 && vout[0].scriptPubKey.empty();
    }

    friend bool operator==(const CTransaction& a, const CTransaction& b)
    {
        return a.GetWitnessHash() == b.GetWitnessHash();
    }

    std::string ToString() const;

    bool HasWitness() const { return m_has_witness; }
    bool HasMpa() const { return !mpa.empty(); }
    /** True when this transaction's identity is the legacy B3 encoding. */
    bool IsLegacyEncoded() const { return m_legacy_encoding; }
};

/** A mutable version of CTransaction. */
struct CMutableTransaction
{
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    uint32_t version;
    uint32_t nTime;
    uint32_t nLockTime;
    //! B3 Modern Payload Area records (see CTransaction::mpa).
    std::vector<CMpaRecord> mpa;
    /** Memory only; see CTransaction::IsLegacyEncoded(). */
    bool m_legacy_encoding{false};

    explicit CMutableTransaction();
    explicit CMutableTransaction(const CTransaction& tx);

    template <typename Stream>
    inline void Serialize(Stream& s) const {
        SerializeTransaction(*this, s, s.template GetParams<TransactionSerParams>());
    }

    template <typename Stream>
    inline void Unserialize(Stream& s) {
        UnserializeTransaction(*this, s, s.template GetParams<TransactionSerParams>());
    }

    template <typename Stream>
    CMutableTransaction(deserialize_type, const TransactionSerParams& params, Stream& s) {
        UnserializeTransaction(*this, s, params);
    }

    template <typename Stream>
    CMutableTransaction(deserialize_type, Stream& s) {
        Unserialize(s);
    }

    /** Compute the hash of this CMutableTransaction. This is computed on the
     * fly, as opposed to GetHash() in CTransaction, which uses a cached result.
     */
    Txid GetHash() const;

    bool HasWitness() const
    {
        for (size_t i = 0; i < vin.size(); i++) {
            if (!vin[i].scriptWitness.IsNull()) {
                return true;
            }
        }
        return false;
    }

    /** True when this transaction's identity is the legacy B3 encoding. */
    bool IsLegacyEncoded() const { return m_legacy_encoding; }
};

typedef std::shared_ptr<const CTransaction> CTransactionRef;
template <typename Tx> static inline CTransactionRef MakeTransactionRef(Tx&& txIn) { return std::make_shared<const CTransaction>(std::forward<Tx>(txIn)); }

#endif // BITCOIN_PRIMITIVES_TRANSACTION_H
