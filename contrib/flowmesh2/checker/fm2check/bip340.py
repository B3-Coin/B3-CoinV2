# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""BIP340 Schnorr signatures over secp256k1, from the BIP's specification
text. Standard library only, not constant time: test keys only."""

import hashlib

P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)


def _tagged(tag, data):
    t = hashlib.sha256(tag.encode()).digest()
    return hashlib.sha256(t + t + data).digest()


# Jacobian coordinates keep scalar multiplication fast enough for batches.
def _to_jac(p):
    return None if p is None else (p[0], p[1], 1)


def _from_jac(p):
    if p is None:
        return None
    x, y, z = p
    zi = pow(z, P - 2, P)
    zi2 = zi * zi % P
    return (x * zi2 % P, y * zi2 * zi % P)


def _jac_double(p):
    if p is None:
        return None
    x, y, z = p
    if y == 0:
        return None
    s = 4 * x * y * y % P
    m = 3 * x * x % P
    nx = (m * m - 2 * s) % P
    ny = (m * (s - nx) - 8 * pow(y, 4, P)) % P
    nz = 2 * y * z % P
    return (nx, ny, nz)


def _jac_add(p, q):
    if p is None:
        return q
    if q is None:
        return p
    x1, y1, z1 = p
    x2, y2, z2 = q
    z1s, z2s = z1 * z1 % P, z2 * z2 % P
    u1, u2 = x1 * z2s % P, x2 * z1s % P
    s1, s2 = y1 * z2s * z2 % P, y2 * z1s * z1 % P
    if u1 == u2:
        return _jac_double(p) if s1 == s2 else None
    h, r = (u2 - u1) % P, (s2 - s1) % P
    h2 = h * h % P
    h3 = h * h2 % P
    nx = (r * r - h3 - 2 * u1 * h2) % P
    ny = (r * (u1 * h2 - nx) - s1 * h3) % P
    return (nx, ny, h * z1 * z2 % P)


def point_mul(p, k):
    result, addend = None, _to_jac(p)
    while k:
        if k & 1:
            result = _jac_add(result, addend)
        addend = _jac_double(addend)
        k >>= 1
    return _from_jac(result)


def point_add(p, q):
    return _from_jac(_jac_add(_to_jac(p), _to_jac(q)))


def lift_x(x):
    if x >= P:
        return None
    y_sq = (pow(x, 3, P) + 7) % P
    y = pow(y_sq, (P + 1) // 4, P)
    if pow(y, 2, P) != y_sq:
        return None
    return (x, y if y % 2 == 0 else P - y)


def pubkey(seckey):
    d = int.from_bytes(seckey, 'big')
    if not 1 <= d < N:
        raise ValueError('secret key out of range')
    return point_mul(G, d)[0].to_bytes(32, 'big')


def sign(msg, seckey, aux=bytes(32)):
    d0 = int.from_bytes(seckey, 'big')
    if not 1 <= d0 < N:
        raise ValueError('secret key out of range')
    pk = point_mul(G, d0)
    d = d0 if pk[1] % 2 == 0 else N - d0
    t = (d ^ int.from_bytes(_tagged('BIP0340/aux', aux), 'big')).to_bytes(32, 'big')
    k0 = int.from_bytes(_tagged('BIP0340/nonce', t + pk[0].to_bytes(32, 'big') + msg), 'big') % N
    if k0 == 0:
        raise ValueError('zero nonce')
    r = point_mul(G, k0)
    k = k0 if r[1] % 2 == 0 else N - k0
    rx = r[0].to_bytes(32, 'big')
    e = int.from_bytes(_tagged('BIP0340/challenge', rx + pk[0].to_bytes(32, 'big') + msg), 'big') % N
    return rx + ((k + e * d) % N).to_bytes(32, 'big')


def verify(msg, pub, sig):
    if len(pub) != 32 or len(sig) != 64:
        return False
    p = lift_x(int.from_bytes(pub, 'big'))
    r = int.from_bytes(sig[:32], 'big')
    s = int.from_bytes(sig[32:], 'big')
    if p is None or r >= P or s >= N:
        return False
    e = int.from_bytes(_tagged('BIP0340/challenge', sig[:32] + pub + msg), 'big') % N
    big_r = point_add(point_mul(G, s), point_mul(p, N - e))
    return big_r is not None and big_r[1] % 2 == 0 and big_r[0] == r
