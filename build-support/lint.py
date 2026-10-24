#!/usr/bin/env python3
#
# Copyright (c) YugabyteDB, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
"""YugabyteDB linter.

Reads the repo's ``.lint`` config and runs the configured linters on changed files (or files
passed on the command line).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import traceback
from dataclasses import dataclass, field
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
CPPLINT_PATH = REPO_ROOT / "src" / "lint" / "cpplint.py"


@dataclass
class Message:
    """A single lint finding."""

    path: str
    line: int | None
    severity: str
    code: str
    message: str
    linter: str

    def format(self) -> str:
        """Render the message as a single display line."""
        loc = f":{self.line}" if self.line else ""
        return f"{self.severity.upper()} {self.linter}/{self.code} {self.path}{loc}: {self.message}"


@dataclass
class Linter:
    """A configured linter from ``.lint`` with compiled include/exclude regexes."""

    name: str
    config: dict
    includes: list[re.Pattern] = field(default_factory=list)
    excludes: list[re.Pattern] = field(default_factory=list)

    @classmethod
    def build(cls, name: str, config: dict, global_excludes: list[re.Pattern]) -> "Linter":
        """Create a Linter from raw JSON config plus the repo-wide exclude patterns."""
        includes = _compile_patterns(config.get("include", []))
        excludes = list(global_excludes) + _compile_patterns(config.get("exclude", []))
        return cls(name=name, config=config, includes=includes, excludes=excludes)

    def matches(self, path: str) -> bool:
        """Return True if ``path`` should be linted by this linter."""
        if self.includes and not any(p.search(path) for p in self.includes):
            return False
        return not any(p.search(path) for p in self.excludes)


def _compile_patterns(raw) -> list[re.Pattern]:
    if raw is None:
        return []
    if isinstance(raw, str):
        raw = [raw]
    return [re.compile(p) for p in raw]


def _git(*args: str) -> str:
    return subprocess.check_output(["git", *args], cwd=REPO_ROOT, text=True).strip()


_CANONICAL_REPO_HINT = "yugabyte/yugabyte-db"

# 2.x preview/stable (e.g. 2.13, 2.20, 2.30) and YYYY.N stable releases.
_RELEASE_BRANCH_RE = re.compile(r"^(2\.[0-9]+|[0-9]{4}\.[0-9]+)$")


def _looks_like_release_branch(name: str) -> bool:
    return bool(_RELEASE_BRANCH_RE.match(name))


def _find_canonical_remote() -> str | None:
    """Return the name of the remote whose URL points at the canonical repo, or None."""
    try:
        for remote in _git("remote").splitlines():
            if not remote:
                continue
            try:
                url = _git("remote", "get-url", remote)
            except subprocess.CalledProcessError:
                continue
            if _CANONICAL_REPO_HINT in url:
                return remote
    except subprocess.CalledProcessError:
        return None
    return None


def _changed_files(base: str | None = None) -> list[str]:
    """Files modified in the working copy plus files changed on this branch vs its base.

    If ``base`` is provided, it is used directly. Otherwise ``@{upstream}`` is used. If
    ``@{upstream}`` isn't set (e.g. a fresh branch that hasn't been pushed yet), fall back
    to ``<canonical-remote>/master`` -- where ``<canonical-remote>`` is the git remote whose
    URL points at the canonical ``yugabyte/yugabyte-db`` repo (commonly named ``upstream``
    in fork-based contributor setups, or ``origin`` in non-fork-based checkouts). We do not
    fall back to *any* remote's master indiscriminately: on a stable release branch
    (e.g. 2024.2) with no ``@{upstream}``, that would include a huge unrelated fileset;
    pass ``--rev <remote>/<release-branch>`` explicitly in that case.
    """
    if base is not None:
        try:
            _git("rev-parse", "--verify", "--quiet", base)
        except subprocess.CalledProcessError:
            sys.exit(f"[lint] --rev: unknown git ref {base!r}")
    else:
        try:
            _git("rev-parse", "--verify", "--quiet", "@{upstream}")
            base = "@{upstream}"
        except subprocess.CalledProcessError:
            canonical = _find_canonical_remote()
            fallback = None
            if canonical:
                # Try <canonical>/<current-branch> first. If the user is on a
                # release branch (2024.2, 2.20, etc.), this resolves to the
                # release branch's upstream tip and the diff stays scoped.
                # Falling back blindly to master here would surface a huge
                # unrelated fileset on release branches.
                try:
                    current_branch = _git("symbolic-ref", "--short", "HEAD")
                except subprocess.CalledProcessError:
                    current_branch = ""
                candidates: list[str] = []
                if current_branch:
                    candidates.append(f"{canonical}/{current_branch}")
                candidates.append(f"{canonical}/master")
                # Dedup so we don't probe <canonical>/master twice when
                # we're on the master branch.
                for candidate in dict.fromkeys(candidates):
                    try:
                        _git("rev-parse", "--verify", "--quiet", candidate)
                        fallback = candidate
                        print(f"[lint] @{{upstream}} not set; using '{fallback}' as base "
                              "(pass --rev to override)", file=sys.stderr)
                        break
                    except subprocess.CalledProcessError:
                        continue
                # Refuse a master fallback when the current branch *name*
                # looks like a release branch but no matching upstream ref
                # exists -- comparing such a branch against master would be
                # the "huge unrelated fileset" foot-gun the docstring warns
                # about.
                if (fallback == f"{canonical}/master"
                        and current_branch
                        and _looks_like_release_branch(current_branch)):
                    sys.exit(f"[lint] on release branch '{current_branch}' but no "
                             f"matching '{canonical}/{current_branch}' ref; pass "
                             f"--rev {canonical}/{current_branch} explicitly")
            if not fallback:
                sys.exit("[lint] no @{upstream} configured for current branch and no remote "
                         f"pointing at {_CANONICAL_REPO_HINT}; pass --rev <base> explicitly")
    resolved = _git("rev-parse", "--symbolic-full-name", base)
    print(f"[lint] comparing against {base} ({resolved})", file=sys.stderr)
    # base...HEAD needs a merge base, and git refuses the whole diff when the two share no
    # common ancestor -- an unrelated history, such as a vendored-upstream remote. Report
    # that rather than letting the diff fail as an uncaught CalledProcessError.
    try:
        _git("merge-base", base, "HEAD")
    except subprocess.CalledProcessError:
        sys.exit(f"[lint] {base!r} has no common ancestor with HEAD -- unrelated histories. "
                 "Pass --rev a ref on this branch's history.")
    committed = set(_git("diff", "--name-only", "--diff-filter=d", f"{base}...HEAD").splitlines())
    uncommitted = set(_git("diff", "--name-only", "--diff-filter=d", "HEAD").splitlines())
    untracked = set(_git("ls-files", "--others", "--exclude-standard").splitlines())
    return sorted(p for p in (committed | uncommitted | untracked) if p)


def _load_config() -> dict:
    with open(REPO_ROOT / ".lint", encoding="utf-8") as f:
        return json.load(f)


def _normalize_path(path: str) -> str:
    p = Path(path)
    if p.is_absolute():
        try:
            p = p.relative_to(REPO_ROOT)
        except ValueError:
            return str(p)
    return str(p)


# --- Linter runners --------------------------------------------------------------------------


def run_text(linter: Linter, files: list[str]) -> list[Message]:
    """Built-in text linter: line length, trailing whitespace, EOF newline, tab characters."""
    max_len = int(linter.config.get("text.max-line-length", 80))
    # severity keys are the arclint integer levels: 0=disabled, 1=advice, 2=warning, 3=error.
    severity = linter.config.get("severity", {})
    msgs: list[Message] = []
    for path in files:
        try:
            with open(REPO_ROOT / path, "rb") as f:
                data = f.read()
        except OSError as exc:
            msgs.append(Message(path, None, "error", "read", str(exc), linter.name))
            continue
        if data and not data.endswith(b"\n") and severity.get("3") != "disabled":
            msgs.append(Message(path, None, "warning", "file-no-newline",
                                "File does not end with newline", linter.name))
        for i, raw in enumerate(data.splitlines(), start=1):
            try:
                line = raw.decode("utf-8")
            except UnicodeDecodeError:
                continue
            if line.rstrip() != line and severity.get("2") != "disabled":
                msgs.append(Message(path, i, "warning", "trailing-whitespace",
                                    "Trailing whitespace", linter.name))
            if len(line) > max_len and severity.get("2") != "disabled":
                msgs.append(Message(path, i, "warning", "line-too-long",
                                    f"Line longer than {max_len} chars ({len(line)})",
                                    linter.name))
            if b"\t" in raw and severity.get("2") != "disabled":
                msgs.append(Message(path, i, "advice", "tab-literal",
                                    "Tab character in source", linter.name))
    return msgs


def run_pep8(linter: Linter, files: list[str]) -> list[Message]:
    """Run pycodestyle (a.k.a. pep8) on ``files``."""
    binary = linter.config.get("bin", "pycodestyle")
    flags = linter.config.get("flags", [])
    msgs: list[Message] = []
    if not shutil.which(binary):
        print(f"[lint] skipping {linter.name}: {binary} not found", file=sys.stderr)
        return msgs
    proc = subprocess.run([binary, *flags, *files], cwd=REPO_ROOT, text=True,
                          capture_output=True, check=False)
    pat = re.compile(r"^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+):\s+(?P<code>\S+)\s+(?P<msg>.*)$")
    for line in proc.stdout.splitlines():
        m = pat.match(line)
        if m:
            msgs.append(Message(m["path"], int(m["line"]), "warning", m["code"], m["msg"],
                                linter.name))
        else:
            print(line, file=sys.stderr)
    return msgs


def _cpplint_filter_arg(severity_rules: dict) -> str:
    # Keys in .lint are regex patterns matched against cpplint rule names (e.g.
    # "(^build/c[+][+]11$)"); strip the regex wrapping and unescape single-char classes so we
    # can pass bare rule names (e.g. "build/c++11") to cpplint's --filter flag.
    disabled = [
        re.sub(r"\[(.)\]", r"\1", rule.strip("()^$"))
        for rule, sev in severity_rules.items()
        if sev == "disabled"
    ]
    return ",".join(f"-{d}" for d in disabled) if disabled else ""


def run_cpplint(linter: Linter, files: list[str]) -> list[Message]:
    """Run Google's cpplint.py on ``files``."""
    binary = CPPLINT_PATH
    if not os.access(binary, os.X_OK):
        print(f"[lint] skipping {linter.name}: cpplint.py not executable at {binary}",
              file=sys.stderr)
        return []
    max_len = int(linter.config.get("cpplint.max_line_length", 80))
    header_root = linter.config.get("cpplint.header_guard_root_dir", "")
    filter_arg = _cpplint_filter_arg(linter.config.get("severity.rules", {}))
    cmd = [binary, f"--linelength={max_len}"]
    if header_root:
        cmd.append(f"--root={header_root}")
    if filter_arg:
        cmd.append(f"--filter={filter_arg}")
    cmd.extend(files)
    proc = subprocess.run(cmd, cwd=REPO_ROOT, text=True, capture_output=True, check=False)
    pat = re.compile(
        r"^(?P<path>.+?):(?P<line>\d+):\s+(?P<msg>.+?)\s+\[(?P<code>[^\]]+)\]\s+\[\d+\]$")
    msgs: list[Message] = []
    for line in proc.stderr.splitlines():
        if line.startswith("Done processing") or line.startswith("Total errors"):
            continue
        m = pat.match(line)
        if m:
            msgs.append(Message(m["path"], int(m["line"]), "error", m["code"], m["msg"],
                                linter.name))
        else:
            print(line, file=sys.stderr)
    return msgs


