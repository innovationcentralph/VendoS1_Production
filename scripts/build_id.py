"""
PlatformIO pre-build hook: stamp the binary with the commit it was built from.

WHY THIS EXISTS

`AT+VERSION?` returned a hardcoded "1.0.0-esp32" from the first commit
(2026-09-08) until 2026-10-16, unchanged across every behavioural change in
between. A board therefore could not say what it was running: the only way to
know was to ask whoever flashed it. The app image's own descriptor is no help
either -- under Arduino/PlatformIO `esp_app_desc_t.version` carries the
*framework's* build stamp (arduino-lib-builder, Mar 5 2024), not this sketch's.

OTA makes that a correctness problem rather than an inconvenience. "Is this
board out of date?" has to be answerable from the board, or the server is
guessing which image to send.

WHAT IT INJECTS

    FW_BUILD_ID     short commit sha, plus "-dirty" when the tree has
                    uncommitted changes, e.g. "a1b2c3d" or "a1b2c3d-dirty"
    FW_BUILD_EPOCH  build time, UTC seconds

The dirty flag matters more than it looks: a bench build with uncommitted
changes is NOT the commit it claims to be, and two different images answering
with the same id is exactly the confusion this is meant to end. Builds without
git (a tarball, a CI checkout with no .git) report "nogit" rather than failing
the build -- an unidentifiable build is bad, an unbuildable one is worse.

Note for release builds: the sha identifies the source, not the release. The
semver in include/version.h is still bumped by hand -- see that file.
"""

import subprocess
import time

Import("env")


def _git(*args):
    """Run a git command, or return None if git/the repo is unavailable."""
    try:
        out = subprocess.check_output(
            ["git"] + list(args),
            stderr=subprocess.DEVNULL,
            universal_newlines=True,
        )
        return out.strip()
    except (subprocess.CalledProcessError, OSError):
        return None


def build_id():
    sha = _git("rev-parse", "--short=7", "HEAD")
    if not sha:
        return "nogit"
    # --porcelain prints one line per changed path and nothing at all when the
    # tree is clean, so any output means dirty. Untracked files are excluded
    # (-uno): a stray log or editor backup next to the source does not change
    # what was compiled, and flagging those would make every build look dirty.
    dirty = _git("status", "--porcelain", "-uno")
    return sha + "-dirty" if dirty else sha


bid = build_id()
env.Append(CPPDEFINES=[
    ("FW_BUILD_ID", env.StringifyMacro(bid)),
    ("FW_BUILD_EPOCH", str(int(time.time()))),
])

print("build id: %s" % bid)
