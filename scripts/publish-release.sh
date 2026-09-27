#!/usr/bin/env bash
set -euo pipefail

version="${1:?usage: scripts/publish-release.sh <version>}"
tag="v${version#v}"

if ! git rev-parse --verify --quiet "refs/tags/$tag" >/dev/null; then
  git tag -a "$tag" -m "OrbitLan $tag"
fi

git push origin "$tag"
printf 'Triggered the GitHub release workflow for %s\n' "$tag"
