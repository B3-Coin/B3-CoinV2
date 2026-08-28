#!/bin/zsh
# Rebuild and package an unsigned B3 Hive release candidate.
# Usage: make_v1_package.sh [outdir] [builddir]
# Defaults: dist/ and build/ under the source tree.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd -P)
OUT=${1:-$REPO/dist}
BUILD_DIR=${2:-$REPO/build}
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  print -u2 -- "error: not a configured CMake build directory: $BUILD_DIR"
  exit 1
fi
BUILD_DIR=$(cd "$BUILD_DIR" && pwd -P)
BIN="$BUILD_DIR/bin"

REQUIRED_BINARIES=(
  b3coind
  b3coin-cli
  b3coin-tx
  b3coin-util
  b3coin-wallet
  b3coin-qt
)
REQUIRED_FILES=(
  "$REPO/README.md"
  "$REPO/COPYING"
  "$REPO/doc/design/b3-product-identity.md"
  "$REPO/contrib/b3hive-demo/start_network.sh"
  "$REPO/contrib/b3hive-demo/stop_network.sh"
)

# A release archive must not silently pick up same-version binaries left by an
# earlier source tree. Rebuild every packaged target in the selected directory.
cmake --build "$BUILD_DIR" --target "${REQUIRED_BINARIES[@]}"

for binary in "${REQUIRED_BINARIES[@]}"; do
  if [[ ! -x "$BIN/$binary" ]]; then
    print -u2 -- "error: required release binary is missing or not executable: $BIN/$binary"
    exit 1
  fi
done
for file in "${REQUIRED_FILES[@]}"; do
  if [[ ! -f "$file" ]]; then
    print -u2 -- "error: required package file is missing: $file"
    exit 1
  fi
done

cmake_version_part() {
  local key=$1
  sed -nE "s/^set\\(${key}[[:space:]]+([0-9]+)\\).*$/\\1/p" "$REPO/CMakeLists.txt"
}

SOURCE_MAJOR=$(cmake_version_part CLIENT_VERSION_MAJOR)
SOURCE_MINOR=$(cmake_version_part CLIENT_VERSION_MINOR)
SOURCE_BUILD=$(cmake_version_part CLIENT_VERSION_BUILD)
SOURCE_RC=$(cmake_version_part CLIENT_VERSION_RC)
for part in "$SOURCE_MAJOR" "$SOURCE_MINOR" "$SOURCE_BUILD" "$SOURCE_RC"; do
  if [[ ! "$part" =~ '^[0-9]+$' ]]; then
    print -u2 -- "error: could not derive one numeric version value from CMakeLists.txt"
    exit 1
  fi
done
SOURCE_VERSION="$SOURCE_MAJOR.$SOURCE_MINOR.$SOURCE_BUILD"
if (( SOURCE_RC > 0 )); then
  SOURCE_VERSION="${SOURCE_VERSION}rc${SOURCE_RC}"
fi

VERSION_LINE=$("$BIN/b3coind" -nosettings -version 2>/dev/null | sed -n '1p')
BINARY_VERSION=$(print -r -- "$VERSION_LINE" | sed -nE 's/^.* version v([0-9]+\.[0-9]+\.[0-9]+(rc[0-9]+)?)([[:space:]].*)?$/\1/p')
if [[ ! "$BINARY_VERSION" =~ '^[0-9]+\.[0-9]+\.[0-9]+(rc[0-9]+)?$' ]]; then
  print -u2 -- "error: could not derive a valid version from b3coind: ${VERSION_LINE:-<no output>}"
  exit 1
fi
if [[ "$BINARY_VERSION" != "$SOURCE_VERSION" ]]; then
  print -u2 -- "error: stale or mismatched build (source $SOURCE_VERSION, b3coind $BINARY_VERSION)"
  exit 1
fi

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd -P)
ARCH="$(uname -m)-$(uname -s | tr '[:upper:]' '[:lower:]')"
NAME="b3-hive-v${BINARY_VERSION}-unsigned-rc-${ARCH}"
ARCHIVE="$OUT/$NAME.tar.gz"
CHECKSUM="$ARCHIVE.sha256"
if [[ -e "$ARCHIVE" || -e "$CHECKSUM" ]]; then
  print -u2 -- "error: refusing to overwrite an existing release candidate: $ARCHIVE"
  exit 1
fi

WORK_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/b3-hive-package.XXXXXX")
trap 'rm -rf -- "$WORK_ROOT"' EXIT HUP INT TERM
STAGE="$WORK_ROOT/$NAME"
mkdir -p "$STAGE/bin" "$STAGE/demo" "$STAGE/doc"

for binary in "${REQUIRED_BINARIES[@]}"; do
  cp -p "$BIN/$binary" "$STAGE/bin/"
done
cp -p "$REPO/contrib/b3hive-demo/start_network.sh" "$STAGE/demo/"
cp -p "$REPO/contrib/b3hive-demo/stop_network.sh" "$STAGE/demo/"
cp -p "$REPO/README.md" "$STAGE/README.md"
cp -p "$REPO/COPYING" "$STAGE/COPYING"
cp -p "$REPO/doc/design/b3-product-identity.md" "$STAGE/doc/"

# Demo scripts inside the archive resolve binaries relative to the archive.
for script in "$STAGE"/demo/*.sh; do
  sed -e 's|$REPO/build/bin|$REPO/bin|g' \
      -e 's|(cd "$(dirname "$0")/../.." \&\& pwd)|(cd "$(dirname "$0")/.." \&\& pwd)|g' \
      "$script" > "$script.tmp"
  mv "$script.tmp" "$script"
  chmod 0755 "$script"
done

{
  print -- "B3 Hive v${BINARY_VERSION} — UNSIGNED RELEASE CANDIDATE"
  print -- ""
  print -- "This archive is for release-candidate testing only."
  print -- "It is not code-signed, notarized, or an authenticated public release."
  print -- "Verify the separately published SHA-256 checksum and obtain the final"
  print -- "signed release through the official B3 distribution channel."
} > "$STAGE/UNSIGNED-RELEASE-CANDIDATE.txt"

tar -C "$WORK_ROOT" -czf "$ARCHIVE" "$NAME"
(cd "$OUT" && shasum -a 256 "$(basename "$ARCHIVE")") > "$CHECKSUM"

print -- "packed unsigned release candidate: $ARCHIVE"
print -- "checksum: $CHECKSUM"
