// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/dev/regtest_operator.h>
#include <key.h>
#include <util/translation.h>

#include <iostream>
#include <memory>
#include <string>

const TranslateFn G_TRANSLATION_FUN{nullptr};

int main(int argc, char**)
{
    using namespace flowmesh2;
    using namespace flowmesh2::dev;
    if (argc != 1) {
        std::cerr << "fm2-regtest-node takes bounded JSONL on stdin; secrets never belong in argv\n";
        return 2;
    }
    ECC_Context ecc;
    std::unique_ptr<RegtestOperator> worker;
    constexpr size_t MAX_LINE{32 * 1024 * 1024};
    for (;;) {
        std::string line;
        line.reserve(4096);
        char c;
        bool got{false};
        while (std::cin.get(c)) {
            got = true;
            if (c == '\n') break;
            if (line.size() >= MAX_LINE) {
                std::cerr << "JSONL request exceeds 32 MiB\n";
                return 2;
            }
            line.push_back(c);
        }
        if (!got) return 0;
        UniValue request, reply{UniValue::VOBJ};
        bool fatal{false};
        try {
            if (!request.read(line) || !request.isObject() || !request["cmd"].isStr()) {
                throw OperatorFailure{ErrorCode::INVALID_INPUT, "expected one JSON object with cmd"};
            }
            reply.pushKV("id", request["id"]);
            const std::string cmd{request["cmd"].get_str()};
            UniValue result;
            if (cmd == "key_info") result = RegtestKeyInfo(request);
            else if (cmd == "network_key_info") result = RegtestNetworkKeyInfo(request);
            else if (cmd == "client_action") result = RegtestClientAction(request);
            else if (cmd == "evm_sign") result = RegtestEvmSign(request);
            else if (cmd == "evm_keygen") result = RegtestEvmKeygen();
            else if (cmd == "evm_key_info") result = RegtestEvmKeyInfo(request);
            else if (cmd == "nitro_lab_profile") result = RegtestNitroLabProfile(request);
            else if (cmd == "open") {
                if (worker) throw OperatorFailure{ErrorCode::ORDER_VIOLATION, "operator already opened"};
                worker = RegtestOperator::Open(request["config"]);
                UniValue info{UniValue::VOBJ};
                info.pushKV("cmd", "info");
                result = worker->Handle(info);
            } else {
                if (!worker) throw OperatorFailure{ErrorCode::ORDER_VIOLATION, "open an operator first"};
                result = worker->Handle(request);
            }
            reply.pushKV("ok", true);
            reply.pushKV("result", std::move(result));
        } catch (const OperatorFailure& failure) {
            UniValue error{UniValue::VOBJ};
            error.pushKV("code", std::string{ToString(failure.code)});
            error.pushKV("detail", std::string{failure.what()});
            error.pushKV("fatal", failure.fatal);
            reply.pushKV("ok", false);
            reply.pushKV("error", std::move(error));
            fatal = failure.fatal;
        } catch (const std::exception&) {
            // Unexpected failures are never swallowed after a possible write.
            // Do not echo request/config text, which may contain a test secret.
            UniValue error{UniValue::VOBJ};
            error.pushKV("code", "INVARIANT");
            error.pushKV("detail", "unexpected operator failure; reopen the original store");
            error.pushKV("fatal", true);
            reply.pushKV("ok", false);
            reply.pushKV("error", std::move(error));
            fatal = true;
        }
        std::cout << reply.write() << '\n' << std::flush;
        if (!std::cout.good()) return 1;
        if (fatal) return 1;
    }
}