def run_golint(linter: Linter, files: list[str]) -> list[Message]:
    """Run ``golint`` on ``files``."""
    binary = shutil.which("golint")
    if not binary:
        print(f"[lint] skipping {linter.name}: golint not found", file=sys.stderr)
        return []
    proc = subprocess.run([binary, *files], cwd=REPO_ROOT, text=True, capture_output=True,
                          check=False)
    pat = re.compile(r"^(?P<path>.+?):(?P<line>\d+):\d+:\s+(?P<msg>.*)$")
    msgs: list[Message] = []
    for line in proc.stdout.splitlines():
        m = pat.match(line)
        if m:
            msgs.append(Message(m["path"], int(m["line"]), "warning", "golint", m["msg"],
                                linter.name))
    return msgs


def _parse_perl_regex(raw_regex: str) -> re.Pattern:
    """Convert a Perl-style ``/pattern/flags`` string (as used in .lint) into a Python pattern."""
    delim = raw_regex[0]
    end = raw_regex.rfind(delim)
    body = raw_regex[1:end]
    flags_str = raw_regex[end + 1:]
    re_flags = 0
    if "i" in flags_str:
        re_flags |= re.IGNORECASE
    if "s" in flags_str:
        re_flags |= re.DOTALL
    if "m" in flags_str:
        re_flags |= re.MULTILINE
    return re.compile(body, re_flags)


