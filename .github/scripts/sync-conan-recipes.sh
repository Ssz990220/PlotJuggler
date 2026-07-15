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

graph_file="${RUNNER_TEMP:-/tmp}/pj4-conan-recipe-graph.json"

echo "Refreshing current dependency recipe revisions from ConanCenter"
conan graph info . \
  -r=conancenter \
  --update \
  "$@" \
  --format=json \
  --out-file="$graph_file" \
  -vwarning

# graph info retrieves recipes but never package binaries. Upload only each
# locally-latest recipe revision here; the normal post-install upload remains
# responsible for the platform-specific binaries. Existing revisions are
# checksum-addressed and skipped by Conan.
echo "Publishing current recipe revisions to JFrog"
conan upload "*#latest" \
  -r=plotjuggler-conan \
  --only-recipe \
  --confirm \
  -cc core.upload:parallel=4
