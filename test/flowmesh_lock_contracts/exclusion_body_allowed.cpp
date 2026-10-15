#include "lock_contract_fixture.h"

StdMutex market;
StdMutex chain;

// EXCLUDED is an entry condition. Acquiring chain in the body is permitted.
void ChainOperation() LOCKS_EXCLUDED(chain)
{
    StdLockGuard chain_guard(chain);
}

void MarketOperation()
{
    StdLockGuard market_guard(market);
    ChainOperation(); // Compiles: no prohibition on market -> chain was declared.
}
