#!/usr/bin/env bash
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Fetch the pinned tools of the regtest gates into a directory the caller names:
#
#   setup_tools.sh DIR
#
# installs DIR/foundry-v1.8.5/{forge,anvil,cast,chisel,solar} and DIR/solc-0.8.35,
# after checking each download against the SHA-256 pinned below. It never
# overwrites: if either target exists, it stops before downloading anything.
# A failed download or a wrong hash leaves DIR as it was. On success it prints
#
#   export FM2_TOOLS=DIR/foundry-v1.8.5
#   export FM2_SOLC=DIR/solc-0.8.35
#
# which the gates read when --tools and --solc are absent (tool_paths.py).
# Linux x86-64 only. Needs curl, sha256sum and tar.
export LC_ALL=C
set -euo pipefail

FOUNDRY_VERSION=1.8.5
FOUNDRY_URL="https://github.com/foundry-rs/foundry/releases/download/v${FOUNDRY_VERSION}/foundry_v${FOUNDRY_VERSION}_linux_amd64.tar.gz"
FOUNDRY_SHA256=6c66ffcc55fa4249197721baa3098bc208014ea1d8aa04b2ed50ac6bccffb226
FOUNDRY_BINARIES=(forge anvil cast chisel solar)
SOLC_VERSION=0.8.35
SOLC_URL="https://github.com/argotorg/solidity/releases/download/v${SOLC_VERSION}/solc-static-linux"
SOLC_SHA256=fa8ac9a32d301ad023a36ee5a29f8e291fe3200c60244e43c142539e82a617f4

die() { echo "setup_tools.sh: $*" >&2; exit 1; }

[ "$#" -eq 1 ] || die "usage: setup_tools.sh DIR"
[ "$(uname -s)-$(uname -m)" = Linux-x86_64 ] || die "the pinned tools are Linux x86-64 builds"
for tool in curl sha256sum tar; do command -v "$tool" >/dev/null || die "$tool is required"; done

mkdir -p -- "$1"
DIR=$(cd -- "$1" && pwd -P)
FOUNDRY_DIR="$DIR/foundry-v${FOUNDRY_VERSION}"
SOLC_PATH="$DIR/solc-${SOLC_VERSION}"
for target in "$FOUNDRY_DIR" "$SOLC_PATH"; do
    [ ! -e "$target" ] && [ ! -L "$target" ] || die "$target exists; refusing to overwrite"
done

STAGE=$(mktemp -d "$DIR/.setup-tools.XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT

fetch() { # URL FILE SHA256
    curl --fail --silent --show-error --location --proto '=https' --tlsv1.2 --retry 3 -o "$2" -- "$1" \
        || die "download failed: $1"
    local got
    got=$(sha256sum -- "$2" | cut -d' ' -f1)
    [ "$got" = "$3" ] || die "SHA-256 mismatch for $1: expected $3, got $got"
}

fetch "$FOUNDRY_URL" "$STAGE/foundry.tar.gz" "$FOUNDRY_SHA256"
fetch "$SOLC_URL" "$STAGE/solc" "$SOLC_SHA256"

mkdir "$STAGE/foundry"
tar -xzf "$STAGE/foundry.tar.gz" -C "$STAGE/foundry" --no-same-owner
for binary in "${FOUNDRY_BINARIES[@]}"; do
    [ -f "$STAGE/foundry/$binary" ] && [ ! -L "$STAGE/foundry/$binary" ] || die "the Foundry archive has no $binary"
done
chmod 0755 "$STAGE/solc"

# Check again right before the moves, so a concurrent run cannot be overwritten.
for target in "$FOUNDRY_DIR" "$SOLC_PATH"; do
    [ ! -e "$target" ] && [ ! -L "$target" ] || die "$target appeared during setup; refusing to overwrite"
done
mv -T -- "$STAGE/foundry" "$FOUNDRY_DIR"
mv -T -- "$STAGE/solc" "$SOLC_PATH"

echo "installed Foundry ${FOUNDRY_VERSION} and solc ${SOLC_VERSION} under $DIR"
echo "export FM2_TOOLS=$FOUNDRY_DIR"
echo "export FM2_SOLC=$SOLC_PATH"
