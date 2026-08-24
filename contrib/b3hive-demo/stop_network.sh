#!/bin/zsh
WORK=${1:?usage: stop_network.sh <workdir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
for i in 0 1 2 3; do
  "$REPO/build/bin/b3coin-cli" -regtest -datadir="$WORK/n$i" -rpcport=$((19900+i)) stop 2>/dev/null || true
done
echo "stopped"
