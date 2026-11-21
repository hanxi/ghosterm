#!/usr/bin/env bash
# Resolve the one tag that both platform builds must use for this release.
set -euo pipefail

version="${1:-}"
if [[ "$GITHUB_REF" == refs/tags/v* ]]; then
  version="${GITHUB_REF#refs/tags/v}"
elif [[ -z "$version" ]]; then
  version="$(git describe --tags --abbrev=0 --match 'v[0-9]*')"
  version="${version#v}"
fi

if ! [[ "$version" =~ ^(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})$ ]]; then
  echo "Invalid formal release version: $version" >&2
  exit 1
fi
IFS=. read -r major minor patch <<< "$version"
if ((major * 1000000 + minor * 1000 + patch + 2 > 2100000000)); then
  echo "Formal release version exceeds the Android versionCode range" >&2
  exit 1
fi
if [[ "$(git rev-parse --verify "refs/tags/v$version^{commit}")" != "$GITHUB_SHA" ]]; then
  echo "Formal release tag v$version does not identify the build commit" >&2
  exit 1
fi
printf '%s\n' "$version"
