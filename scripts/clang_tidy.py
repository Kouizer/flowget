#!/usr/bin/env python3
"""Run clang-tidy on the translation units of one build directory and compare with the baseline.

The file list comes from `compile_commands.json`, so it is exactly what the platform builds: on
Linux there is no `storage_win32.cpp`, on Windows there is no `storage_posix.cpp`. The previous CI
command passed `git ls-files 'core/*.cpp' 'cli/*.cpp'`, which asked clang-tidy for files that are
not in the database and produced "not found in compilation database" errors.

Scope: `core/` and `cli/` (production code). `tests/` is intentionally out of scope: test code has
a different style profile and would dominate the baseline; see `tasks/todo.md`.

Usage: clang_tidy.py [--build-dir build/linux-x64] [--clang-tidy clang-tidy-18]
                     [--baseline scripts/clang_tidy_baseline.txt] [--update-baseline] [--list-files]

Exit codes: 0 = findings within the baseline (or baseline updated), 1 = tool/database/list problem,
2 = findings exceed the baseline.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

_WARNING = ": warning: "
_SCOPE = ("core", "cli")


class ScriptError(Exception):
    """A failure that should be reported without a traceback."""


def read_baseline(path: Path) -> int:
    """Return the finding count stored in the baseline file."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise ScriptError(
            f"cannot read the baseline at {path}: {error}\n"
            "Create it with --update-baseline after configuring a build preset."
        ) from error

    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        try:
            return int(stripped)
        except ValueError as error:
            raise ScriptError(
                f"the baseline at {path} does not contain an integer: {stripped}"
            ) from error
    raise ScriptError(f"the baseline at {path} is empty")


def write_baseline(path: Path, count: int) -> None:
    """Replace the baseline with the current count."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "# clang-tidy findings for core/ and cli/ (see scripts/clang_tidy.py).\n"
        "# New code must not add findings: lower this number only when findings are fixed.\n"
        f"{count}\n",
        encoding="utf-8",
        newline="\n",
    )


def select_files(database: Path, build_dir: Path) -> list[Path]:
    """Return the unique project sources under core/ and cli/ that this build compiled."""
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ScriptError(f"cannot read {database}: {error}") from error

    root = Path.cwd().resolve()
    build_root = build_dir.resolve()
    files: set[Path] = set()
    for entry in entries:
        raw = entry.get("file")
        if not raw:
            continue
        source = Path(raw).resolve()
        try:
            relative = source.relative_to(root)
        except ValueError:
            continue  # vendored sources (googletest in build/_deps) live outside the repo root
        if not relative.parts or relative.parts[0] not in _SCOPE:
            continue
        if source == build_root or build_root in source.parents:
            continue
        if source.is_file():
            files.add(source)

    if not files:
        raise ScriptError(
            f"no core/ or cli/ sources found in {database}; is the build directory up to date?"
        )
    return sorted(files)


def run_clang_tidy(clang_tidy: str, build_dir: Path, files: list[Path]) -> int:
    """Run clang-tidy over `files`, stream its output and return the warning count."""
    if shutil.which(clang_tidy) is None:
        raise ScriptError(
            f"'{clang_tidy}' was not found on PATH.\n"
            "Install the version CI uses: sudo apt-get install -y clang-tidy-18"
        )

    version = subprocess.run(
        [clang_tidy, "--version"], capture_output=True, text=True, check=False
    )
    version_line = (version.stdout or "").strip().splitlines()
    print(f"clang-tidy: {version_line[0] if version_line else clang_tidy}")

    command = [clang_tidy, "-p", str(build_dir), *map(str, files)]
    process = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
    )
    assert process.stdout is not None
    count = 0
    for line in process.stdout:
        sys.stdout.write(line)
        if _WARNING in line:
            count += 1
    status = process.wait()
    if status != 0:
        raise ScriptError(
            f"clang-tidy exited with code {status}; this is a toolchain or compile error, not a finding"
        )
    return count


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path, default=Path("build/linux-x64"))
    parser.add_argument("--clang-tidy", default="clang-tidy-18")
    parser.add_argument("--baseline", type=Path, default=Path("scripts/clang_tidy_baseline.txt"))
    parser.add_argument("--update-baseline", action="store_true")
    parser.add_argument("--list-files", action="store_true")
    args = parser.parse_args(argv)

    try:
        database = args.build_dir / "compile_commands.json"
        if not database.is_file():
            raise ScriptError(
                f"{database} not found; configure the build preset first "
                "(cmake --preset linux-x64)"
            )
        files = select_files(database, args.build_dir)
        print(f"build dir: {args.build_dir}")
        print(f"files: {len(files)}")
        if args.list_files:
            for source in files:
                print(source.relative_to(Path.cwd().resolve()))
            return 0

        count = run_clang_tidy(args.clang_tidy, args.build_dir, files)
        if args.update_baseline:
            write_baseline(args.baseline, count)
            print(f"clang-tidy: {count} finding(s); baseline updated at {args.baseline}")
            return 0

        baseline = read_baseline(args.baseline)
        if count > baseline:
            print(
                f"clang-tidy: {count} finding(s) in {len(files)} file(s); "
                f"baseline {baseline} -> EXCEEDED by {count - baseline}",
                file=sys.stderr,
            )
            return 2
        print(
            f"clang-tidy: {count} finding(s) in {len(files)} file(s); baseline {baseline} -> OK"
        )
    except ScriptError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
