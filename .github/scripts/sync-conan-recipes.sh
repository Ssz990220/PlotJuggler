#!/usr/bin/env bash

# A JFrog local Conan repository does not proxy ConanCenter metadata. Refresh
# the recipe revisions used by the current graph before resolving binaries from
# JFrog, otherwise an older mirrored recipe can win by remote priority and
# conflict with a newer direct requirement (for example assimp/5.4.3's draco
# requirement).

set -euo pipefail

if [[ "${PJ_USE_JFROG:-false}" != "true" ]]; then
  echo "Skipping Conan recipe sync: JFrog is not enabled"
  exit 0
fi

# A committed lockfile pins every recipe revision, which makes revision
# refreshing pointless — and actively breaks: `conan graph info .` auto-loads
# ./conan.lock in strict mode and fails on platform-only tool requires the
# other platform's lock leg never captured (e.g. strawberryperl on MSVC).
if [[ -f conan.lock ]]; then
  echo "Skipping Conan recipe sync: conan.lock pins recipe revisions"
  exit 0
fi

sync_home="$(mktemp -d)"
graph_file="${sync_home}/graph.json"
local_recipe_list="${sync_home}/local-recipes.json"
remote_recipe_list="${sync_home}/remote-recipes.json"

echo "Refreshing current dependency recipe revisions from ConanCenter"
CONAN_HOME="$sync_home" conan profile detect --force >/dev/null
CONAN_HOME="$sync_home" conan graph info . \
  -r=conancenter \
  --update \
  "$@" \
  --format=json \
  --out-file="$graph_file" \
  -vwarning

# The clean home contains exactly the latest recipes selected for this graph.
# Export that set as Conan's native PackageList so the real job cache can fetch
# recipe metadata without requiring a matching binary to exist on ConanCenter.
CONAN_HOME="$sync_home" conan list "*#latest" \
  --format=json \
  --out-file="$local_recipe_list" \
  -vwarning

# `conan list` names its source "Local Cache", while `conan download -r`
# expects the matching remote name at the PackageList root. Keep the original
# list for upload and rewrite only that first root key for download. Both Linux
# and Windows jobs execute this with Git's GNU sed.
sed '0,/"Local Cache"/s//"conancenter"/' \
  "$local_recipe_list" > "$remote_recipe_list"

conan download \
  --list="$remote_recipe_list" \
  -r=conancenter \
  --only-recipe \
  -vwarning

# graph info retrieves recipes but never package binaries. Upload only each
# graph-selected recipe revision here; the normal post-install upload remains
# responsible for platform-specific binaries. Existing revisions are
# checksum-addressed and skipped by Conan.
echo "Publishing current recipe revisions to JFrog"
conan upload \
  --list="$local_recipe_list" \
  -r=plotjuggler-conan \
  --only-recipe \
  --confirm \
  -cc core.upload:parallel=4
