# SPDX-License-Identifier: MPL-2.0
"""Prune the repository's GitHub Actions caches to the newest entry per family.

GitHub gives each repository a single ~10 GB Actions cache budget shared across
*all* workflows and branches, and LRU-evicts (by last-accessed) above it. PJ4's
rolling per-SHA cache keys (``ccache-linux-main-<sha>``,
``sccache-windows-...-<sha>``, ``conan-...-<hash>``) write a fresh large blob on
every run, so the budget fills with stale duplicates and starts evicting the
infrequently-accessed Windows sccache -> cold 50-95 min compiles.

Since every consumer restores via ``restore-keys`` prefix (always the most
recent match), only the newest entry per (ref, key-family) is useful; the rest
are dead weight. This script deletes them.

It groups caches by (git ref, key-family), where the family is the key with its
trailing rolling suffix (``-<7..64 hex>``) stripped, then keeps the newest
``--keep`` per group and deletes the older ones via the REST API. Version- or
content-stable keys (Qt, pre-commit) have no hex suffix, form singleton
families, and are therefore never touched.

Dry-run by default; pass ``--apply`` to actually delete. Auth and transport go
through the ``gh`` CLI (``GH_TOKEN`` in CI), so no token handling lives here.
"""

import argparse
import json
import os
import re
import subprocess
import sys

# Rolling per-SHA / per-conanfile-hash suffix appended to a stable key stem.
_ROLLING_SUFFIX = re.compile(r"-[0-9a-f]{7,64}$")


def _family(key: str) -> str:
    """Key stem shared by every rolling variant (suffix stripped)."""
    return _ROLLING_SUFFIX.sub("", key)


def _gh_api(repo: str, path: str, method: str | None = None) -> str:
    args = ["gh", "api"]
    if method:
        args += ["--method", method]
    args.append(f"/repos/{repo}/actions/{path}")
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def _list_caches(repo: str) -> list[dict]:
    """All cache entries, following pagination."""
    caches: list[dict] = []
    page = 1
    while True:
        batch = json.loads(_gh_api(repo, f"caches?per_page=100&page={page}"))["actions_caches"]
        caches += batch
        if len(batch) < 100:
            return caches
        page += 1


def prune(repo: str, keep: int, apply: bool) -> int:
    """Delete all but the newest ``keep`` caches per (ref, family). Returns bytes freed."""
    caches = _list_caches(repo)
    groups: dict[tuple[str, str], list[dict]] = {}
    for cache in caches:
        groups.setdefault((cache["ref"], _family(cache["key"])), []).append(cache)

    freed = 0
    for (ref, family), items in sorted(groups.items()):
        items.sort(key=lambda c: c["created_at"], reverse=True)  # newest first
        for stale in items[keep:]:
            verb = "deleting" if apply else "would delete"
            print(f"{verb} {stale['size_in_bytes'] / 1e9:6.2f} GB  {ref}  {stale['key']}")
            if apply:
                _gh_api(repo, f"caches/{stale['id']}", method="DELETE")
            freed += stale["size_in_bytes"]

    total = sum(c["size_in_bytes"] for c in caches)
    mode = "freed" if apply else "would free"
    print(f"\n{len(caches)} caches, {total / 1e9:.2f} GB total; "
          f"{mode} {freed / 1e9:.2f} GB -> {(total - freed) / 1e9:.2f} GB remaining")
    return freed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=os.environ.get("REPO") or os.environ.get("GITHUB_REPOSITORY"),
                        help="owner/name (defaults to $REPO / $GITHUB_REPOSITORY)")
    parser.add_argument("--keep", type=int, default=1, help="newest entries to keep per family (default 1)")
    parser.add_argument("--apply", action="store_true", help="actually delete (default: dry-run)")
    args = parser.parse_args()
    if not args.repo:
        parser.error("no repository: pass --repo or set $REPO / $GITHUB_REPOSITORY")
    prune(args.repo, args.keep, args.apply)
    return 0


if __name__ == "__main__":
    sys.exit(main())
