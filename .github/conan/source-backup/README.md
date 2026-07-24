# Conan source backup

Repo-hosted mirror for third-party **source tarballs** whose upstream host is
unreachable from GitHub Actions runners. CI copies this directory into Conan's
source **download cache** (`core.sources:download_cache`), so a
`conan install --build=missing` is served locally and never depends on a
third-party download host being reachable from a datacenter IP.
(Serving it over HTTP via `core.sources:download_urls` +
`raw.githubusercontent.com` does not work: this repo is private, so
unauthenticated raw fetches return 404.)

## Why this exists

`minizip/1.2.13` (a hard, unconditional requirement of `assimp`, which pj_scene3D
uses for URDF mesh loading) has no source of its own: its Conan recipe downloads
**zlib's** source tarball (minizip lives in zlib's `contrib/minizip/`) from a
single URL, `https://zlib.net/fossils/zlib-1.2.13.tar.gz` — and zlib.net rejects
GitHub-runner IPs with HTTP 415. Any cold Windows CI build was therefore a
lottery: runs could die mid-`conan install` before the missing binary package
was available in the PlotJuggler Conan repository.

The recipe for zlib itself carries a GitHub mirror URL and never fails; the
minizip recipe simply lacks one. An upstream PR adding the mirror to
conan-center's minizip recipe accompanies this change — **once that merges and
the pinned recipe revision picks it up, this directory can be retired.**

## Contract

- Each backed-up source is stored as **two files**, exactly as Conan's source
  download-cache layout expects:
  - `<sha256>` — the tarball itself, named by the sha256 of its contents. This
    is the same checksum the recipe's `conandata.yml` pins, and Conan re-verifies
    it on every use — a corrupted or tampered file can never be consumed.
  - `<sha256>.json` — Conan's metadata sidecar (`{"references": {...}}`). Without
    it Conan treats the blob as missing. Generate it by letting Conan download
    the source once with `core.sources:download_cache=<dir>` set, then copy the
    pair out of `<dir>/s/`.
- CI wires it up in the "Seed Conan source download cache" step of
  `windows-ci.yml`: copy both files into `<download_cache>/s/` and append
  `core.sources:download_cache=<dir>` to `global.conf`. A seeded source is
  served locally; anything not seeded falls through to the recipe's own URLs
  as usual. The `<dir>/s/` layout is Conan-internal but observed stable across
  Conan 2.x; if it ever changes, the seed simply misses and CI degrades to the
  old (network-dependent) behavior rather than breaking.

## Current contents

| File (sha256) | What it is | Consumed by |
|---|---|---|
| `b3a24de97a8fdb…` | `zlib-1.2.13.tar.gz` (byte-identical to the official release; also mirrored at `github.com/madler/zlib/releases/download/v1.2.13/`) | `minizip/1.2.13` ← `assimp` |

## Adding a new file

```bash
# 1. Fetch the tarball from any trustworthy mirror.
curl -LO <mirror-url>/<tarball>
# 2. Its sha256 MUST match the one pinned in the recipe's conandata.yml.
sha256sum <tarball>
# 3. Store it under that sha256 name, generate the .json via Conan's download
#    cache (see Contract above), commit both, and add a row to the table.
```
