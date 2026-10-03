#!/usr/bin/env python3
from pathlib import Path
import hashlib
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "third_party" / "pdp7-unix" / "build"
DST = ROOT / "components" / "simh_pdp7_probe" / "assets"

EXPECTED = {
    "image.fs": "0892392eb8de98db5a0ac5862fb836ab171f9bc7bd6c460e553bab1f1407f6ed",
    "boot.rim": "a69adf03a700058300501b2e4a74e732b344fd175e727c9a87c7c7b7132bd4f2",
}

def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()

def main():
    missing = [name for name in EXPECTED if not (SRC / name).is_file()]
    if missing:
        print("Missing pdp7-unix output: " + ", ".join(missing), file=sys.stderr)
        print("Run: make -C third_party/pdp7-unix", file=sys.stderr)
        return 2

    DST.mkdir(parents=True, exist_ok=True)
    for name, expected in EXPECTED.items():
        src = SRC / name
        got = sha256(src)
        if got != expected:
            print(f"{name}: SHA256 mismatch", file=sys.stderr)
            print(f"got      {got}", file=sys.stderr)
            print(f"expected {expected}", file=sys.stderr)
            return 3
        dst = DST / name
        shutil.copy2(src, dst)
        if sha256(dst) != expected:
            print(f"{name}: post-copy verification failed", file=sys.stderr)
            return 4
        print(f"PASS {name} {expected}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
