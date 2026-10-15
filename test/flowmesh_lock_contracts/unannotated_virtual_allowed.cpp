#include "lock_contract_fixture.h"

StdMutex chain;

struct Interface {
    virtual void Touch() = 0;
};

struct Implementation final : Interface {
    void Touch() override LOCKS_EXCLUDED(chain) {}
};

void CallThroughBase(Interface& dependency)
{
    StdLockGuard chain_guard(chain);
    dependency.Touch(); // Implementation-only contract is invisible here.
}
