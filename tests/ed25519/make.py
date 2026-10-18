#!/usr/bin/env -S uv run --no-project --with cryptography python3
"""Writes tests/ed25519/cases.txt: Ed25519 public keys, signatures and messages, each with the verdict of
ed25519-dalek 3.0.0's verify_strict (tests/ed25519/oracle), which gs_ed25519_verify must match.

The cases are signatures made by Python's cryptography package, those altered in a byte, and edge cases
made here with the curve's arithmetic (RFC 8032, section 5.1): keys and R of small order and of mixed
order, encodings that are not canonical, and S at and above the group's order.

    tests/ed25519/make.py   (needs cargo, and crates.io or a crate cache, for the oracle)"""
import hashlib, os, random, subprocess, sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

p = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
d = -121665 * pow(121666, p - 2, p) % p
I = pow(2, (p - 1) // 4, p)

def add(P, Q):
    x1, y1 = P; x2, y2 = Q
    t = d * x1 * x2 * y1 * y2 % p
    return ((x1 * y2 + x2 * y1) * pow(1 + t, p - 2, p) % p, (y1 * y2 + x1 * x2) * pow(1 - t, p - 2, p) % p)

def mul(k, P):
    R = (0, 1)
    while k:
        if k & 1: R = add(R, P)
        P, k = add(P, P), k >> 1
    return R

def x_of(y, sign):
    xx = (y * y - 1) * pow(d * y * y + 1, p - 2, p) % p
    x = pow(xx, (p + 3) // 8, p)
    if (x * x - xx) % p: x = x * I % p
    if (x * x - xx) % p: return None
    return p - x if x & 1 != sign else x

def enc(P, y=None):  # y: an encoding of P's y other than the canonical one
    x, yy = P
    return ((yy if y is None else y) | (x & 1) << 255).to_bytes(32, "little")

B = (x_of(4 * pow(5, p - 2, p) % p, 0), 4 * pow(5, p - 2, p) % p)
# The points of small order: the torsion subgroup, from a point of order 8.
T8 = next(mul(L, (x, y)) for y in range(2, 100) if (x := x_of(y, 0)) is not None and mul(4, mul(L, (x, y))) != (0, 1))
torsion = [mul(i, T8) for i in range(8)]

def k_of(R, A, m):
    return int.from_bytes(hashlib.sha512(R + A + m).digest(), "little") % L

def sign(a, A, m, r=None, torsion_r=(0, 1)):  # a signature with secret scalar a for the encoded key A; R may carry torsion
    r = r or random.randrange(1, L)
    R = enc(add(mul(r, B), torsion_r))
    return R + ((r + k_of(R, A, m) * a) % L).to_bytes(32, "little")

random.seed(8032)
cases = []
for i in range(120):  # the cryptography package's signatures, and altered ones
    key = Ed25519PrivateKey.generate()
    pk = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    m = os.urandom(random.choice([0, 1, 32, 200]))
    sig = key.sign(m)
    cases.append((pk, sig, m))
    which = random.randrange(3)
    bad = [bytearray(pk), bytearray(sig), bytearray(m or b"x")]
    bad[which][random.randrange(len(bad[which]))] ^= 1 << random.randrange(8)
    cases.append((bytes(bad[0]), bytes(bad[1]), bytes(bad[2]) if m or which == 2 else m))
    s = int.from_bytes(sig[32:], "little")
    for s2 in (s + L, s + 2 * L, s + 7 * L):  # S above the order, the same point
        if s2 < 2**256: cases.append((pk, sig[:32] + s2.to_bytes(32, "little"), m))
for T in torsion:  # keys and R of small order, in every pairing, with S = 0 and random
    for U in torsion:
        for e in {enc(T), enc(T, T[1] + p) if T[1] + p < 2**255 else enc(T)} | ({(T[1] | 1 << 255).to_bytes(32, "little")} if T[0] == 0 else set()):
            for s in (0, random.randrange(L)):
                cases.append((e, enc(U) + s.to_bytes(32, "little"), b"small"))
for i in range(80):  # keys and R of mixed order: right with the cofactor, and without it only when the torsion cancels
    a = random.randrange(1, L)
    T, U = random.choice(torsion[1:]), random.choice(torsion)
    m = b"mixed %d" % i
    A = enc(add(mul(a, B), T))
    cases.append((A, sign(a, A, m, torsion_r=U), m))
    A = enc(mul(a, B))
    cases.append((A, sign(a, A, m, torsion_r=random.choice(torsion[1:])), m))
for y in range(0, 19):  # encodings with y above p, of points on the curve and off it
    for sign_bit in (0, 1):
        e = (y + p | sign_bit << 255).to_bytes(32, "little")
        a = random.randrange(1, L)
        cases.append((e, sign(a, e, b"odd key"), b"odd key"))
        A = enc(mul(a, B))
        cases.append((A, e + sign(a, A, b"odd R")[32:], b"odd R"))
neg = lambda P: ((p - P[0]) % p, P[1])
for T in torsion:  # small-order keys and R for which [S]B - [k]A = R does hold: only the small-order rule refuses them
    for i in range(400):
        m = b"holds %d" % i
        for U in torsion:
            R = enc(U)
            if mul(k_of(R, enc(T), m), neg(T)) == U:
                cases.append((enc(T), R + bytes(32), m))
                break
        else: continue
        break
for T in torsion[1:]:  # a small-order key with a good R and an S that make the equation hold: only the key's rule refuses
    for i in range(400):
        m, sc, U = b"small key %d" % i, random.randrange(1, L), random.choice(torsion)
        R = enc(add(mul(sc, B), U))
        if mul(k_of(R, enc(T), m), neg(T)) == U:
            cases.append((enc(T), R + sc.to_bytes(32, "little"), m))
            break
for i in range(4):  # R the identity, with S chosen to make the equation hold for a good key
    a = random.randrange(1, L)
    A, R, m = enc(mul(a, B)), enc((0, 1)), b"identity %d" % i
    cases.append((A, R + (k_of(R, A, m) * a % L).to_bytes(32, "little"), m))
for i in range(400):  # and a key of mixed order with R of small order that cancels its torsion
    a, T = random.randrange(1, L), random.choice(torsion[1:])
    A, m = enc(add(mul(a, B), T)), b"cancel %d" % i
    for U in torsion[1:]:
        R = enc(U)
        k = k_of(R, A, m)
        if mul(k, neg(T)) == U:
            cases.append((A, R + (k * a % L).to_bytes(32, "little"), m))
            break
a = random.randrange(1, L)  # S exactly the order, and a signature with R given as the point it should not be
A = enc(mul(a, B))
cases.append((A, sign(a, A, b"m")[:32] + L.to_bytes(32, "little"), b"m"))
cases.append((A, enc(mul(3, B)) + bytes(32), b"m"))

oracle = os.path.join(os.path.dirname(os.path.abspath(__file__)), "oracle")
subprocess.run(["cargo", "build", "--release", "--quiet", "--manifest-path", os.path.join(oracle, "Cargo.toml")] + sys.argv[1:], check=True)
lines = "".join(f"{pk.hex()} {sig.hex()} {m.hex() or '-'}\n" for pk, sig, m in cases)
verdicts = subprocess.run([os.path.join(oracle, "target", "release", "oracle")], input=lines, capture_output=True, text=True, check=True).stdout.split()
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "cases.txt"), "w") as f:
    f.write("# verdict (1 accepted by ed25519-dalek 3.0.0 verify_strict), public key, signature, message (- for none); written by make.py\n")
    for v, line in zip(verdicts, lines.splitlines()): f.write(f"{v} {line}\n")
print(f"{len(cases)} cases, {verdicts.count('1')} accepted")
