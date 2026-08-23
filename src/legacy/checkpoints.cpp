// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// The historical mainnet checkpoint table lives in its own translation unit
// inside bitcoin_common: CMainParams (kernel/chainparams.cpp, bitcoin_common)
// installs it, and the tools that link only bitcoin_common (b3coin-tx,
// b3coin-util, b3coin-wallet) must not pull the whole legacy consensus
// engine (bitcoin_node) in for one table.

#include <legacy/consensus.h>

#include <uint256.h>

#include <map>

namespace legacy {

const std::map<int, uint256>& MainnetCheckpoints()
{
    // Verbatim from the historical client's checkpoints.cpp (mapCheckpoints).
    // Height 0 is the genesis hash. Do not add, remove or alter these.
    static const std::map<int, uint256> checkpoints{
        {0,     uint256{"4b0d7f133c5267d715d4d8992635a5490d1edd6b7072cce3f8fe116aba983b6a"}},
        {77900, uint256{"907be67958dcd6d9d06c2c896f3b65aad687867ff342db2c7cb0ff5d717c5255"}},
        {77988, uint256{"d87412238aad0d2e53fd6ba6fcc3e429257ee0fc93872ca4716c976caa40a14d"}},
        {78000, uint256{"f535a817b179afce21ead0e5d30940bb5cc76dfa89bb524c57162a45fdd0916c"}},
        {78690, uint256{"05a0a3e7abba6d37a529897e199a9c295357e4645d7c080c0580099e8a6b3c98"}},
        {78691, uint256{"89c9542eecf041961fb4cb083c2652dbb03150be080334c3977bd1a171ace2b5"}},
        {78961, uint256{"dc8da75ee5b362cc8ee55e466c1cd58faf7849f02b28bd2cc68fe9891758c41e"}},
        {81686, uint256{"9c97add390392589888f686c4296062f140ad1d601c92f61ca3607684f9dc8da"}},
        {81786, uint256{"c003a4193f8c83729676eaea3bb8a7fb97d8b208309970e919155ac0c722eef8"}},
        {90000, uint256{"b7a69eb99067eb23cbe1f22f2240ddd45930ab02fda291e6c718d21fc7c58226"}},
        {95000, uint256{"52ec943ff2ae9552308e2f7a3ef5aaddb8d6b7e1d45ada2e607ff60bf6380fb9"}},
        {95150, uint256{"9b9dba004fa65750105b505569db97b635095a9dd0e620729f7fe4103f5576bd"}},
        {95350, uint256{"095f1cb3cf1f1300ad99f891c2c0bb13cc374d9215781ad988e82cc0086a8e45"}},
    };
    return checkpoints;
}

} // namespace legacy
