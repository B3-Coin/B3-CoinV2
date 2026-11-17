// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/bls.h>
#include <crypto/sha256.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/netio/api_json.h>
#include <flowmesh2/netio/genesis_doc.h>
#include <flowmesh2/netio/mesh_handshake.h>
#include <key.h>
#include <pubkey.h>
#include <random.h>
#include <util/fs.h>
#include <util/translation.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <charconv>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** fm2-keygen: the regtest cluster generator (stage 2 step 5, M5-NODE-01).
 * DEV ONLY (fm2_dev): REGTEST only, TEST assets, and the only place outside
 * the seat signer that signs with a BLS seat key (the endpoint records of
 * genesis.json; guard rule 7 allows dev/).
 *
 *   fm2-keygen --out <dir> [--seats 4] [--address 127.0.0.1]
 *              [--mesh-port-base 15649] [--api-port-base 18800]
 *
 * Writes <dir>/genesis.json (public; netio/genesis_doc.h) and, per seat,
 * <dir>/seat<index>/seat.key (the BLS IKM, 32 bytes hex) and
 * <dir>/seat<index>/transport.key (the secp256k1 transport secret, 32 bytes hex),
 * both 0600. Real key custody is not part of this tool.
 */
// Core's clientversion.cpp (bitcoin_common) refers to this; every executable
// defines it, as bitcoind and bitcoin-cli do. Nothing here is translated.
const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {

using namespace flowmesh2;

int Usage(std::string_view problem)
{
    std::cerr << "fm2-keygen: " << problem << "\n"
              << "usage: fm2-keygen --out <dir> [--seats <k>] [--address <ip>] [--mesh-port-base <port>] [--api-port-base <port>]\n";
    return 2;
}

std::optional<uint64_t> ParseU64(std::string_view text)
{
    uint64_t value{0};
    const auto [end, error]{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
    return value;
}

uint256 Label(const std::string& label)
{
    uint256 out;
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(label.data()), label.size()).Finalize(out.data());
    return out;
}

bool WriteSecret(const fs::path& path, const std::string& text)
{
    const int fd{::open(fs::PathToString(path).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600)};
    if (fd < 0) return false;
    const std::string line{text + "\n"};
    const bool ok{::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size())};
    ::close(fd);
    return ok;
}

