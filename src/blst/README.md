# blst (vendored, pinned) — B3 benchmark-only use

Upstream: https://github.com/supranational/blst — **tag v0.3.17, commit
54e6e55674722fc2797ebb4bbb71b26d881eb4b8**, Apache-2.0 (see LICENSE).

Vendored subset: `bindings/blst.h`, `bindings/blst_aux.h`, `src/*.c|h`
(`src/server.c` is the single compilation unit), `build/assembly.S` and the
pre-generated per-platform assembly under `build/{mach-o,elf,coff,win64}`.
Nothing else from upstream is included (no language bindings, no build scripts).

Status: consensus dependency since plan Commit 2 (owner-approved
2026-08-23), reached ONLY through the narrow wrapper `src/crypto/bls.{h,cpp}`
(the scheme is frozen there, not inherited from library defaults). No other
target may include `blst.h`. Built with `__BLST_PORTABLE__` so CPU-feature
dispatch never changes results.
