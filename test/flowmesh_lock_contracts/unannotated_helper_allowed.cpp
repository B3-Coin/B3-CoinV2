#include "lock_contract_fixture.h"

StdMutex market;
StdMutex chain;

void UnannotatedHelper()
{
    StdLockGuard chain_guard(chain);
}

void MarketOperation() LOCKS_EXCLUDED(chain)
{
    StdLockGuard market_guard(market);
    UnannotatedHelper(); // No interprocedural inference of the helper's lock.
}
