#!/bin/zsh
# B3 FlowMesh v1 working model -- local demo network.
#
# Boots four validator nodes on the modern regtest (-b3modernregtest, the
# architecture-contract section-64 activation): deterministic legacy genesis,
# 160-block PoW corridor, then Modern PoS with live BLS finality (four
# validators, weights 15/10/5/1, quorum 21/31, epochs of 30 blocks).
#
# Usage:   ./start_network.sh <workdir>
# Attach B3 Hive afterwards with the command the script prints.
# Stop:    ./stop_network.sh <workdir>
set -e
WORK=${1:?usage: start_network.sh <workdir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$REPO/build/bin"
[ -x "$BIN/b3coind" ] || { echo "build b3coind first (cmake --build build --target b3coind b3coin-cli)"; exit 1; }
mkdir -p "$WORK"

ARGS=(-regtest -b3modernregtest -b3corridorlength=160 -fallbackfee=0.0001
      -addresstype=legacy -changetype=legacy -server -listen=0)
P2P0=19800

# -- launch ------------------------------------------------------------------
for i in 0 1 2 3; do
  mkdir -p "$WORK/n$i"
  EXTRA=()
  if [ $i -eq 0 ]; then EXTRA=(-listen=1 -port=$P2P0 -discover=0); else EXTRA=(-connect=127.0.0.1:$P2P0); fi
  "$BIN/b3coind" "${ARGS[@]}" "${EXTRA[@]}" -datadir="$WORK/n$i" -rpcport=$((19900+i)) -daemon
done

rpc() { local i=$1; shift; "$BIN/b3coin-cli" -regtest -datadir="$WORK/n$i" -rpcport=$((19900+i)) "$@" }
for i in 0 1 2 3; do
  until rpc $i getblockcount >/dev/null 2>&1; do sleep 0.5; done
  rpc $i createwallet demo >/dev/null
done
echo "[demo] four nodes up"

mine() { # mine $1 corridor blocks with future-bound pacing
  local left=$1 addr=$2
  while [ $left -gt 0 ]; do
    local chunk=$(( left < 90 ? left : 90 ))
    rpc 0 generatetoaddress $chunk "$addr" >/dev/null
    left=$((left-chunk))
    [ $left -gt 0 ] && { echo "[demo] corridor pacing pause (future bound)..."; sleep 45; }
  done
}

ADDR0=$(rpc 0 getnewaddress)
echo "[demo] mining corridor funding blocks..."
mine 135 "$ADDR0"

echo "[demo] funding the other three validators"
STAKES=(15 10 5 1)
for i in 1 2 3; do
  rpc 0 sendtoaddress "$(rpc $i getnewaddress)" $((STAKES[i+1]+5)) >/dev/null
done
rpc 0 generatetoaddress 2 "$ADDR0" >/dev/null

echo "[demo] STAKE + FINALITY_KEY binding on all four validators"
for i in 0 1 2 3; do
  rpc $i createstake ${STAKES[i+1]} >/dev/null
  rpc $i bindfinalitykey >/dev/null
done
# wait until every stake + binding tx has relayed to the miner (n0's own
# two are local; six more must arrive from n1..n3)
for t in $(seq 1 60); do
  MP=$(rpc 0 getmempoolinfo | python3 -c "import json,sys; print(json.load(sys.stdin)['size'])")
  [ "$MP" -ge 8 ] && break
  sleep 1
done
echo "[demo] mempool has $MP transactions (need 8)"
rpc 0 generatetoaddress 3 "$ADDR0" >/dev/null

H=$(rpc 0 getblockcount)
echo "[demo] corridor to height 160 (now $H)"
mine $((160-H)) "$ADDR0"

echo "[demo] starting staking + finality signing on all nodes"
for i in 0 1 2 3; do rpc $i startstaking >/dev/null; done

echo "[demo] waiting for Modern PoS finality (quorum 21/31)..."
for t in $(seq 1 120); do
  FIN=$(rpc 0 getfinalitystatus 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('finalized',{}).get('height',-1))" 2>/dev/null || echo -1)
  [ "$FIN" -ge 166 ] && break
  sleep 2
done
rpc 0 getfinalitystatus | python3 -m json.tool | head -20

cat <<DONE

============================================================
 B3 FlowMesh v1 demo network is LIVE and finalizing.
   workdir: $WORK   (nodes n0..n3, RPC 19900..19903)

 Watch it:
   $BIN/b3coin-cli -regtest -datadir=$WORK/n0 -rpcport=19900 getfinalitystatus

 Attach B3 Hive (observer node with its own wallet):
   $BIN/b3coin-qt -regtest -b3modernregtest -b3corridorlength=160 \\
     -datadir=$WORK/hive -connect=127.0.0.1:$P2P0 -fallbackfee=0.0001 \\
     -addresstype=legacy -changetype=legacy

 Stop everything:
   $(dirname "$0")/stop_network.sh $WORK
============================================================
DONE
