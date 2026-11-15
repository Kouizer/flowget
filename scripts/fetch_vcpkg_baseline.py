#!/usr/bin/env python3
"""Fetch the vcpkg registry baseline pinned in `vcpkg.json` into a shallow vcpkg clone.

`vcpkg.json` pins `builtin-baseline` to a registry commit, but CI checks out microsoft/vcpkg
with actions/checkout's default `fetch-depth: 1`. That clone holds a single commit — the tip of
master — so the pinned commit disappears as soon as master moves on, and vcpkg fails while
resolving versions:

    error: while checking out baseline from commit '<sha>', failed to `git show`
           versions/baseline.json. This may be fixed by fetching commits with `git fetch`.
    fatal: path 'versions/baseline.json' exists on disk, but not in '<sha>'

The baseline commit itself is the fix: fetch exactly that one commit instead of cloning vcpkg's
full history (hundreds of megabytes) on every run. The SHA is read from the manifest, so bumping
the baseline keeps working without touching this script or the workflow.

Usage: fetch_vcpkg_baseline.py [--vcpkg-dir vcpkg] [--manifest vcpkg.json]

When `GITHUB_OUTPUT` is set, `baseline=<sha>` is appended to it so the workflow can put the
baseline in the binary-cache key.

Exit codes: 0 = baseline readable, 1 = manifest/checkout unusable or fetch failed, 2 = bad usage.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

_SHA1 = re.compile(r"^[0-9a-f]{40}$")
_BASELINE_FILE = "versions/baseline.json"


class ScriptError(Exception):
    """A failure that should be reported without a traceback."""


def read_baseline(manifest: Path) -> str:
    """Return the `builtin-baseline` commit from a vcpkg manifest."""
    try:
        # utf-8-sig: Windows editors may add a BOM to JSON written by hand.
        data = json.loads(manifest.read_text(encoding="utf-8-sig"))
    except OSError as error:
        raise ScriptError(f"cannot read {manifest}: {error}") from error
    except json.JSONDecodeError as error:
        raise ScriptError(f"cannot parse {manifest}: {error}") from error

    baseline = data.get("builtin-baseline") if isinstance(data, dict) else None
    if not isinstance(baseline, str) or not _SHA1.match(baseline.lower()):
        raise ScriptError(f"{manifest}: 'builtin-baseline' must be a 40-character commit hash")
    return baseline.lower()


def git(vcpkg_dir: Path, *args: str) -> subprocess.CompletedProcess[str]:
    """Run git inside the vcpkg checkout, capturing output for error messages."""
    try:
        return subprocess.run(
            ["git", "-C", str(vcpkg_dir), *args],
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError as error:
        raise ScriptError("git is not on PATH") from error


def baseline_available(vcpkg_dir: Path, baseline: str) -> bool:
    """True when `<baseline>:versions/baseline.json` resolves in the checkout."""
    return git(vcpkg_dir, "cat-file", "-e", f"{baseline}:{_BASELINE_FILE}").returncode == 0


def fetch_baseline(vcpkg_dir: Path, baseline: str) -> None:
    """Deepen the shallow clone just enough to contain the pinned baseline commit."""
    result = git(vcpkg_dir, "fetch", "--depth=1", "origin", baseline)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise ScriptError(
            f"could not fetch baseline commit {baseline} into {vcpkg_dir}: {detail}\n"
            "The commit must stay reachable in microsoft/vcpkg (it is an ancestor of master)."
        )
    if not baseline_available(vcpkg_dir, baseline):
        raise ScriptError(
            f"fetched {baseline}, but {_BASELINE_FILE} is still unreadable in {vcpkg_dir}"
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--vcpkg-dir", type=Path, default=Path("vcpkg"))
    parser.add_argument("--manifest", type=Path, default=Path("vcpkg.json"))
    args = parser.parse_args(argv)

    try:
        baseline = read_baseline(args.manifest)
        if not args.vcpkg_dir.is_dir():
            raise ScriptError(f"vcpkg checkout not found: {args.vcpkg_dir}")

        if baseline_available(args.vcpkg_dir, baseline):
            print(f"vcpkg baseline {baseline} already present in {args.vcpkg_dir}")
        else:
            print(f"fetching vcpkg baseline {baseline} (shallow clone in {args.vcpkg_dir})")
            fetch_baseline(args.vcpkg_dir, baseline)
            print(f"vcpkg baseline {baseline} ready")
    except ScriptError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as output:
            output.write(f"baseline={baseline}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