def run_script_and_regex(linter: Linter, files: list[str]) -> list[Message]:
    """Run a shell script per file and parse its output with the configured regex."""
    script = linter.config["script-and-regex.script"]
    pat = _parse_perl_regex(linter.config["script-and-regex.regex"])
    msgs: list[Message] = []
    for path in files:
        proc = subprocess.run(["bash", "-c", f"{script} \"$0\"", path], cwd=REPO_ROOT,
                              text=True, capture_output=True, check=False)
        output = (proc.stdout or "") + (proc.stderr or "")
        if pat.flags & re.MULTILINE:
            matches = list(pat.finditer(output))
        else:
            m = pat.search(output)
            matches = [m] if m else []
        for m in matches:
            gd = m.groupdict()
            line = gd.get("line")
            msgs.append(Message(
                path,
                int(line) if line and line.isdigit() else None,
                gd.get("severity", "warning"),
                gd.get("name", linter.name),
                gd.get("message", output.strip()),
                linter.name,
            ))
    return msgs


RUNNERS = {
    "text": run_text,
    "pep8": run_pep8,
    "googlecpplint": run_cpplint,
    "golint": run_golint,
    "script-and-regex": run_script_and_regex,
}


# --- Main ------------------------------------------------------------------------------------


def _resolve_files(args: argparse.Namespace) -> list[str]:
    if args.everything:
        return subprocess.check_output(
            ["git", "ls-files"], cwd=REPO_ROOT, text=True).splitlines()
    if args.paths:
        return [_normalize_path(p) for p in args.paths]
    return _changed_files(args.rev)


