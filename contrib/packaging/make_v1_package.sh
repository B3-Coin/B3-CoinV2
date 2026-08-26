#!/bin/zsh
# Pack the B3 FlowMesh v1 working release (safe / fail-closed).
# Usage: make_v1_package.sh [outdir]   (default: dist/)
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$REPO/dist}
BIN="$REPO/build/bin"
VER=$("$BIN/b3coind" -version | head -1 | awk '{print $NF}' 2>/dev/null || echo v31.1.0)
ARCH=$(uname -m)-$(uname -s | tr 'A-Z' 'a-z')
NAME="b3-flowmesh-v1-$ARCH"
STAGE="$OUT/$NAME"
rm -rf "$STAGE"; mkdir -p "$STAGE/bin" "$STAGE/demo" "$STAGE/doc"

for b in b3coind b3coin-cli b3coin-tx b3coin-wallet b3coin-qt b3-bridge-ethcheck; do
  [ -x "$BIN/$b" ] && cp "$BIN/$b" "$STAGE/bin/"
done
cp "$REPO"/contrib/b3hive-demo/*.sh "$STAGE/demo/"
cp "$REPO"/doc/design/b3-v1-working-release.md "$STAGE/README.md"
cp "$REPO"/doc/design/b3-bridge-threat-model.md "$STAGE/doc/"
cp "$REPO"/doc/design/b3-product-identity.md "$STAGE/doc/"
# demo scripts inside the package resolve binaries relative to the package
sed -i '' 's|$REPO/build/bin|$REPO/bin|g; s|(cd "$(dirname "$0")/../.." \&\& pwd)|(cd "$(dirname "$0")/.." \&\& pwd)|g' "$STAGE"/demo/*.sh
tar -C "$OUT" -czf "$OUT/$NAME.tar.gz" "$NAME"
shasum -a 256 "$OUT/$NAME.tar.gz"
echo "packed: $OUT/$NAME.tar.gz"
