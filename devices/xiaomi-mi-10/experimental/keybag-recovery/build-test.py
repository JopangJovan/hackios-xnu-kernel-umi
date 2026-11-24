#!/usr/bin/env python3
"""Build/test isolated synthetic bags on macOS; optionally build the iOS helper.
Never installs a job, boots a phone, or discovers real credentials.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ios", action="store_true", help="also compile/sign; does not install")
    args = parser.parse_args()
    os.umask(0o077)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    flags = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-Wno-deprecated-declarations"]
    sources = [str(HERE / f) for f in ("secure_store.c", "store-test.c")]
    exe = out / "store-test"
    subprocess.run(["clang", *flags, "-fsanitize=address,undefined", *sources, "-o", str(exe)], check=True)
    fixture = Path(tempfile.mkdtemp(prefix="synthetic-", dir=out))
    started = time.monotonic()
    with (out / "test.log").open("w") as log:
        for operation in ("create", "verify", "verify"):
            subprocess.run([str(exe), operation, str(fixture)], stdout=log, stderr=subprocess.STDOUT, check=True)
    if args.ios:
        target = out / "store-test-arm64"
        subprocess.run(["xcrun", "--sdk", "iphoneos", "clang", "-arch", "arm64",
                        "-miphoneos-version-min=16.5", *flags, *sources, "-o", str(target)], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", str(target)], check=True)
    files = [HERE / f for f in ("secure_store.h", "secure_store.c", "store-test.c")]
    files.append(HERE.parent / "keybag/umi_aks_crypto.h")
    result = {"result": "PASS", "host_asan_ubsan": True, "repeat_verify": 2,
              "elapsed_seconds": round(time.monotonic() - started, 2),
              "synthetic_fixture": str(fixture), "ios_built_not_installed": args.ios,
              "sha256": {str(p.relative_to(HERE.parent)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS; synthetic fixture retained at", fixture)

if __name__ == "__main__":
    main()
