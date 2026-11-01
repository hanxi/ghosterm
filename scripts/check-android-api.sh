#!/usr/bin/env bash
# Check each Android module: app-only lint does not check dependency source APIs.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec bash "$ROOT/scripts/build-android.sh" \
  :app:lintRelease :bridge:lintRelease :data:lintRelease :terminal:lintRelease
