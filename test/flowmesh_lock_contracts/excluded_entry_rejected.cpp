#include "lock_contract_fixture.h"

StdMutex chain;

void ChainOperation() LOCKS_EXCLUDED(chain) {}

void InvalidEntry()
{
    StdLockGuard chain_guard(chain);
    ChainOperation();
}
