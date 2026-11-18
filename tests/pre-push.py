#!/usr/bin/env python3
"""Run the host checks in the detached worktree for the SHA being pushed."""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args, cwd=ROOT, **env):
    subprocess.run(args, cwd=cwd, env={**os.environ, **env}, check=True)


def prepare_checkout(cache, destination, script, *, inputs=(), args=(), version_env, **env):
    # The fetchers derive their output paths from their location. Run them in
    # an empty staging tree so a developer's modified checkout never seeds the cache.
    digest = hashlib.sha256(os.fsencode(os.environ.get(version_env, "")))
    for path in (script, *inputs):
        digest.update(os.fsencode(path) + b"\0" + hashlib.sha256((ROOT / path).read_bytes()).digest())
    key = f"{destination.name}-{digest.hexdigest()}"
    local = ROOT / destination
    local.parent.mkdir(parents=True, exist_ok=True)
    marker = local / ".fc-pre-push-cache-key"
    if marker.is_file() and marker.read_text() == key:
        return
    if local.exists():
        raise SystemExit(f"{local} already exists with a different or unknown cache key; use a fresh verification worktree")

    snapshot = cache / key
    with (cache / (key + ".lock")).open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not snapshot.exists():
            print(f"Fetching {destination.name} for the shared cache", flush=True)
            with tempfile.TemporaryDirectory(dir=cache) as tmp:
                stage = Path(tmp)
                for path in (script, *inputs):
                    (stage / path).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(ROOT / path, stage / path)
                run(stage / script, *args, cwd=stage, **env)
                (stage / destination).rename(snapshot)
    # A private copy also works inside Docker, which mounts only this worktree.
    with tempfile.TemporaryDirectory(dir=local.parent) as tmp:
        copy = Path(tmp) / destination.name
        print(f"Restoring {destination.name} in the worktree", flush=True)
        shutil.copytree(snapshot, copy, symlinks=True)
        (copy / marker.name).write_text(key)
        copy.rename(local)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("remote_tip", help="remote main SHA before the push")
    parser.add_argument("--include-docs", action="store_true", help="also run on a documentation-only diff")
    parser.add_argument("--fuzz", action="store_true", help="also build and run the five local fuzz targets")
    opts = parser.parse_args()
    # Disable rename detection so moving code into docs still counts as deleting code.
    changes = subprocess.check_output(
        ["git", "diff", "--no-renames", "--name-only", "-z", opts.remote_tip, "HEAD", "--"], cwd=ROOT
    ).split(b"\0")
    docs_only = all(
        path == b"LICENSE" or path.startswith(b"docs/") or path.endswith(b".md")
        for path in changes if path
    )
    if docs_only and not (opts.include_docs or opts.fuzz):
        print("No code changes in the outgoing diff; skipping build and tests.")
        return

    mbedtls = os.environ.get("MBEDTLS_DIR")
    if not mbedtls:
        parser.error("set MBEDTLS_DIR to the directory containing MbedTLSConfig.cmake")
    common = subprocess.check_output(["git", "rev-parse", "--git-common-dir"], cwd=ROOT, text=True).strip()
    cache = Path(os.environ.get("FC_CHECK_CACHE", (ROOT / common).resolve().parent / "build/pre-push-cache")).resolve()
    cache.mkdir(parents=True, exist_ok=True)
    patches = sorted(p.relative_to(ROOT) for p in (ROOT / "third_party/patches").glob("wamr-*.patch"))
    prepare_checkout(cache, Path("third_party/wamr"), Path("third_party/fetch-wamr.sh"),
                     inputs=patches, version_env="WAMR_TAG")
    prepare_checkout(cache, Path("build/esp-prov"), Path("tests/e2e/esp-prov.sh"), args=("--checkout",),
                     version_env="IDF_IMAGE", ESP_PROV_LOCAL="1")
    run("port/host/build-ci.sh")
    python_id = hashlib.sha256(os.fsencode(sys.executable) + sys.version.encode()).hexdigest()
    with (cache / "prov-host.lock").open("a") as lock:
        # Independent worktrees share the installer's venv and its loopback port.
        fcntl.flock(lock, fcntl.LOCK_EX)
        run("tests/e2e/prov-host.sh", PROV_VENV=str(cache / f"prov-venv-{python_id}"))
    run("tests/unit.sh", f"-DMbedTLS_DIR={mbedtls}")
    if opts.fuzz:
        cc = os.environ.get("CC", "clang")
        if sys.platform == "darwin" and "CC" not in os.environ:
            llvm = subprocess.check_output(["brew", "--prefix", "llvm"], text=True).strip()
            cc = str(Path(llvm) / "bin/clang")
        run(
            "tests/fuzz.sh", f"-DMbedTLS_DIR={mbedtls}",
            CC=cc,
            FUZZ_SECONDS=os.environ.get("FUZZ_SECONDS", "3"),
            FUZZ_JOBS=os.environ.get("FUZZ_JOBS", "3"),
        )


if __name__ == "__main__":
    main()
