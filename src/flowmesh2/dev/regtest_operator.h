// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_DEV_REGTEST_OPERATOR_H
#define B3_FLOWMESH2_DEV_REGTEST_OPERATOR_H

#ifndef FM2_DEV_COMPONENTS
#error "the operator harness is REGTEST only"
#endif

#include <flowmesh2/core/result.h>
#include <univalue.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace flowmesh2::dev {

class OperatorFailure : public std::runtime_error
{
public:
    OperatorFailure(ErrorCode code_in, const std::string& detail, bool fatal_in = false)
        : std::runtime_error{detail}, code{code_in}, fatal{fatal_in} {}
    ErrorCode code;
    bool fatal;
};

//! Pure, local test-key helpers; the wrapper owns authorization and key files.
UniValue RegtestKeyInfo(const UniValue& request);
UniValue RegtestClientAction(const UniValue& request);
UniValue RegtestNetworkKeyInfo(const UniValue& request);
UniValue RegtestEvmSign(const UniValue& request);
UniValue RegtestEvmKeygen();
UniValue RegtestEvmKeyInfo(const UniValue& request);
UniValue RegtestNitroLabProfile(const UniValue& request);

/** One real static-roster regtest operator. Explicit modes exercise direct
 * quorum commit, fixed-view PREPARE/COMMIT, or durable view change under its
 * separate profile and control journal. Authenticated production network
 * transport remains outside this local harness. The private
 * observer command must be fed only by the wrapper's independent B3/EVM
 * checks. No RPC, synthetic credit generator, FakeChainAdapter, memory
 * journal or single-seat certifier lives here.
 */
class RegtestOperator
{
public:
    static std::unique_ptr<RegtestOperator> Open(const UniValue& config);
    ~RegtestOperator();
    UniValue Handle(const UniValue& request);
    RegtestOperator(const RegtestOperator&) = delete;
    RegtestOperator& operator=(const RegtestOperator&) = delete;
private:
    struct Impl;
    explicit RegtestOperator(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace flowmesh2::dev
#endif
