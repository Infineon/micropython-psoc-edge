# This scripts supports the versioning of the MicroPython PSOC Edge
# The port is already present in MicroPython upstream.
# Before, the versioning scheme of this fork has been independent.
# Now, it is important to align the versioning with the upstream MicroPython scheme.

# ---------------------------------------------------------------------------------

# The proposed versioning scheme he is the following:

# <git-describe(master)>-ifx-pse.v<fork-version>

# For example:

# v1.29.0-15-gde7364ab-ifx-pse.v1.1.0 → The fork's master is 15 commits past v1.29.0.
# v1.30.0-ifx-pse.v1.1.0              → The fork's master is exactly at v1.30.0.

# This way we can track the differences between upstream and fork, and clearly identify the latest version. The latest version will always have the highest number of the fork prefix (if progress).
# The git-describe value identifies the upstream release and synchronization commit used by the fork.

# ---------------------------------------------------------------------------------

# Usage: next-version.py <ifx-pse-version>
#
# The local "master" branch is assumed to mirror the upstream micropython/micropython
# main branch. Its git-describe value is used as the upstream portion of the version.
# Upstream tags are fetched first because they may not be present locally.

import argparse
import re
import subprocess
import sys

UPSTREAM_MIRROR_BRANCH = "master"
UPSTREAM_REMOTE = "upstream"
UPSTREAM_URL = "https://github.com/micropython/micropython.git"


def run_git(*args):
    result = subprocess.run(["git", *args], capture_output=True, text=True, check=True)
    return result.stdout.strip()


def branch_exists(branch):
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet", f"{branch}^{{commit}}"],
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def get_described_version(branch):
    tags = run_git("tag", "--list", "--merged", branch).splitlines()
    stable_tags = [tag for tag in tags if re.fullmatch(r"v\d+\.\d+\.\d+", tag)]
    if not stable_tags:
        sys.exit(f"error: no stable release tags found reachable from '{branch}'")

    describe_args = ["describe", "--tags", "--long"]
    for tag in stable_tags:
        describe_args.extend(("--match", tag))
    describe_args.append(branch)
    return run_git(*describe_args)


def ensure_upstream_remote(remote):
    result = subprocess.run(["git", "remote", "get-url", remote], capture_output=True, text=True)
    if result.returncode == 0:
        return

    try:
        run_git("remote", "add", remote, UPSTREAM_URL)
    except subprocess.CalledProcessError as error:
        message = error.stderr.strip() or f"git remote exited with status {error.returncode}"
        sys.exit(f"error: could not configure upstream remote '{remote}': {message}")


def fetch_upstream_tags(remote):
    try:
        run_git("fetch", "--quiet", remote, "--tags")
    except subprocess.CalledProcessError as error:
        message = error.stderr.strip() or f"git fetch exited with status {error.returncode}"
        sys.exit(f"error: could not fetch tags from remote '{remote}': {message}")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Compute the next MicroPython PSOC Edge fork version."
    )
    parser.add_argument("ifx_pse_version", help="The ifx-pse fork tag to release, e.g. v1.1.0")
    parser.add_argument(
        "--master",
        default=UPSTREAM_MIRROR_BRANCH,
        help="Local branch synchronized with upstream (default: %(default)s)",
    )
    parser.add_argument(
        "--remote",
        default=UPSTREAM_REMOTE,
        help="Git remote containing upstream tags (default: %(default)s)",
    )
    args = parser.parse_args()
    if not re.match(r"^v\d+\.\d+\.\d+$", args.ifx_pse_version):
        sys.exit(f"error: ifx-pse-version '{args.ifx_pse_version}' must look like vX.Y.Z")
    return args


def main():
    args = parse_args()
    ensure_upstream_remote(args.remote)
    fetch_upstream_tags(args.remote)
    mirror_branch = args.master
    if not branch_exists(mirror_branch):
        mirror_branch = f"origin/{mirror_branch}"
    if not branch_exists(mirror_branch):
        sys.exit(f"error: synchronized branch '{args.master}' not found locally")
    described_version = get_described_version(mirror_branch)
    version = f"{described_version}-ifx-pse.{args.ifx_pse_version}"
    print(version)


if __name__ == "__main__":
    main()
