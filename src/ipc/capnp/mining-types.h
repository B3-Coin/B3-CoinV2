// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IPC_CAPNP_MINING_TYPES_H
#define BITCOIN_IPC_CAPNP_MINING_TYPES_H

#include <consensus/block_codec.h>
#include <interfaces/mining.h>
#include <ipc/capnp/common.capnp.proxy-types.h>
#include <ipc/capnp/common-types.h>
#include <ipc/capnp/mining.capnp.proxy.h>
#include <node/miner.h>
#include <node/types.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <validation.h>

#include <cassert>
#include <cstring>

namespace mp {
/**
 * Mining IPC must carry the exact modern transaction form. The generic IPC
 * codec intentionally uses TX_WITH_WITNESS, which omits B3's MPA flag and
 * section; using it for submitSolution would silently strip a finality-bearing
 * coinbase and make its mandatory payload-root output invalid.
 */
template <typename Value, typename Output>
void CustomBuildField(TypeList<CTransactionRef>, Priority<2>,
                      InvokeContext& invoke_context, Value&& value,
                      Output&& output)
{
    assert(value);
    DataStream stream;
    stream << TX_MODERN(*value);
    auto result{output.init(stream.size())};
    std::memcpy(result.begin(), stream.data(), stream.size());
}

template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<CTransactionRef>, Priority<2>,
                               InvokeContext& invoke_context, Input&& input,
                               ReadDest&& read_dest)
{
    assert(input.has());
    return read_dest.update([&](CTransactionRef& value) {
        const auto data{input.get()};
        SpanReader stream{{data.begin(), data.end()}};
        auto wrapper{ParamsStream{stream, TX_MODERN}};
        wrapper >> value;
    });
}

/**
 * A marker-modern B3 block uses the modern transaction body (including MPA)
 * and a trailing block-signature vector. Select that codec for Mining IPC's
 * getBlock/checkBlock Data fields; ordinary Bitcoin templates retain their
 * existing witness encoding byte-for-byte.
 */
template <typename Value, typename Output>
void CustomBuildField(TypeList<CBlock>, Priority<2>,
                      InvokeContext& invoke_context, Value&& value,
                      Output&& output)
{
    DataStream stream;
    if (Consensus::HasB3BlockCodecV2(value.nVersion)) {
        stream << TX_LEGACY_B3(value);
    } else {
        stream << TX_WITH_WITNESS(value);
    }
    auto result{output.init(stream.size())};
    std::memcpy(result.begin(), stream.data(), stream.size());
}

template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<CBlock>, Priority<2>,
                               InvokeContext& invoke_context, Input&& input,
                               ReadDest&& read_dest)
{
    assert(input.has());
    const auto data{input.get()};
    SpanReader prefix{{data.begin(), data.end()}};
    int32_t version{0};
    prefix >> version;
    return read_dest.update([&](CBlock& value) {
        SpanReader stream{{data.begin(), data.end()}};
        if (Consensus::HasB3BlockCodecV2(version)) {
            auto wrapper{ParamsStream{stream, TX_LEGACY_B3}};
            wrapper >> value;
        } else {
            auto wrapper{ParamsStream{stream, TX_WITH_WITNESS}};
            wrapper >> value;
        }
    });
}
} // namespace mp

#endif // BITCOIN_IPC_CAPNP_MINING_TYPES_H