def main() -> int:
    """Entry point: parse args, load config, run linters, print findings."""
    ap = argparse.ArgumentParser(description="YugabyteDB linter.")
    ap.add_argument("paths", nargs="*", help="Files to lint. If omitted, uses changed files.")
    ap.add_argument("--everything", action="store_true",
                    help="Lint all tracked files (ignores paths).")
    ap.add_argument("--rev", default=None,
                    help="Git ref to diff against when selecting changed files "
                         "(default: @{upstream}; required if @{upstream} is not set).")
    ap.add_argument("--only", action="append", default=[],
                    help="Only run linters whose name matches (repeatable).")
    ap.add_argument("--list-linters", action="store_true",
                    help="List configured linters and exit.")
    args = ap.parse_args()

    cfg = _load_config()
    global_excludes = _compile_patterns(cfg.get("exclude", []))
    linters = [Linter.build(name, lc, global_excludes)
               for name, lc in cfg.get("linters", {}).items()]

    if args.list_linters:
        for linter in linters:
            print(f"{linter.name}\t{linter.config.get('type')}")
        return 0

    files = _resolve_files(args)
    if not files:
        print("No files to lint.")
        return 0

    all_msgs: list[Message] = []
    for linter in linters:
        if args.only and not any(flt in linter.name for flt in args.only):
            continue
        runner = RUNNERS.get(linter.config.get("type"))
        if runner is None:
            continue
        matched = [f for f in files if linter.matches(f)]
        if not matched:
            continue
        try:
            all_msgs.extend(runner(linter, matched))
        except Exception as exc:  # pylint: disable=broad-exception-caught
            print(f"[lint] {linter.name} failed: {exc}", file=sys.stderr)
            traceback.print_exc()

    for m in all_msgs:
        print(m.format())

    errors = sum(1 for m in all_msgs if m.severity.lower() == "error")
    warnings = sum(1 for m in all_msgs if m.severity.lower() == "warning")
    print(f"\n{errors} error(s), {warnings} warning(s), {len(all_msgs)} total message(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