bool WritePublic(const fs::path& path, const std::string& text)
{
    const int fd{::open(fs::PathToString(path).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)};
    if (fd < 0) return false;
    const bool ok{::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size())};
    ::close(fd);
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    std::string out_dir;
    uint64_t seats{4};
    std::string address{"127.0.0.1"};
    uint64_t mesh_port_base{15'649};
    uint64_t api_port_base{18'800};
    for (int i{1}; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") return Usage("help");
        if (i + 1 >= argc) return Usage("missing value for " + std::string{arg});
        const std::string value{argv[++i]};
        if (arg == "--out") {
            out_dir = value;
        } else if (arg == "--seats") {
            const std::optional<uint64_t> k{ParseU64(value)};
            if (!k || *k == 0 || *k > 64) return Usage("seats must be 1..64");
            seats = *k;
        } else if (arg == "--address") {
            address = value;
        } else if (arg == "--mesh-port-base") {
            const std::optional<uint64_t> p{ParseU64(value)};
            if (!p || *p == 0 || *p + seats > 65'535) return Usage("bad mesh port base");
            mesh_port_base = *p;
        } else if (arg == "--api-port-base") {
            const std::optional<uint64_t> p{ParseU64(value)};
            if (!p || *p == 0 || *p + seats > 65'535) return Usage("bad api port base");
            api_port_base = *p;
        } else {
            return Usage("unknown option " + std::string{arg});
        }
    }
    if (out_dir.empty()) return Usage("--out is required");
    const ECC_Context ecc;
    const fs::path out{fs::PathFromString(out_dir)};
    std::error_code ec;
    fs::create_directories(out, ec);

    netio::GenesisDoc doc;
    doc.network.chain = ChainType::REGTEST;
    doc.network.magic = {0xf2, 0x51, 0x4d, 0x10};
    doc.network.modern_chain_domain = Label("fm2/regtest/domain");
    doc.mesh_magic = {0xf2, 0x51, 0x4d, 0x11};
    doc.base = Anchor{129, Label("fm2/devchain/block/129")};
    doc.facts_start = FactCursor{130, 0};
    doc.assets.emplace("B3", AssetId::Native());
    for (const char* name : {"rUSD", "TOK"}) {
        const std::string text{name};
        doc.assets.emplace(text, AssetId{Tagged(HashTag::TEST_ASSET, std::span{reinterpret_cast<const unsigned char*>(text.data()), text.size()})});
    }
    doc.markets.push_back(netio::GenesisMarket{"B3", "rUSD", 1'000'000, 1});
    doc.markets.push_back(netio::GenesisMarket{"TOK", "rUSD", 1, 10});
    const uint256 domain{doc.Domain()};

    struct SeatSecrets {
        std::string ikm_hex;
        std::string transport_hex;
    };
    std::vector<SeatSecrets> secrets;
    std::vector<bls::SecretKey> bls_keys;
    for (uint64_t i{0}; i < seats; ++i) {
        std::array<unsigned char, 32> ikm{};
        GetStrongRandBytes(ikm);
        const std::optional<bls::SecretKey> key{bls::SecretKey::FromIKM(ikm)};
        if (!key.has_value()) {
            std::cerr << "fm2-keygen: the IKM gave no BLS key\n";
            return 1;
        }
        CKey transport;
        transport.MakeNewKey(true);
        netio::GenesisSeat seat;
        seat.index = static_cast<uint32_t>(i);
        seat.bls_pubkey = key->GetPublicKey().Compressed();
        seat.pop = key->SignPoP().Compressed();
        seat.mesh_address = address;
        seat.mesh_port = static_cast<uint16_t>(mesh_port_base + i);
        seat.api_address = address;
        seat.api_port = static_cast<uint16_t>(api_port_base + i);
        seat.endpoint.seat = seat.index;
        seat.endpoint.addresses = {address};
        seat.endpoint.port = seat.mesh_port;
        seat.endpoint.counter = 1;
        const XOnlyPubKey xonly{transport.GetPubKey()};
        std::copy(xonly.begin(), xonly.end(), seat.endpoint.transport_key.begin());
        const uint256 digest{netio::EndpointDigest(seat.endpoint, domain)};
        seat.endpoint.signature = key->Sign(std::span<const unsigned char>{digest.data(), digest.size()}).Compressed();
        doc.seats.push_back(seat);
        std::array<unsigned char, 32> transport_bytes{};
        std::copy(transport.begin(), transport.end(), reinterpret_cast<std::byte*>(transport_bytes.data()));
        secrets.push_back(SeatSecrets{netio::ToHex(ikm), netio::ToHex(transport_bytes)});
        bls_keys.push_back(*key);
    }
    // The document must verify as a reader would see it.
    const Result<SeatSet> seat_set{doc.SeatSetOf()};
    if (!seat_set.has_value()) {
        std::cerr << "fm2-keygen: the seat set does not verify: " << seat_set.error().detail << "\n";
        return 1;
    }
    for (const netio::GenesisSeat& seat : doc.seats) {
        if (const Result<void> ok{netio::VerifyEndpointRecord(seat.endpoint, *seat_set, domain)}; !ok.has_value()) {
            std::cerr << "fm2-keygen: an endpoint record does not verify: " << ok.error().detail << "\n";
            return 1;
        }
    }
    if (!WritePublic(out / fs::u8path("genesis.json"), doc.Json())) {
        std::cerr << "fm2-keygen: could not write genesis.json\n";
        return 1;
    }
    for (uint64_t i{0}; i < seats; ++i) {
        const fs::path seat_dir{out / fs::u8path("seat" + std::to_string(i))};
        fs::create_directories(seat_dir, ec);
        ::chmod(fs::PathToString(seat_dir).c_str(), 0700);
        if (!WriteSecret(seat_dir / fs::u8path("seat.key"), secrets[i].ikm_hex) ||
            !WriteSecret(seat_dir / fs::u8path("transport.key"), secrets[i].transport_hex)) {
            std::cerr << "fm2-keygen: could not write the keys of seat " << i << "\n";
            return 1;
        }
    }
    std::cout << "fm2-keygen: wrote " << fs::PathToString(out / fs::u8path("genesis.json")) << " and " << seats << " seat key directories\n";
    return 0;
}
