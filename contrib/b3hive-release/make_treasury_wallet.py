#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""B3 dev-treasury wallet generator — SIMPLE, OFFLINE, ZERO DEPENDENCIES.

Owner ruling 2026-08-26: the treasury is one plain wallet, no complexity,
no locks. This script creates exactly that: one secp256k1 key from the
operating system's cryptographic randomness, and the derived legacy P2PKH
address (mainnet base58 version 63, WIF 153 — the values pinned in
src/kernel/chainparams.cpp).

RUN THIS OFFLINE ON THE OWNER'S MACHINE. The private key (WIF) is written
ONLY to the key file (mode 0600) — never printed, never transmitted. Only
the ADDRESS should ever leave the machine (it becomes the pinned treasury
destination). To spend later, import into B3 Hive with:
    b3coin-cli getdescriptorinfo "pkh(<WIF>)"          -> checksum
    b3coin-cli importdescriptors '[{"desc":"pkh(<WIF>)#<checksum>","timestamp":"now"}]'
(verified end-to-end against the node: script-derived address == node-derived).

Usage:
    make_treasury_wallet.py <keyfile>            # mainnet
    make_treasury_wallet.py <keyfile> --regtest  # test derivation only
    make_treasury_wallet.py --address-of <keyfile> [--regtest]
