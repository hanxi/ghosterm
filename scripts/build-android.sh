#!/usr/bin/env bash
# Build native dependencies and APK with a pinned Gradle wrapper.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/tmp/android-build"
export TMPDIR="$ROOT/tmp/android-build"
export JAVA_TOOL_OPTIONS="${JAVA_TOOL_OPTIONS:-} -Djava.io.tmpdir=$TMPDIR"
export ANDROID_HOME="${ANDROID_HOME:-$ROOT/build/android-toolchain/sdk}"
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$ROOT/build/android-toolchain/gradle-home}"
if [ -z "${JAVA_HOME:-}" ]; then
  # Optional local toolchain used by the initial implementation; CI supplies JAVA_HOME.
  JDK="$ROOT/build/android-toolchain/jdk-17.0.21+9/Contents/Home"
  if [ -x "$JDK/bin/java" ]; then export JAVA_HOME="$JDK"; fi
fi
python3 "$ROOT/scripts/check-abi-android.py"
python3 "$ROOT/scripts/build-android-native.py"
if [ "$#" -eq 0 ]; then set -- :app:assembleDebug :app:assembleDebugAndroidTest; fi
exec "$ROOT/android/gradlew" -p "$ROOT/android" "$@"
