#!/usr/bin/env -S uv run --no-project --with cryptography python3
"""Writes tests/update/server: a build 2 of a made-up program, as a publisher would put it out, for
tests/update.c. Its manifest is signed with a key made from a fixed seed (its public half is in the
test); forged/ holds the same manifest with a file's hash changed and the old signature."""
import hashlib, os
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "server")
key = Ed25519PrivateKey.from_private_bytes(hashlib.sha256(b"gesso gs_update test").digest())
files = {"prog": b"#!/bin/sh\necho build 2\n", "data.txt": b"data of build 2\n", "same.txt": b"the same in both builds\n"}
sha = {n: hashlib.sha256(b).hexdigest() for n, b in files.items()}
with open(os.path.join(here, "files", sha["prog"]), "wb") as f: f.write(files["prog"])  # the default place, files/SHA256
with open(os.path.join(here, "assets", "data.txt"), "wb") as f: f.write(files["data.txt"])  # a place the manifest names
manifest = (f"build 2\nfile prog {sha['prog']} {len(files['prog'])}\nfile data.txt {sha['data.txt']} {len(files['data.txt'])} assets/data.txt\n"
            f"file same.txt {sha['same.txt']} {len(files['same.txt'])}\n").encode()
for folder, text in (("", manifest), ("forged", manifest.replace(sha["data.txt"].encode(), sha["same.txt"].encode()))):
    with open(os.path.join(here, folder, "manifest.txt"), "wb") as f: f.write(text)
    with open(os.path.join(here, folder, "manifest.txt.sig"), "w") as f: f.write(key.sign(manifest).hex() + "\n")
print(key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex())
