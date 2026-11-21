#!/usr/bin/env bash
# The public policy directory contains only the three signed channel inputs.
set -euo pipefail

if [[ "$#" != 1 || ! -d "$1" || -L "$1" ]]; then
  echo "Expected a regular plugin channel directory" >&2
  exit 2
fi
directory="${1%/}"
shopt -s dotglob nullglob
entries=("$directory"/*)
if [[ "${#entries[@]}" != 3 ]]; then
  echo "Plugin channel directory must contain exactly three files" >&2
  exit 1
fi
for name in channel-policy.json channel-policy.sig policy.json; do
  if [[ ! -f "$directory/$name" || -L "$directory/$name" ]]; then
    echo "Missing regular plugin channel file: $name" >&2
    exit 1
  fi
done
