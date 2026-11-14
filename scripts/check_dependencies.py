#!/usr/bin/env python3
"""Fail the build when a dependency manifest declares GPL/AGPL code (ADR-0004).

Scope: manifests only, not the resolved dependency tree. A GPL package can therefore still enter
through a transitive dependency of an allowed port; the vcpkg licence report
(`vcpkg depend-info --format=tree` plus `THIRD_PARTY_LICENSES.md`, M7) covers that gap, and this
script covers the common accident: someone adds a copyleft package by name.

Exit codes: 0 = clean, 1 = copyleft dependency found, 2 = manifest could not be parsed.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# A name token that is itself a copyleft marker: gpl, gpl2, gpl3, gplv2, gplv3, agpl, agpl3, ...
_COPYLEFT_TOKEN = re.compile(r"^a?gpl(v?\d+(\.\d+)*)?$", re.IGNORECASE)

# LGPL is allowed only for dynamically linked FFmpeg (ADR-0004 §2), so it is a warning that forces a
# human review rather than a failure.
_LGPL_TOKEN = re.compile(r"^lgpl(v?\d+(\.\d+)*)?$", re.IGNORECASE)

# Copyleft packages whose names carry no licence token. Kept short and explicit on purpose: every
# addition is a reviewed fact, not a guess.
_KNOWN_COPYLEFT = {
    "cdrtools",
    "libdvdcss",
    "libmad",
    "libx264",
    "libx265",
    "mplayer",
    "x264",
    "x265",
    "youtubedl-android",
}

DEFAULT_MANIFESTS = ("vcpkg.json", "app/pubspec.yaml", "extension/package.json")

Dependencies = list[str]


def _name_tokens(name: str) -> list[str]:
    """Split a package name into lowercase tokens: '@scope/gpl-thing' -> ['scope', 'gpl', 'thing']."""
    return [token for token in re.split(r"[^0-9a-z]+", name.lower()) if token]


def classify(name: str) -> str | None:
    """Return 'deny' for GPL/AGPL, 'review' for LGPL, or None when the name looks fine."""
    flat = name.lower()
    if flat in _KNOWN_COPYLEFT:
        return "deny"

    tokens = _name_tokens(flat)
    if any(_COPYLEFT_TOKEN.match(token) for token in tokens):
        return "deny"
    if any(_LGPL_TOKEN.match(token) for token in tokens):
        return "review"
    return None


def _deps_from_vcpkg(data: dict) -> Dependencies:
    names: Dependencies = []

    def collect(dependencies: object) -> None:
        if not isinstance(dependencies, list):
            return
        for dependency in dependencies:
            if isinstance(dependency, str):
                names.append(dependency)
            elif isinstance(dependency, dict) and isinstance(dependency.get("name"), str):
                names.append(dependency["name"])
            elif isinstance(dependency, dict) and isinstance(dependency.get("host"), str):
                names.append(dependency["host"])

    collect(data.get("dependencies"))
    features = data.get("features")
    if isinstance(features, dict):
        for feature in features.values():
            if isinstance(feature, dict):
                collect(feature.get("dependencies"))
    return names


def _deps_from_pubspec(text: str) -> Dependencies:
    """Minimal YAML reader: enough for pubspec dependency blocks, without a PyYAML dependency."""
    sections = {"dependencies", "dev_dependencies", "dependency_overrides"}
    names: Dependencies = []
    section: str | None = None

    for raw_line in text.splitlines():
        line = raw_line.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip())
        stripped = line.strip()

        if indent == 0:
            section = stripped[:-1] if stripped.endswith(":") else None
            continue
        # Direct dependencies sit exactly one level below the section header; anything deeper
        # (`sdk: flutter`, version constraints) is not a package name.
        if section not in sections or indent != 2:
            continue

        match = re.match(r"^([A-Za-z0-9_.\-]+)\s*:", stripped)
        if match:
            names.append(match.group(1))
    return names


def _deps_from_package_json(data: dict) -> Dependencies:
    names: Dependencies = []
    for key in ("dependencies", "devDependencies", "peerDependencies", "optionalDependencies"):
        block = data.get(key)
        if isinstance(block, dict):
            names.extend(str(name) for name in block)
    return names


def read_manifest(path: Path) -> Dependencies:
    # utf-8-sig tolerates the BOM that Windows editors happily add to JSON/YAML.
    text = path.read_text(encoding="utf-8-sig")
    if path.name == "vcpkg.json":
        return _deps_from_vcpkg(json.loads(text))
    if path.name == "package.json":
        return _deps_from_package_json(json.loads(text))
    if path.name == "pubspec.yaml":
        return _deps_from_pubspec(text)
    raise ValueError(f"unsupported manifest format: {path.name}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument(
        "manifests",
        nargs="*",
        help=f"manifest paths relative to --root (default: {', '.join(DEFAULT_MANIFESTS)})",
    )
    args = parser.parse_args(argv)

    root: Path = args.root
    manifests = [root / name for name in (args.manifests or DEFAULT_MANIFESTS)]

    violations: list[str] = []
    reviews: list[str] = []
    inspected = 0

    for manifest in manifests:
        if not manifest.is_file():
            # Absent manifests are normal: app/ and extension/ arrive with M3/M4.
            continue
        try:
            names = read_manifest(manifest)
        except (json.JSONDecodeError, ValueError) as error:
            print(f"error: cannot parse {manifest}: {error}", file=sys.stderr)
            return 2

        inspected += 1
        print(f"{manifest}: {len(names)} declared dependencies")
        for name in names:
            verdict = classify(name)
            if verdict == "deny":
                violations.append(f"{manifest}: '{name}' is GPL/AGPL (ADR-0004 forbids linking it)")
            elif verdict == "review":
                reviews.append(f"{manifest}: '{name}' is LGPL; allowed only as a dynamic FFmpeg build")

    for message in reviews:
        print(f"warning: {message}")
    for message in violations:
        print(f"error: {message}", file=sys.stderr)

    if violations:
        return 1
    if inspected == 0:
        print(f"error: no manifest found in {root}", file=sys.stderr)
        return 2

    print(f"ok: {inspected} manifest(s), no GPL/AGPL dependencies")
    return 0


if __name__ == "__main__":
    sys.exit(main())
