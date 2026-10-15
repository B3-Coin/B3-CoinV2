#include "lock_contract_fixture.h"

StdMutex chain;

struct Interface {
    virtual void Touch() LOCKS_EXCLUDED(chain) = 0;
};

struct Implementation final : Interface {
    void Touch() override LOCKS_EXCLUDED(chain) {}
};

void CallThroughBase(Interface& dependency)
{
    StdLockGuard chain_guard(chain);
    dependency.Touch(); // Visible base-interface contract rejects this call.
}
