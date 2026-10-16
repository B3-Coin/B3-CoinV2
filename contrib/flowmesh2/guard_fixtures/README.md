# Guard fixtures (`contrib/flowmesh2/guard_fixtures/`)

Input data for `contrib/flowmesh2/test_check_guards.py` (CTest
`flowmesh2_guards_selftest`). Nothing here is compiled by CMake or scanned by
`check_guards.py` on the real tree: every fixture file ends in `.fixture`, and
the harness strips that suffix when it copies a fixture into its work
directory under `build-rebirth/fm2-test-tmp/`.

- `clean/` is a minimal fake repository that must pass every rule. The harness
  completes it with copies of the real C1 sources, the real
  `src/flowmesh2/CMakeLists.txt` and the real `core/result.h`, `core/codes.h`
  and `core/codes.cpp`, so the C1 pins and the stub-declaration and registry
  exemptions are exercised on the real bytes.
- `planted/<scenario>/` overlays one violation on top of `clean/`. The
  scenario name starts with `r<N>_`, the one rule that must fail.
- `nm/nm_fixture.cpp.fixture` holds the symbols for rule 3 and rule 4 (build
  mode). The harness compiles it once per `FM2_NM_<CASE>` macro with the CMake
  C++ compiler, archives the objects with `CMAKE_AR` into a fake build tree
  and writes a matching `fm2_libs.manifest`.

The harness also generates one scenario per entry of its own copy of the rule
1 and rule 2 lists, so removing an entry from the guard makes the self-test
fail. Owner-protected like the guard itself (TODO.md rule 5).
