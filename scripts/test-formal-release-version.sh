#!/usr/bin/env bash
# Exercise the release resolver without modifying the project's tags.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/tmp"
SCRATCH="$(cd "$ROOT/tmp" && pwd -P)"
if [[ -n "${TMPDIR:-}" && -d "$TMPDIR" ]]; then
  REQUESTED="$(cd "$TMPDIR" && pwd -P)"
  case "$REQUESTED" in
    "$SCRATCH"/*) SCRATCH="$REQUESTED" ;;
  esac
fi
FIXTURE="$(mktemp -d "$SCRATCH/formal-version-XXXXXX")"
trap 'rm -rf -- "$FIXTURE"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

cd "$FIXTURE"
git init -q --template=
git config --local user.name Fixture
git config --local user.email fixture@example.invalid
git config --local commit.gpgsign false
git config --local tag.gpgsign false
git config --local core.hooksPath /dev/null
git commit -q --allow-empty -m older
git tag v0.1.0
git commit -q --allow-empty -m current
export GITHUB_REF=refs/heads/main
GITHUB_SHA="$(git rev-parse HEAD)"
export GITHUB_SHA
RESOLVER="$ROOT/scripts/resolve-formal-release-version.sh"

reject() {
  local expected="$1" output
  shift
  if output="$(bash "$RESOLVER" "$@" 2>&1)" || [[ "$output" != *"$expected"* ]]; then
    echo "Expected release rejection: $expected" >&2
    echo "$output" >&2
    exit 1
  fi
}

reject 'No formal release tag identifies the build commit'
git tag v1.2.3-rc.1
reject 'No formal release tag identifies the build commit'
git tag -a -m release v1.2.3
[[ "$(bash "$RESOLVER")" == 1.2.3 ]]
git tag v1.2.4
reject 'Multiple formal release tags'
[[ "$(bash "$RESOLVER" 1.2.4)" == 1.2.4 ]]
[[ "$(GITHUB_REF=refs/tags/v1.2.3 bash "$RESOLVER")" == 1.2.3 ]]
reject 'does not identify the build commit' 0.1.0
git tag v2100.0.0
reject 'exceeds the Android versionCode range' 2100.0.0
echo 'Formal release version checks passed'
