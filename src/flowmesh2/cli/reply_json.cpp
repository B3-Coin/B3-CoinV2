// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/cli/reply_json.h>

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/netio/api_json.h>

namespace flowmesh2::cli {

namespace {

Error Shape() { return Error{ErrorCode::DECODE, JSON_SHAPE}; }

//! `value[key]` when `value` is an object holding `key`.
Result<const UniValue*> Field(const UniValue& value, const char* key)
{
    if (!value.isObject() || !value.exists(key)) return Shape();
    return Ok(&value[key]);
}

Result<uint256> HashField(const UniValue& value, const char* key)
{
    const Result<const UniValue*> field{Field(value, key)};
    if (!field.has_value()) return field.error();
    return ParseHash(**field);
}

Result<std::vector<unsigned char>> BytesField(const UniValue& value, const char* key)
{
    const Result<const UniValue*> field{Field(value, key)};
    if (!field.has_value()) return field.error();
    return ParseBytes(**field);
}

Result<uint64_t> U64Field(const UniValue& value, const char* key)
{
    const Result<const UniValue*> field{Field(value, key)};
    if (!field.has_value()) return field.error();
    return ParseU64(**field);
}

Result<std::string> StringField(const UniValue& value, const char* key)
{
    const Result<const UniValue*> field{Field(value, key)};
    if (!field.has_value()) return field.error();
    if (!(*field)->isStr()) return Shape();
    return Ok((*field)->get_str());
}

std::string OptionalString(const UniValue& value, const char* key)
{
    if (value.isObject() && value.exists(key) && value[key].isStr()) return value[key].get_str();
    return std::string{};
}

Result<MerkleProof> ParseProof(const UniValue& value)
{
    const Result<std::vector<unsigned char>> encoded{BytesField(value, "encoded")};
    if (!encoded.has_value()) return encoded.error();
    return DecodeExact<MerkleProof>(*encoded, 8 + 8 + 9 + 32 * 64);
}

} // namespace

Result<UniValue> ParseJson(std::string_view text)
{
    UniValue value;
    if (!value.read(std::string{text})) return Error{ErrorCode::DECODE, JSON_TEXT};
    return Ok(std::move(value));
}

Result<uint256> ParseHash(const UniValue& value)
{
    if (!value.isStr()) return Shape();
    const std::optional<uint256> hash{uint256::FromHex(value.get_str())};
    if (!hash.has_value()) return Error{ErrorCode::DECODE, JSON_HEX};
    return Ok(*hash);
}

Result<std::vector<unsigned char>> ParseBytes(const UniValue& value)
{
    if (!value.isStr()) return Shape();
    std::optional<std::vector<unsigned char>> bytes{netio::FromHex(value.get_str())};
    if (!bytes.has_value()) return Error{ErrorCode::DECODE, JSON_HEX};
    return Ok(std::move(*bytes));
}

Result<uint64_t> ParseU64(const UniValue& value)
{
    if (!value.isNum()) return Shape();
    try {
        return Ok(value.getInt<uint64_t>());
    } catch (const std::runtime_error&) {
        return Shape();
    }
}

Result<read::ReplyHead> ParseReplyHead(const UniValue& value)
{
    read::ReplyHead head;
    const Result<const UniValue*> scope{Field(value, "scope")};
    if (!scope.has_value()) return scope.error();
    const Result<const UniValue*> commit{Field(value, "commit")};
    if (!commit.has_value()) return commit.error();
    for (const auto& [key, target] : std::initializer_list<std::pair<const char*, uint256*>>{{"domain", &head.scope.domain},
                                                                                           {"configuration", &head.scope.configuration},
                                                                                           {"validator_set", &head.scope.validator_set},
                                                                                           {"anchor", &head.scope.anchor}}) {
        const Result<uint256> hash{HashField(**scope, key)};
        if (!hash.has_value()) return hash.error();
        *target = *hash;
    }
    for (const auto& [key, target] : std::initializer_list<std::pair<const char*, uint64_t*>>{{"epoch", &head.scope.epoch},
                                                                                            {"authority_revision", &head.scope.authority_revision}}) {
        const Result<uint64_t> number{U64Field(**scope, key)};
        if (!number.has_value()) return number.error();
        *target = *number;
    }
    for (const auto& [key, target] : std::initializer_list<std::pair<const char*, uint256*>>{{"id", &head.commit.id},
                                                                                           {"parent", &head.commit.parent},
                                                                                           {"state_root", &head.commit.state_root}}) {
        const Result<uint256> hash{HashField(**commit, key)};
        if (!hash.has_value()) return hash.error();
        *target = *hash;
    }
    const Result<uint64_t> sequence{U64Field(**commit, "sequence")};
    if (!sequence.has_value()) return sequence.error();
    head.commit.sequence = *sequence;
    const Result<uint64_t> publication{U64Field(value, "publication")};
    if (!publication.has_value()) return publication.error();
    head.publication = *publication;
    const Result<std::vector<unsigned char>> envelope{BytesField(value, "envelope")};
    if (!envelope.has_value()) return envelope.error();
    Result<read::CertifiedEnvelope> decoded{read::CertifiedEnvelope::Decode(*envelope)};
    if (!decoded.has_value()) return decoded.error();
    head.envelope = std::move(*decoded);
    const Result<std::string> label{StringField(value, "label")};
    if (!label.has_value()) return label.error();
    if (*label != read::ToString(read::ReadLabel::CERTIFIED)) return Error{ErrorCode::DECODE, JSON_LABEL};
    head.label = read::ReadLabel::CERTIFIED;
    return Ok(std::move(head));
}

Result<read::AccountWitness> ParseWitness(const UniValue& value, const ProfileParams& profile)
{
    read::AccountWitness witness;
    const Result<const UniValue*> leaf{Field(value, "leaf")};
    if (!leaf.has_value()) return leaf.error();
    const Result<std::vector<unsigned char>> leaf_bytes{BytesField(**leaf, "encoded")};
    if (!leaf_bytes.has_value()) return leaf_bytes.error();
    Result<AccountLeafV0> decoded_leaf{AccountLeafV0::Decode(*leaf_bytes, profile)};
    if (!decoded_leaf.has_value()) return decoded_leaf.error();
    witness.leaf = std::move(*decoded_leaf);
    const Result<std::vector<unsigned char>> roots{BytesField(value, "roots")};
    if (!roots.has_value()) return roots.error();
    const Result<StateRootsV0> decoded_roots{DecodeExact<StateRootsV0>(*roots, StateRootsV0::ENCODED_BYTES)};
    if (!decoded_roots.has_value()) return decoded_roots.error();
    witness.roots = *decoded_roots;
    const Result<const UniValue*> proof{Field(value, "proof")};
    if (!proof.has_value()) return proof.error();
    Result<MerkleProof> decoded_proof{ParseProof(**proof)};
    if (!decoded_proof.has_value()) return decoded_proof.error();
    witness.proof = std::move(*decoded_proof);
    return Ok(std::move(witness));
}

Result<read::IncludedOutcome> ParseIncluded(const UniValue& value, const ProfileParams& profile)
{
    read::IncludedOutcome included;
    const Result<const UniValue*> ref{Field(value, "ref")};
    if (!ref.has_value()) return ref.error();
    const Result<uint64_t> height{U64Field(**ref, "height")};
    if (!height.has_value()) return height.error();
    const Result<uint64_t> index{U64Field(**ref, "index")};
    if (!index.has_value() || *index > UINT32_MAX) return Shape();
    included.ref.height = *height;
    included.ref.index = static_cast<uint32_t>(*index);
    const Result<std::vector<unsigned char>> envelope{BytesField(value, "envelope")};
    if (!envelope.has_value()) return envelope.error();
    Result<read::CertifiedEnvelope> decoded{read::CertifiedEnvelope::Decode(*envelope)};
    if (!decoded.has_value()) return decoded.error();
    included.envelope = std::move(*decoded);
    const Result<uint64_t> outcome_index{U64Field(value, "outcome_index")};
    if (!outcome_index.has_value() || *outcome_index > UINT32_MAX) return Shape();
    included.outcome.index = static_cast<uint32_t>(*outcome_index);
    const Result<const UniValue*> outcome{Field(value, "outcome")};
    if (!outcome.has_value()) return outcome.error();
    const Result<std::vector<unsigned char>> outcome_bytes{BytesField(**outcome, "encoded")};
    if (!outcome_bytes.has_value()) return outcome_bytes.error();
    Result<OutcomeV2> decoded_outcome{OutcomeV2::Decode(*outcome_bytes, profile)};
    if (!decoded_outcome.has_value()) return decoded_outcome.error();
    included.outcome.outcome = std::move(*decoded_outcome);
    const Result<const UniValue*> proof{Field(value, "proof")};
    if (!proof.has_value()) return proof.error();
    Result<MerkleProof> decoded_proof{ParseProof(**proof)};
    if (!decoded_proof.has_value()) return decoded_proof.error();
    included.outcome.proof = std::move(*decoded_proof);
    return Ok(std::move(included));
}

Result<StateReply> ParseStateReply(const UniValue& value, const ProfileParams& profile)
{
    StateReply reply;
    const Result<const UniValue*> head{Field(value, "head")};
    if (!head.has_value()) return head.error();
    Result<read::ReplyHead> parsed{ParseReplyHead(**head)};
    if (!parsed.has_value()) return parsed.error();
    reply.head = std::move(*parsed);
    const Result<std::string> status{StringField(value, "status")};
    if (!status.has_value()) return status.error();
    reply.found = *status == "FOUND";
    reply.reason = OptionalString(value, "reason");
    if (reply.found) {
        const Result<const UniValue*> witness{Field(value, "witness")};
        if (!witness.has_value()) return witness.error();
        Result<read::AccountWitness> parsed_witness{ParseWitness(**witness, profile)};
        if (!parsed_witness.has_value()) return parsed_witness.error();
        reply.witness = std::move(*parsed_witness);
    }
    return Ok(std::move(reply));
}

Result<StatusReply> ParseStatusReply(const UniValue& value, const ProfileParams& profile)
{
    StatusReply reply;
    const Result<std::string> status{StringField(value, "status")};
    if (!status.has_value()) return status.error();
    reply.status = *status;
    reply.reason = OptionalString(value, "reason");
    if (value.exists("head")) {
        Result<read::ReplyHead> head{ParseReplyHead(value["head"])};
        if (!head.has_value()) return head.error();
        reply.head = std::move(*head);
    }
    if (value.exists("included")) {
        Result<read::IncludedOutcome> included{ParseIncluded(value["included"], profile)};
        if (!included.has_value()) return included.error();
        reply.included = std::move(*included);
    }
    if (value.exists("expiry")) {
        Result<read::AccountWitness> expiry{ParseWitness(value["expiry"], profile)};
        if (!expiry.has_value()) return expiry.error();
        reply.expiry = std::move(*expiry);
    }
    if (value.exists("pending") && value["pending"].isObject()) {
        reply.pending_status = OptionalString(value["pending"], "status");
        if (value["pending"].exists("height")) {
            const Result<uint64_t> height{ParseU64(value["pending"]["height"])};
            if (height.has_value()) reply.pending_height = *height;
        }
    }
    return Ok(std::move(reply));
}

Result<AdmissionReplyJson> ParseAdmissionReply(const UniValue& value, const ProfileParams& profile)
{
    AdmissionReplyJson reply;
    const Result<std::string> code{StringField(value, "code")};
    if (!code.has_value()) return code.error();
    reply.code = *code;
    reply.reason = OptionalString(value, "reason");
    if (value.exists("action_id")) {
        const Result<uint256> id{ParseHash(value["action_id"])};
        if (!id.has_value()) return id.error();
        reply.id = ActionId{*id};
    }
    if (value.exists("head_height")) {
        const Result<uint64_t> height{ParseU64(value["head_height"])};
        if (!height.has_value()) return height.error();
        reply.head_height = *height;
    }
    if (value.exists("outcome")) {
        Result<read::IncludedOutcome> outcome{ParseIncluded(value["outcome"], profile)};
        if (!outcome.has_value()) return outcome.error();
        reply.outcome = std::move(*outcome);
    }
    return Ok(std::move(reply));
}

Result<HealthReplyJson> ParseHealthReply(const UniValue& value)
{
    HealthReplyJson reply;
    const Result<const UniValue*> fenced{Field(value, "fenced")};
    if (!fenced.has_value()) return fenced.error();
    if (!(*fenced)->isBool()) return Shape();
    reply.fenced = (*fenced)->get_bool();
    if (value.exists("published_height")) {
        const Result<uint64_t> height{ParseU64(value["published_height"])};
        if (!height.has_value()) return height.error();
        reply.published_height = *height;
    }
    if (value.exists("pool_live")) {
        const Result<uint64_t> live{ParseU64(value["pool_live"])};
        if (live.has_value()) reply.pool_live = *live;
    }
    return Ok(std::move(reply));
}

} // namespace flowmesh2::cli
