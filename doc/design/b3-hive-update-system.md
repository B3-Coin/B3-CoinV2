# B3 Hive secure update system — design record

Owner specification received 2026-08-26 (item 8, verbatim ruling; this
document records the mapping into the tree). Scope isolation is absolute:
nothing under `src/update/` or the Qt glue may include consensus, wallet
storage, FlowMesh execution, activation, or P2P code.

## Trust model implementation

- **Byte-canonical manifest**: the signed payload IS the raw bytes of the
  manifest file from the magic line `B3-HIVE-MANIFEST-V1` up to (and
  including) the newline before `-----SIGNATURES-----`. Signatures are
  BIP340 Schnorr over `TaggedHash("B3/HIVE/UPDATE/V1", payload_bytes)`,
  verified with the repository's audited secp256k1 (`XOnlyPubKey`).
  No JSON, no canonicalization step, no ambiguity: what was signed is
  exactly what is parsed, and the strict parser rejects anything else.
- **Threshold + rotation**: production pins N x-only release keys and a
  threshold K in the binary (update-specific; disjoint from wallet,
  validator, bridge and consensus keys). Each signature line names a key
  id (first 8 hex of the key); unknown ids are ignored and never counted;
  duplicate ids counted once; fewer than K valid signatures = reject.
  Rotation ships new pinned sets in new releases (updates update the
  updater), with overlap periods where both sets sign.
- **Fail closed**: a build with no configured manifest URL or an empty
  pinned key set performs no network activity and shows nothing.

## Manifest format (strict, versioned)

```
B3-HIVE-MANIFEST-V1
channel=stable
sequence=<u64, strictly increasing per channel>
published=<unix u64>
expires=<unix u64>
notes_sha256=<64 hex>
artifact.os=<macos|windows|linux>
artifact.arch=<arm64|x86|x86_64>
artifact.format=<dmg|pkg|exe|appimage|targz>
artifact.version=<semver, e.g. 31.1.1>
artifact.size=<u64 bytes>
artifact.sha256=<64 hex>
artifact.url=https://<approved host>/...
[artifact.* blocks repeat, each starting with artifact.os]
-----SIGNATURES-----
sig=<keyid 8 hex>:<128 hex BIP340 signature>
```

LF-only, UTF-8, exact field order inside each block, no blank lines in the
payload, no unknown keys, no duplicates (except repeated artifact blocks),
hard size bound on the whole file. Any deviation = reject.

## Verifier rejection matrix (all consensus-grade strict)

signature threshold unmet / invalid sig / unknown magic or version line /
malformed or out-of-order fields / expired (`now >= expires`) / future
(`published > now + skew`) / channel mismatch / sequence <= last accepted
(rollback & replay) / artifact version <= installed / no artifact matching
host os+arch+format / non-HTTPS URL / URL host not in the approved list /
size or digest field malformed / manifest over size bound.

## Components (dependency-injected, testable offline)

`src/update/`: `manifest.{h,cpp}` (codec+verifier) -> `checker.h` ->
`downloader.h` (transport interface; stream-hash, size enforcement, atomic
temp files) -> `installer.h` (platform interface; external helper process,
argv-only exec, previous install preserved) -> `manager.{h,cpp}` (state
machine: Idle -> Checking -> UpdateAvailable -> Downloading -> Verified ->
AwaitingShutdown -> Installing; every transition requiring approval is
explicit). Qt glue under `src/qt/` renders the state machine only.

`contrib/b3hive-release/`: deterministic offline manifest generator/signer.
Test keys are generated per-run and marked; no private key ever enters the
tree, build, installer or server. Production URL + key set are release
inputs supplied by the owner — never invented, never defaulted.

## Delivery order (small buildable commits)

1. This document.
2. Manifest codec + verifier + full rejection-matrix tests.
3. Checker/downloader policies + tests (hostile redirect, oversize,
   truncation, digest mismatch, disk-full, symlink/path traversal).
4. Manager state machine + tests (incl. fail-closed-unconfigured,
   installer-gated-on-shutdown, indicator state transitions).
5. Installer platform implementations + external updater helper.
6. Qt settings panel + status indicator + dialog.
7. Release signing tool + operator documentation.
