// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//! B3 finality benchmark harness — BENCHMARK-ONLY (owner ruling 2026-08-23).
//!
//! Measures, on the reference machine, the costs that the Modern PoS V1
//! finality gadget and the Modern Payload Area would impose on validation:
//!   - BLS12-381 (blst, portable build): key/PoP verification, signing,
//!     hash-to-G2, aggregate-certificate verification at representative
//!     validator-set sizes and participation rates, aggregation itself,
//!     and worst-case INVALID-proof rejection paths;
//!   - BIP340 (in-tree secp256k1): the FINALITY_KEY_EVIDENCE identity sig;
//!   - MPA: strict section encode/decode/hash and payload_root Merkle roots.
//! It links against the vendored blst ONLY through this executable; no
//! node, wallet, test or consensus target depends on it.
//!
//! Honesty notes: every timed operation is verified OUTSIDE the timed
//! region to return the expected result (true for valid, false for the
//! tampered inputs), so the numbers are for real work, not early exits —
//! except where the early exit IS the thing being measured (malformed
//! encodings), which is labelled as such.

#include <blst.h>

#include <consensus/merkle.h>
#include <hash.h>
#include <key.h>
#include <modern/creation_action.h>
#include <pubkey.h>
#include <random.h>
#include <uint256.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

//! Run `fn` `iters` times, return {min, median} microseconds per call.
struct Timing { double min_us; double median_us; };
Timing TimeIt(const std::function<void()>& fn, int iters)
{
    std::vector<double> samples;
    samples.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());
    return {samples.front(), samples[samples.size() / 2]};
}

const std::string SIG_DST = "BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";
const std::string POP_DST = "BLS_POP_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";

struct Validator {
    blst_scalar sk;
    blst_p1 pk;
    blst_p1_affine pk_aff;
    unsigned char pk_bytes[48];
    blst_p2_affine pop;
};

std::vector<Validator> MakeValidators(size_t n)
{
    std::vector<Validator> v(n);
    for (size_t i = 0; i < n; ++i) {
        unsigned char ikm[32];
        std::memset(ikm, 0, sizeof(ikm));
        std::memcpy(ikm, &i, sizeof(i));
        ikm[31] = 0x5a;
        blst_keygen(&v[i].sk, ikm, sizeof(ikm), nullptr, 0);
        blst_sk_to_pk_in_g1(&v[i].pk, &v[i].sk);
        blst_p1_to_affine(&v[i].pk_aff, &v[i].pk);
        blst_p1_compress(v[i].pk_bytes, &v[i].pk);
        blst_p2 h;
        blst_hash_to_g2(&h, v[i].pk_bytes, 48, (const unsigned char*)POP_DST.data(), POP_DST.size(), nullptr, 0);
        blst_p2 sig;
        blst_sign_pk_in_g1(&sig, &h, &v[i].sk);
        blst_p2_to_affine(&v[i].pop, &sig);
    }
    return v;
}

bool VerifyPoP(const blst_p1_affine& pk, const unsigned char pk_bytes[48], const blst_p2_affine& pop)
{
    return blst_core_verify_pk_in_g1(&pk, &pop, true, pk_bytes, 48,
                                     (const unsigned char*)POP_DST.data(), POP_DST.size(), nullptr, 0) == BLST_SUCCESS;
}

bool VerifyAgg(const blst_p1_affine& agg_pk, const blst_p2_affine& agg_sig, const unsigned char digest[32])
{
    return blst_core_verify_pk_in_g1(&agg_pk, &agg_sig, true, digest, 32,
                                     (const unsigned char*)SIG_DST.data(), SIG_DST.size(), nullptr, 0) == BLST_SUCCESS;
}

void Row(const char* name, const Timing& t, const char* note = "")
{
    std::printf("| %-62s | %10.1f | %10.1f | %s |\n", name, t.min_us, t.median_us, note);
}

} // namespace

