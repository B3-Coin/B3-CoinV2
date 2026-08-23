# blst (vendored, pinned) — B3 benchmark-only use

Upstream: https://github.com/supranational/blst — **tag v0.3.17, commit
54e6e55674722fc2797ebb4bbb71b26d881eb4b8**, Apache-2.0 (see LICENSE).

Vendored subset: `bindings/blst.h`, `bindings/blst_aux.h`, `src/*.c|h`
(`src/server.c` is the single compilation unit), `build/assembly.S` and the
pre-generated per-platform assembly under `build/{mach-o,elf,coff,win64}`.
Nothing else from upstream is included (no language bindings, no build scripts).

Status (owner ruling 2026-08-23): **benchmark-only**. Built only when
`-DB3_FINALITY_BENCH=ON` (default OFF); linked into no node, wallet, test or
consensus target. Built with `__BLST_PORTABLE__` so CPU-feature dispatch never
changes results. Not an authorization to use BLS in consensus.
