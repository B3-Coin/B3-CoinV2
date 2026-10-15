#include "lock_contract_fixture.h"

StdMutex market;
StdMutex chain;

// The caller's relevant capability is excluded at the blocking boundary.
void BlockingChain() LOCKS_EXCLUDED(market)
{
    StdLockGuard chain_guard(chain);
}

void InvalidBoundaryCall()
{
    StdLockGuard market_guard(market);
    BlockingChain();
}