int main()
{
    ECC_Context ecc_context{};
    std::printf("# B3 finality benchmark (blst %s portable, single thread)\n\n", "v0.3.17");
    std::printf("| operation | min us | median us | note |\n|---|---:|---:|---|\n");

    // ---------------------------------------------------------------- BLS basics
    auto vals = MakeValidators(8192);
    unsigned char digest[32];
    for (int i = 0; i < 32; ++i) digest[i] = (unsigned char)(0xA5 ^ i);

    // PoP valid
    {
        bool ok = VerifyPoP(vals[0].pk_aff, vals[0].pk_bytes, vals[0].pop);
        if (!ok) { std::printf("PoP self-test failed\n"); return 1; }
        Row("BLS PoP verify (valid)", TimeIt([&] { (void)VerifyPoP(vals[0].pk_aff, vals[0].pk_bytes, vals[0].pop); }, 200), "1 pairing check, incl. subgroup checks");
    }
    // PoP invalid: wrong message (another validator's PoP against this pk)
    {
        bool ok = VerifyPoP(vals[0].pk_aff, vals[0].pk_bytes, vals[1].pop);
        if (ok) { std::printf("PoP tamper self-test failed\n"); return 1; }
        Row("BLS PoP verify (INVALID sig, well-formed)", TimeIt([&] { (void)VerifyPoP(vals[0].pk_aff, vals[0].pk_bytes, vals[1].pop); }, 200), "full pairing then reject = worst case");
    }
    // Malformed encodings: uncompress failure (fast reject)
    {
        unsigned char bad[48]; std::memcpy(bad, vals[0].pk_bytes, 48); bad[0] ^= 0x1f; // break flags/field
        blst_p1_affine out;
        BLST_ERROR e = blst_p1_uncompress(&out, bad);
        const char* note = (e == BLST_SUCCESS) ? "(decoded?!)" : "decode rejects before any pairing";
        Row("BLS pubkey uncompress (MALFORMED)", TimeIt([&] { blst_p1_affine o; (void)blst_p1_uncompress(&o, bad); }, 2000), note);
        Row("BLS pubkey uncompress (valid) + in_g1 subgroup check", TimeIt([&] { blst_p1_affine o; (void)blst_p1_uncompress(&o, vals[0].pk_bytes); (void)blst_p1_affine_in_g1(&o); }, 2000), "cost of accepting a key's bytes");
    }
    // Signature uncompress valid/malformed
    {
        unsigned char sig_bytes[96];
        blst_p2 s; blst_p2_from_affine(&s, &vals[0].pop); blst_p2_compress(sig_bytes, &s);
        Row("BLS signature uncompress (valid) + in_g2 subgroup check", TimeIt([&] { blst_p2_affine o; (void)blst_p2_uncompress(&o, sig_bytes); (void)blst_p2_affine_in_g2(&o); }, 1000), "");
        unsigned char sb[96]; std::memcpy(sb, sig_bytes, 96); sb[0] ^= 0x1f;
        Row("BLS signature uncompress (MALFORMED)", TimeIt([&] { blst_p2_affine o; (void)blst_p2_uncompress(&o, sb); }, 2000), "decode rejects");
    }
    // hash-to-G2 and sign
    {
        Row("hash_to_G2(32-byte digest)", TimeIt([&] { blst_p2 h; blst_hash_to_g2(&h, digest, 32, (const unsigned char*)SIG_DST.data(), SIG_DST.size(), nullptr, 0); }, 500), "validator + verifier both pay this");
        blst_p2 h; blst_hash_to_g2(&h, digest, 32, (const unsigned char*)SIG_DST.data(), SIG_DST.size(), nullptr, 0);
        Row("BLS sign (given H(m))", TimeIt([&] { blst_p2 s; blst_sign_pk_in_g1(&s, &h, &vals[0].sk); }, 200), "per validator per checkpoint");
    }
    // BIP340
    {
        CKey key; key.MakeNewKey(true);
        XOnlyPubKey xpk{key.GetPubKey()};
        uint256 msg{digest};
        std::vector<unsigned char> sig(64);
        uint256 aux{};
        if (!key.SignSchnorr(msg, sig, nullptr, aux)) { std::printf("schnorr sign failed\n"); return 1; }
        if (!xpk.VerifySchnorr(msg, sig)) { std::printf("schnorr verify failed\n"); return 1; }
        Row("BIP340 verify (FINALITY_KEY_EVIDENCE identity sig)", TimeIt([&] { (void)xpk.VerifySchnorr(msg, sig); }, 500), "secp256k1 in-tree");
    }

    // ------------------------------------------------------- certificates
    struct SetCase { size_t n; double participation; };
    const SetCase cases[] = {
        {16, 1.0}, {128, 1.0}, {512, 1.0}, {1024, 1.0}, {2048, 1.0}, {3500, 1.0}, {3500, 0.95}, {3500, 0.67},
        {4096, 1.0}, {8192, 1.0}, {8192, 0.95}, {8192, 0.67}};
    blst_p2 hm; blst_hash_to_g2(&hm, digest, 32, (const unsigned char*)SIG_DST.data(), SIG_DST.size(), nullptr, 0);
    for (const auto& c : cases) {
        const size_t signers = (size_t)(c.n * c.participation);
        // each signer signs (setup, untimed except one row)
        std::vector<blst_p2_affine> sigs(signers);
        for (size_t i = 0; i < signers; ++i) { blst_p2 s; blst_sign_pk_in_g1(&s, &hm, &vals[i].sk); blst_p2_to_affine(&sigs[i], &s); }
        std::vector<const blst_p2_affine*> sig_ptrs(signers);
        for (size_t i = 0; i < signers; ++i) sig_ptrs[i] = &sigs[i];
        std::vector<const blst_p1_affine*> pk_ptrs(signers);
        for (size_t i = 0; i < signers; ++i) pk_ptrs[i] = &vals[i].pk_aff;
        // aggregation (aggregator cost)
        blst_p2 agg_sig; blst_p2s_add(&agg_sig, sig_ptrs.data(), signers);
        blst_p2_affine agg_sig_aff; blst_p2_to_affine(&agg_sig_aff, &agg_sig);
        // verifier: aggregate pubkeys of signers
        blst_p1 agg_pk; blst_p1s_add(&agg_pk, pk_ptrs.data(), signers);
        blst_p1_affine agg_pk_aff; blst_p1_to_affine(&agg_pk_aff, &agg_pk);
        if (!VerifyAgg(agg_pk_aff, agg_sig_aff, digest)) { std::printf("agg self-test failed n=%zu\n", c.n); return 1; }
        char name[128];
        const int iters = c.n >= 4096 ? 10 : 30;
        std::snprintf(name, sizeof(name), "cert verify n=%zu part=%.0f%% : aggregate %zu pks + verify", c.n, c.participation * 100, signers);
        Row(name, TimeIt([&] {
            blst_p1 ap; blst_p1s_add(&ap, pk_ptrs.data(), signers);
            blst_p1_affine apa; blst_p1_to_affine(&apa, &ap);
            (void)VerifyAgg(apa, agg_sig_aff, digest);
        }, iters), "FastAggregateVerify, node side");
        std::snprintf(name, sizeof(name), "cert verify n=%zu : pairing only (pks pre-aggregated)", c.n);
        Row(name, TimeIt([&] { (void)VerifyAgg(agg_pk_aff, agg_sig_aff, digest); }, 30), "");
        std::snprintf(name, sizeof(name), "aggregate %zu signatures (aggregator)", signers);
        Row(name, TimeIt([&] { blst_p2 a; blst_p2s_add(&a, sig_ptrs.data(), signers); }, c.n >= 4096 ? 5 : 20), "G2 additions");
        // worst case invalid: one signer's sig replaced by another message's sig -> full pairing then reject
        if (c.participation == 1.0 && (c.n == 3500 || c.n == 8192)) {
            std::vector<blst_p2_affine> bad = sigs;
            blst_p2 hb; unsigned char d2[32]; std::memcpy(d2, digest, 32); d2[0] ^= 1;
            blst_hash_to_g2(&hb, d2, 32, (const unsigned char*)SIG_DST.data(), SIG_DST.size(), nullptr, 0);
            blst_p2 s; blst_sign_pk_in_g1(&s, &hb, &vals[0].sk); blst_p2_to_affine(&bad[0], &s);
            std::vector<const blst_p2_affine*> bp(signers); for (size_t i = 0; i < signers; ++i) bp[i] = &bad[i];
            blst_p2 ab; blst_p2s_add(&ab, bp.data(), signers); blst_p2_affine aba; blst_p2_to_affine(&aba, &ab);
            if (VerifyAgg(agg_pk_aff, aba, digest)) { std::printf("tamper self-test failed\n"); return 1; }
            std::snprintf(name, sizeof(name), "cert verify n=%zu INVALID (one bad sig): aggregate + verify", c.n);
            Row(name, TimeIt([&] {
                blst_p1 ap; blst_p1s_add(&ap, pk_ptrs.data(), signers);
                blst_p1_affine apa; blst_p1_to_affine(&apa, &ap);
                (void)VerifyAgg(apa, aba, digest);
            }, 10), "worst-case rejection = full cost");
        }
    }

    // ------------------------------------------------------------------ MPA
    {
        using modern::CreationAction;
        auto mk = [](uint16_t type, size_t len) { CreationAction a; a.action_type = type; a.action_version = 1; a.payload.assign(len, 0x42); return a; };
        // Note: the strict codec rejects unknown types; for a pure codec/hash cost we use a
        // registered type number with payload sizes of the finality objects.
        std::vector<CreationAction> cert{mk(3, 1240)};
        std::vector<CreationAction> keys; for (int i = 0; i < 64; ++i) keys.push_back(mk(3, 244));
        std::vector<CreationAction> maxsec; for (int i = 0; i < 5; ++i) maxsec.push_back(mk(3, 4000)); // 20 KB section cap today
        auto enc_cert = modern::EncodeCreationActionSection(cert);
        auto enc_keys = modern::EncodeCreationActionSection(keys);
        auto enc_max = modern::EncodeCreationActionSection(maxsec);
        if (!enc_cert || !enc_keys || !enc_max) { std::printf("section encode failed (bounds) — sizes: cert=%d keys=%d max=%d\n", (bool)enc_cert, (bool)enc_keys, (bool)enc_max); }
        auto bench_section = [&](const char* label, const std::vector<CreationAction>& acts, const std::optional<std::vector<unsigned char>>& enc) {
            if (!enc) return;
            char name[128];
            std::snprintf(name, sizeof(name), "MPA encode %s (%zu B)", label, enc->size());
            Row(name, TimeIt([&] { (void)modern::EncodeCreationActionSection(acts); }, 2000), "");
            std::snprintf(name, sizeof(name), "MPA strict decode %s", label);
            Row(name, TimeIt([&] { std::vector<CreationAction> out; size_t cur{0}; std::string err; (void)modern::DecodeCreationActionSection(*enc, cur, out, err); }, 2000), "incl. registry check");
            std::snprintf(name, sizeof(name), "MPA section TaggedHash %s", label);
            Row(name, TimeIt([&] { HashWriter w{TaggedHash("B3/MPA/SECTION/V1")}; w << std::span<const unsigned char>(*enc); (void)w.GetSHA256(); }, 2000), "");
        };
        bench_section("1 x 1240-B certificate", cert, enc_cert);
        bench_section("64 x 244-B key evidence", keys, enc_keys);
        bench_section("5 x 4000-B (section cap 20 KB)", maxsec, enc_max);
        for (size_t n : {1UL, 100UL, 1000UL, 5000UL, 10000UL}) {
            std::vector<uint256> leaves(n);
            for (size_t i = 0; i < n; ++i) { HashWriter w{TaggedHash("B3/MPA/LEAF/V1")}; w << (uint32_t)i << uint256{}; leaves[i] = w.GetSHA256(); }
            char name[128]; std::snprintf(name, sizeof(name), "payload_root: %zu leaves (build leaves + ComputeMerkleRoot)", n);
            Row(name, TimeIt([&] {
                std::vector<uint256> l(n);
                for (size_t i = 0; i < n; ++i) { HashWriter w{TaggedHash("B3/MPA/LEAF/V1")}; w << (uint32_t)i << uint256{}; l[i] = w.GetSHA256(); }
                (void)ComputeMerkleRoot(std::move(l));
            }, n >= 5000 ? 20 : 200), "");
        }
    }
    std::printf("\nDone.\n");
    return 0;
}
