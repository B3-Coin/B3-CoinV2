#include "lock_contract_fixture.h"

struct State {
    StdMutex mutex;
    int value GUARDED_BY(mutex){0};

    int ReadLocked() EXCLUSIVE_LOCKS_REQUIRED(mutex) { return value; }

    int Read()
    {
        StdLockGuard guard(mutex);
        return ReadLocked();
    }
};
