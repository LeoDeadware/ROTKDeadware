#!/usr/bin/env python3
"""Pre-publish gate (RULES R-L1/R-L2): what goes to GitHub — the tracked tree, optionally every
blob in the history, and release assets — holds no secrets, no game data or anything derived
from it, nothing personal, no vendored third-party code; LICENSE is there and sizes are sane.

  tools/publish_check.py [--repo DIR] [--history] [--private] [--release] [ASSET.zip|DIR|FILE ...]

--release: also the owner's release rules — one app with both languages, Ukrainian and English,
chosen in the launcher, and the README says so; a prebuilt app (.dmg / .app among the assets) ships
unsigned, so the README carries the one-line Gatekeeper workaround (FAIL if not).

--private: the repo is private (game data / personal findings become WARN; secrets stay FAIL).
Exceptions: `.publish-allow` in the repo root, one glob per line + `# reason` (a line without a
reason is ignored); a glob matching the path or the reported location (`assets.zip:*` = everything
in that release asset) turns WARN/FAIL into ALLOW, never for secrets. Exit 1 on any FAIL. Stdlib only (runs in CI).
"""
import argparse
import fnmatch
import os
import re
import subprocess
import sys
import zipfile

SECRETS = [
    ("GitHub token", r"\b(gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})"),
    ("Anthropic key", r"\bsk-ant-[A-Za-z0-9_-]{20,}"),
    ("OpenAI-style key", r"\bsk-(proj-)?[A-Za-z0-9]{32,}"),
    ("AWS key id", r"\bAKIA[0-9A-Z]{16}\b"),
    ("private key", r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
    ("Slack token", r"\bxox[abpr]-[A-Za-z0-9-]{10,}"),
    ("Google API key", r"\bAIza[0-9A-Za-z_-]{35}\b"),
    ("GitLab token", r"\bglpat-[A-Za-z0-9_-]{20}\b"),
    ("npm token", r"\bnpm_[A-Za-z0-9]{36}\b"),
    ("password/token literal", r"(?i)\b(password|passwd|api[_-]?key|secret|access[_-]?token)\b[\"']?\s*[:=]\s*[\"'][^\"'\s$<{]{8,}[\"']"),
]
SECRETS = [(n, re.compile(p)) for n, p in SECRETS]
SECRET_FILES = re.compile(r"(^|/)(\.env(\..+)?|id_(rsa|dsa|ecdsa|ed25519)|.+\.(pem|p12|pfx|key|keychain|mobileprovision)"
                          r"|\.netrc|\.npmrc|\.pypirc|credentials(\.json)?|.*service[-_]account.*\.json)$", re.I)
FAKE = re.compile(r"(?i)(example|dummy|fake|placeholder|xxxx|abcdefghij|your[_-]?(key|token))")

# Game data and what is derived from it (images, audio, video, archives, executables, fonts, Ghidra).
GAME_EXT = {"xbe", "iso", "xiso", "flat", "exe", "dll", "bin", "dat", "pak", "h4r", "h4d", "xpr", "xwb", "xsb",
            "rfs", "bik", "wma", "wav", "mp3", "ogg", "flac", "xma", "adpcm", "dds", "tga", "bmp", "png", "jpg",
            "jpeg", "gif", "webp", "mp4", "mov", "ttf", "otf", "fnt", "gpr", "rep", "rgba"}
GAME_DIRS = re.compile(r"(^|/)(game|extracted|generated|art|voice/raw|recon/ghidra|mods/hd_[^/]*)/")
# our placeholders and build files inside those dirs (ROTK lift/game/CMakeLists.txt): not game data
BUILD_FILES = re.compile(r"(^|/)(\.gitkeep|CMakeLists\.txt)$")
VENDOR_DIRS = re.compile(r"(^|/)(third_party|vendor|external|node_modules)/")
ORIGINAL_TEXT_COL = {"en", "english", "orig", "original", "source"}
PERSONAL = [("home path", re.compile(r"/Users/(?!Shared/)[A-Za-z][\w.-]*/")),
            ("local e-mail", re.compile(r"[\w.+-]+@[\w-]+\.local\b"))]
PIPE_SHELL = re.compile(r"\b(curl|wget)\b[^|\n]*\|\s*(sudo\s+)?(ba|z)?sh\b")
UNPINNED_ACTION = re.compile(r"^\s*-?\s*uses:\s*([^@\s]+)@(?![0-9a-f]{40}\b)(\S+)", re.M)

WARN_MB, FAIL_MB, TOTAL_FAIL_MB = 2, 20, 100
LICENSES = ("LICENSE", "LICENSE.md", "LICENSE.txt", "COPYING")
# README must name both versions (in either language) and must not say one of them is missing.
LANG_UK = re.compile(r"(?i)(ukrainian|українськ\w*)")
LANG_EN = re.compile(r"(?i)(\benglish\b|англійськ\w*)")
LANG_SWITCH = re.compile(r"(?i)(?=.*(launcher|лаунчер))(?=.*(language|мов[аиуі]\b|мову|мовою))")
ONE_VERSION = re.compile(r"(?i)(no english (option|version)|only in ukrainian|ukrainian[- ]only|"
                         r"(англійськ\w*|української) (верс\w* )?нема|лише українськ\w*|тільки українськ\w*)")
GATEKEEPER = re.compile(r"(?i)(gatekeeper|right-click\W+(it\W+)?.{0,20}open|control-click|open anyway|com\.apple\.quarantine"
                        r"|правий клік|правою кнопкою|«відкрити»|все одно відкрити)")


class Report:
    def __init__(self, private, allow):
        self.private, self.allow, self.rows = private, allow, []

    def add(self, level, cat, where, msg, path=None):
        if level != "FAIL!" and any(fnmatch.fnmatch(path or "", g) or fnmatch.fnmatch(where, g) for g in self.allow):
            level = "ALLOW"
        if level == "GAME":
            level = "WARN" if self.private else "FAIL"
        self.rows.append((level.rstrip("!"), cat, where, msg))

    def print(self, limit=12):
        order = {"FAIL": 0, "WARN": 1, "ALLOW": 2}
        seen = {}
        for level, cat, where, msg in sorted(self.rows, key=lambda r: (order[r[0]], r[1], r[2])):
            k = (level, cat)
            seen[k] = seen.get(k, 0) + 1
            if seen[k] <= limit:
                print(f"{level:5}  [{cat}] {where}  {msg}")
            elif seen[k] == limit + 1:
                n = sum(1 for r in self.rows if (r[0], r[1]) == k) - limit
                print(f"{level:5}  [{cat}] ... and {n} more")
        fails = sum(1 for r in self.rows if r[0] == "FAIL")
        warns = sum(1 for r in self.rows if r[0] == "WARN")
        print(f"publish_check: {'FAIL' if fails else 'PASS'} ({fails} fail, {warns} warn, "
              f"{'private' if self.private else 'public'} repo)")
        return 1 if fails else 0


def mask(s):
    return s[:6] + "…" if len(s) > 6 else "…"


def scan_text(rep, where, path, text, personal=True, workflow=False):
    for i, line in enumerate(text.splitlines(), 1):
        if len(line) > 4000:
            continue
        for name, rx in SECRETS:
            for m in rx.finditer(line):
                if not FAKE.search(m.group(0)):
                    rep.add("FAIL!", "secret", f"{where}:{i}", f"{name} {mask(m.group(0))}")
        if personal:
            for name, rx in PERSONAL:
                m = rx.search(line)
                if m:
                    rep.add("GAME", "personal", f"{where}:{i}", f"{name} {m.group(0)}", path)
        if PIPE_SHELL.search(line):
            rep.add("FAIL", "download", f"{where}:{i}", "pipes a download into a shell (pin + checksum it)", path)
    if workflow:
        for m in UNPINNED_ACTION.finditer(text):
            if not m.group(1).startswith("./"):
                rep.add("FAIL", "download", where, f"action {m.group(1)}@{m.group(2)} not pinned to a commit SHA", path)
    first = text.split("\n", 1)[0]
    if path.endswith((".tsv", ".csv")):
        cols = {c.strip().lower() for c in re.split(r"[\t,]", first)}
        if cols & ORIGINAL_TEXT_COL:
            rep.add("GAME", "game-text", where, f"column {sorted(cols & ORIGINAL_TEXT_COL)[0]!r}: original game text?", path)


def check_path(rep, where, path, size):
    if SECRET_FILES.search(path):
        rep.add("FAIL!", "secret", where, "credential-like file name")
    ext = path.rsplit(".", 1)[-1].lower() if "." in os.path.basename(path) else ""
    if ext in GAME_EXT:
        rep.add("GAME", "game-data", where, f".{ext}: game data or derived (image/audio/binary)", path)
    elif GAME_DIRS.search(path) and not BUILD_FILES.search(path):
        rep.add("GAME", "game-data", where, "under a game-data directory", path)
    if VENDOR_DIRS.search(path):
        rep.add("FAIL", "third-party", where, "vendored third-party code: fetch it at a pinned commit instead", path)
    mb = size / 1e6
    if mb > FAIL_MB:
        rep.add("FAIL", "size", where, f"{mb:.1f} MB > {FAIL_MB} MB", path)
    elif mb > WARN_MB:
        rep.add("WARN", "size", where, f"{mb:.1f} MB", path)


def is_text(data):
    return b"\0" not in data[:8000]


def git(repo, *args, data=None):
    return subprocess.run(["git", "-C", repo, *args], input=data, capture_output=True, check=True).stdout


def check_tree(rep, repo):
    files = [f for f in git(repo, "ls-files", "-z").decode().split("\0") if f]
    total = 0
    for f in files:
        p = os.path.join(repo, f)
        if not os.path.isfile(p) or os.path.islink(p):
            continue
        size = os.path.getsize(p)
        total += size
        check_path(rep, f, f, size)
        if size < 5_000_000:
            data = open(p, "rb").read()
            if is_text(data):
                scan_text(rep, f, f, data.decode("utf-8", "replace"),
                          workflow=f.startswith(".github/workflows/"))
    if total / 1e6 > TOTAL_FAIL_MB:
        rep.add("FAIL", "size", "(tree)", f"{total / 1e6:.0f} MB tracked > {TOTAL_FAIL_MB} MB")
    if not any(os.path.isfile(os.path.join(repo, n)) for n in LICENSES):
        rep.add("GAME", "license", "(root)", "no LICENSE file")
    return len(files)


def check_history(rep, repo):
    """Every blob reachable from any ref: names for game data, text for secrets; authors' e-mails."""
    objs = git(repo, "rev-list", "--objects", "--all").decode().splitlines()
    named = {}
    for line in objs:
        sha, _, name = line.partition(" ")
        if name:
            named.setdefault(sha, name)
    head = set(git(repo, "ls-files", "-z").decode().split("\0"))
    meta = git(repo, "cat-file", "--batch-check=%(objectname) %(objecttype) %(objectsize)",
               data="\n".join(named).encode()).decode().splitlines()
    want = []
    for line in meta:
        sha, typ, size = line.split()
        if typ != "blob":
            continue
        name, size = named[sha], int(size)
        if name not in head:  # HEAD's files were checked as the tree
            check_path(rep, f"{name}@{sha[:8]}", name, size)
        if size < 2_000_000:
            want.append(sha)
    out = git(repo, "cat-file", "--batch", data="\n".join(want).encode())
    i = 0
    while i < len(out):
        nl = out.index(b"\n", i)
        sha, _, size = out[i:nl].decode().split()
        size = int(size)
        blob = out[nl + 1:nl + 1 + size]
        i = nl + 2 + size
        if is_text(blob):
            for name, rx in SECRETS:
                for m in rx.finditer(blob.decode("utf-8", "replace")):
                    if not FAKE.search(m.group(0)):
                        rep.add("FAIL!", "secret", f"{named[sha]}@{sha[:8]}", f"{name} {mask(m.group(0))} (in history)")
    for e in sorted(set(git(repo, "log", "--all", "--format=%ae%n%ce").decode().split())):
        if not (e.endswith("users.noreply.github.com") or e == "noreply@github.com"):   # GitHub's own merge commits
            rep.add("GAME", "personal", "(history)", f"commit e-mail {e} (use the GitHub noreply address)")
    return len(want)


def check_asset(rep, path):
    if os.path.isdir(path):
        for d, _, files in os.walk(path):
            for f in files:
                p = os.path.join(d, f)
                rel = os.path.relpath(p, path)
                check_path(rep, f"{os.path.basename(path)}/{rel}", rel, os.path.getsize(p))
        return
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                where = f"{os.path.basename(path)}:{info.filename}"
                check_path(rep, where, info.filename, info.file_size)
                if info.file_size < 2_000_000:
                    data = z.read(info)
                    if is_text(data):
                        scan_text(rep, where, info.filename, data.decode("utf-8", "replace"))
        return
    check_path(rep, os.path.basename(path), os.path.basename(path), os.path.getsize(path))


def check_release(rep, repo, assets=()):
    """Owner's rules: one app, Ukrainian + English chosen in the launcher (2026-11-05); unsigned
    prebuilt apps get a one-line Gatekeeper workaround in the README (2026-11-06)."""
    p = next((os.path.join(repo, n) for n in ("README.md", "README") if os.path.isfile(os.path.join(repo, n))), None)
    if not p:
        rep.add("FAIL", "release", "(root)", "no README")
        return
    text = open(p, encoding="utf-8").read()
    if not re.search(r"[А-Яа-яІіЇїЄєҐґ]{4}", text):
        rep.add("FAIL", "release", "README", "no Ukrainian text")
    if not re.search(r"\b(the|and|game)\b", text):
        rep.add("FAIL", "release", "README", "no English text")
    for name, rx in (("Ukrainian", LANG_UK), ("English", LANG_EN)):
        if not rx.search(text):
            rep.add("FAIL", "release", "README", f"does not name {name} as a language of the game")
    if not LANG_SWITCH.search(text):
        rep.add("FAIL", "release", "README", "does not say the language is chosen in the launcher")
    for i, line in enumerate(text.splitlines(), 1):
        m = ONE_VERSION.search(line)
        if m:
            rep.add("FAIL", "release", f"README:{i}", f"says one language is missing: {m.group(0)!r}")
    if any(ships_app(a) for a in assets) and not GATEKEEPER.search(text):
        rep.add("FAIL", "release", "README", "prebuilt unsigned app: no one-line Gatekeeper workaround")


def ships_app(path):
    if path.endswith((".dmg", ".app", ".app/")):
        return True
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            return any(".app/" in n for n in z.namelist())
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", default=".")
    ap.add_argument("--history", action="store_true", help="also every blob and author in the git history")
    ap.add_argument("--private", action="store_true", help="private repo: game data / personal only WARN")
    ap.add_argument("--release", action="store_true", help="also the release rules: one app, UA + EN in the launcher; Gatekeeper line")
    ap.add_argument("assets", nargs="*", help="release assets (zip, folder, file) to check too")
    a = ap.parse_args()
    repo = os.path.abspath(a.repo)
    allow = []
    p = os.path.join(repo, ".publish-allow")
    if os.path.exists(p):
        rows = [l.split("#", 1) for l in open(p, encoding="utf-8") if not l.lstrip().startswith("#")]
        allow = [r[0].strip() for r in rows if len(r) == 2 and r[0].strip() and r[1].strip()]
    rep = Report(a.private, allow)
    n = check_tree(rep, repo)
    print(f"tree: {n} tracked files")
    if a.history:
        print(f"history: {check_history(rep, repo)} blobs scanned")
    if a.release:
        check_release(rep, repo, a.assets)
    for asset in a.assets:
        check_asset(rep, asset)
    return rep.print()


if __name__ == "__main__":
    sys.exit(main())
