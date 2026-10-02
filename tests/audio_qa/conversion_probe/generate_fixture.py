"""Regenerate the public synthetic pack fixture (cryptography 50.0.1).

This deterministic test key is public test material, never a production identity.
Only the private test invocation loads the adjacent trust registry.
"""

import hashlib
import struct
import zipfile
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def main():
    directory = Path(__file__).resolve().parent
    pcm = (directory.parent / "data/known-pcm-v1.s16le.pcm").read_bytes()
    assert hashlib.sha256(pcm).hexdigest() == "5d764a90b6ae1b6606aa95bbd343c54fd1a7b3aa7f7f20e116afc53423081602"
    wave = b"RIFF" + struct.pack("<I", len(pcm) + 36) + b"WAVEfmt "
    wave += struct.pack("<IHHIIHH", 16, 1, 2, 22050, 88200, 4, 16)
    wave += b"data" + struct.pack("<I", len(pcm)) + pcm
    entries = {
        "fixture.wav": wave,
        "manifest.toml": b'[pack]\nname="QA common mix fixture"\nversion="1.0.0"\ngame_id="qa-common-mix"\nayther_min="0.1.0"\n[regions]\ndefault="NTSC"\nsupported=["NTSC"]\n',
    }
    integrity = "version = 1\n"
    for name, content in sorted(entries.items()):
        integrity += f'\n[[entry]]\npath="{name}"\nsha256="{hashlib.sha256(content).hexdigest()}"\nsize={len(content)}\n'
    entries["integrity.toml"] = integrity.encode()
    seed = hashlib.sha256(b"AYTHER QA-020b public fixture key; not production").digest()
    key = Ed25519PrivateKey.from_private_bytes(seed)
    key_id = b"qa-common-mix-fixture"
    entries["signature.bin"] = b"AYTHSIG\0" + bytes([1, len(key_id)]) + key_id + key.sign(integrity.encode())
    data = directory / "data"
    data.mkdir(exist_ok=True)
    with zipfile.ZipFile(data / "common-mix.ay", "w", compression=zipfile.ZIP_STORED) as archive:
        for name, content in sorted(entries.items()):
            archive.writestr(zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0)), content)
    public_key = key.public_key().public_bytes_raw().hex()
    registry = f'version=1\n[[keys]]\nid="{key_id.decode()}"\nalgorithm="ed25519"\npublic_key="{public_key}"\nnot_before_unix=0\nnot_after_unix=4102444800\ngames=["qa-common-mix"]\n'
    (data / "trust.toml").write_text(registry, encoding="utf-8", newline="\n")
    for path in sorted(data.iterdir()):
        print(path.name, hashlib.sha256(path.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