"""
import hashlib
import os
import sys

# ---- secp256k1 (pure python, standard parameters) ---------------------------
P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
Gx = 0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
Gy = 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8


def _inv(a, m):
    return pow(a, m - 2, m)


def _add(pt1, pt2):
    if pt1 is None:
        return pt2
    if pt2 is None:
        return pt1
    x1, y1 = pt1
    x2, y2 = pt2
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if pt1 == pt2:
        lam = (3 * x1 * x1) * _inv(2 * y1, P) % P
    else:
        lam = (y2 - y1) * _inv(x2 - x1, P) % P
    x3 = (lam * lam - x1 - x2) % P
    y3 = (lam * (x1 - x3) - y1) % P
    return (x3, y3)


def _mul(k, pt):
    result = None
    addend = pt
    while k:
        if k & 1:
            result = _add(result, addend)
        addend = _add(addend, addend)
        k >>= 1
    return result


def pubkey_compressed(priv: int) -> bytes:
    x, y = _mul(priv, (Gx, Gy))
    return bytes([2 + (y & 1)]) + x.to_bytes(32, "big")


# ---- RIPEMD-160 (hashlib when available, pure-python fallback) --------------
def ripemd160(data: bytes) -> bytes:
    try:
        h = hashlib.new("ripemd160")
        h.update(data)
        return h.digest()
    except Exception:
        return _ripemd160_pure(data)


def _ripemd160_pure(m: bytes) -> bytes:
    # Compact RIPEMD-160 per the specification.
    def rol(x, n):
        return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF

    r1 = [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
          7,4,13,1,10,6,15,3,12,0,9,5,2,14,11,8,
          3,10,14,4,9,15,8,1,2,7,0,6,13,11,5,12,
          1,9,11,10,0,8,12,4,13,3,7,15,14,5,6,2,
          4,0,5,9,7,12,2,10,14,1,3,8,11,6,15,13]
    r2 = [5,14,7,0,9,2,11,4,13,6,15,8,1,10,3,12,
          6,11,3,7,0,13,5,10,14,15,8,12,4,9,1,2,
          15,5,1,3,7,14,6,9,11,8,12,2,10,0,4,13,
          8,6,4,1,3,11,15,0,5,12,2,13,9,7,10,14,
          12,15,10,4,1,5,8,7,6,2,13,14,0,3,9,11]
    s1 = [11,14,15,12,5,8,7,9,11,13,14,15,6,7,9,8,
          7,6,8,13,11,9,7,15,7,12,15,9,11,7,13,12,
          11,13,6,7,14,9,13,15,14,8,13,6,5,12,7,5,
          11,12,14,15,14,15,9,8,9,14,5,6,8,6,5,12,
          9,15,5,11,6,8,13,12,5,12,13,14,11,8,5,6]
    s2 = [8,9,9,11,13,15,15,5,7,7,8,11,14,14,12,6,
          9,13,15,7,12,8,9,11,7,7,12,7,6,15,13,11,
          9,7,15,11,8,6,6,14,12,13,5,14,13,13,7,5,
          15,5,8,11,14,14,6,14,6,9,12,9,12,5,15,8,
          8,5,12,9,12,5,14,6,8,13,6,5,15,13,11,11]
    K1 = [0x00000000, 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xA953FD4E]
    K2 = [0x50A28BE6, 0x5C4DD124, 0x6D703EF3, 0x7A6D76E9, 0x00000000]

    def f(j, x, y, z):
        if j < 16: return x ^ y ^ z
        if j < 32: return (x & y) | (~x & z)
        if j < 48: return (x | ~y) ^ z
        if j < 64: return (x & z) | (y & ~z)
        return x ^ (y | ~z)

    padded = m + b"\x80" + b"\x00" * ((55 - len(m)) % 64) + (8 * len(m)).to_bytes(8, "little")
    h = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0]
    for off in range(0, len(padded), 64):
        X = [int.from_bytes(padded[off + 4 * i:off + 4 * i + 4], "little") for i in range(16)]
        a, b, c, d, e = h
        A, B, C, D, E = h
        for j in range(80):
            T = (a + f(j, b, c, d) + X[r1[j]] + K1[j // 16]) & 0xFFFFFFFF
            T = (rol(T, s1[j]) + e) & 0xFFFFFFFF
            a, e, d, c, b = e, d, rol(c, 10), b, T
            T = (A + f(79 - j, B, C, D) + X[r2[j]] + K2[j // 16]) & 0xFFFFFFFF
            T = (rol(T, s2[j]) + E) & 0xFFFFFFFF
            A, E, D, C, B = E, D, rol(C, 10), B, T
        T = (h[1] + c + D) & 0xFFFFFFFF
        h = [T,
             (h[2] + d + E) & 0xFFFFFFFF,
             (h[3] + e + A) & 0xFFFFFFFF,
             (h[4] + a + B) & 0xFFFFFFFF,
             (h[0] + b + C) & 0xFFFFFFFF]
    return b"".join(x.to_bytes(4, "little") for x in h)


# ---- base58check ------------------------------------------------------------
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58check(payload: bytes) -> str:
    chk = hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    n = int.from_bytes(payload + chk, "big")
    out = ""
    while n:
        n, r = divmod(n, 58)
        out = B58[r] + out
    for byte in payload:
        if byte == 0:
            out = "1" + out
        else:
            break
    return out


def derive(priv: int, mainnet: bool):
    pub = pubkey_compressed(priv)
    h160 = ripemd160(hashlib.sha256(pub).digest())
    addr_ver = 63 if mainnet else 111   # chainparams PUBKEY_ADDRESS
    wif_ver = 153 if mainnet else 239   # chainparams SECRET_KEY
    address = b58check(bytes([addr_ver]) + h160)
    wif = b58check(bytes([wif_ver]) + priv.to_bytes(32, "big") + b"\x01")  # compressed
    return address, wif


def main():
    args = [a for a in sys.argv[1:]]
    mainnet = "--regtest" not in args
    args = [a for a in args if a != "--regtest"]

    if len(args) == 2 and args[0] == "--address-of":
        wif = open(args[1]).read().strip()
        # decode WIF
        num = 0
        for c in wif:
            num = num * 58 + B58.index(c)
        raw = num.to_bytes(38 + wif.count("1"), "big").lstrip(b"\x00")
        body, chk = raw[:-4], raw[-4:]
        assert hashlib.sha256(hashlib.sha256(body).digest()).digest()[:4] == chk, "bad WIF checksum"
        priv = int.from_bytes(body[1:33], "big")
        address, _ = derive(priv, mainnet)
        print(address)
        return

    if len(args) != 1:
        print(__doc__)
        sys.exit(2)
    keyfile = args[0]
    if os.path.exists(keyfile):
        sys.exit(f"refusing to overwrite existing key file: {keyfile}")

    priv = 0
    while not (1 <= priv < N):
        priv = int.from_bytes(os.urandom(32), "big")
    address, wif = derive(priv, mainnet)

    fd = os.open(keyfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(wif + "\n")

    print(f"network:  {'mainnet' if mainnet else 'regtest'}")
    print(f"address:  {address}")
    print(f"key file: {keyfile}  (WIF, mode 0600 — NEVER share; import with importprivkey)")
    print("Only the ADDRESS leaves this machine.")


if __name__ == "__main__":
    main()
