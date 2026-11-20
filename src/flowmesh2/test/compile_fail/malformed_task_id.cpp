// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Compile-fail control (CTest flowmesh2_negative_compile, Clang only).
// A TaskId literal that is not in the TODO.md ledger format must be a build
// error also where the TaskId is made by implicit conversion inside the
// dynamic initialiser of a namespace-scope object, which is how
// test/stub_inventory_tests.cpp registers every stub. The guard is the
// consteval constructor calling a function that is not constexpr; that
// function is defined (Clang 18 emits the constructor's body there and
// wants the symbol at link time), and this control shows the definition did
// not weaken the check. Built as is it must fail with the diagnostic below;
// built with FM2_COMPILE_FAIL_FIXED (the literal is a ledger ID) it must
// compile.
//
// FM2_EXPECT_ERROR: malformed_task_id\.cpp:\d+:\d+: error: call to consteval function .flowmesh2::TaskId::TaskId. is not a constant expression

#include <flowmesh2/core/result.h>

#include <string_view>

namespace {

struct Registrar {
    explicit Registrar(flowmesh2::TaskId task) : id{task.View()} {}
    std::string_view id;
};

#ifdef FM2_COMPILE_FAIL_FIXED
const Registrar REGISTRAR{"R1"};
#else
const Registrar REGISTRAR{"R01"};
#endif

} // namespace

std::string_view Fm2CompileFailMalformedTaskId() { return REGISTRAR.id; }
