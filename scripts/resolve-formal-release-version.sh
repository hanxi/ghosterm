#!/usr/bin/env bash
# Resolve the one tag that both platform builds must use for this release.
set -euo pipefail

version="${1:-}"
version_pattern='^(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})$'
if [[ "$GITHUB_REF" == refs/tags/v* ]]; then
  version="${GITHUB_REF#refs/tags/v}"
elif [[ -z "$version" ]]; then
  tags="$(git tag --points-at "$GITHUB_SHA" --list 'v[0-9]*')"
  while IFS= read -r tag; do
    candidate="${tag#v}"
    if [[ "$candidate" =~ $version_pattern ]]; then
      if [[ -n "$version" ]]; then
        echo "Multiple formal release tags identify the build commit; specify a version" >&2
        exit 1
      fi
      version="$candidate"
    fi
  done <<< "$tags"
  if [[ -z "$version" ]]; then
    echo "No formal release tag identifies the build commit" >&2
    exit 1
  fi
fi

if ! [[ "$version" =~ $version_pattern ]]; then
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
