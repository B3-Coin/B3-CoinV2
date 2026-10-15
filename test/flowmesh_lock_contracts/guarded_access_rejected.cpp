#include "lock_contract_fixture.h"

struct State {
    StdMutex mutex;
    int value GUARDED_BY(mutex){0};

    int ReadUnlocked()
    {
        return value;
    }
};
