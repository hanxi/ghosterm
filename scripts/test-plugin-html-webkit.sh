#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/tmp/plugin-html-webkit-probe"
mkdir -p "${ROOT}/tmp"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}/tool-tmp"
cleanup() {
  python3 - "${BUILD_DIR}" <<'PY'
from pathlib import Path
import shutil
import sys
shutil.rmtree(Path(sys.argv[1]), ignore_errors=True)
PY
}
trap cleanup EXIT

cat > "${BUILD_DIR}/ModelStubs.swift" <<'SWIFT'
import Foundation

public enum ExtensionController {
    public struct ReceivedResource: Sendable {
        public let resourceID: String
        public let sha256: String
        public let mime: String
        public let displayName: String
        public let size: UInt64
        public let bytes: Data
        public init(resourceID: String, sha256: String, mime: String, displayName: String, size: UInt64, bytes: Data) {
            self.resourceID = resourceID
            self.sha256 = sha256
            self.mime = mime
            self.displayName = displayName
            self.size = size
            self.bytes = bytes
        }
    }
}
SWIFT

cd "${ROOT}"
TMPDIR="${BUILD_DIR}/tool-tmp" swiftc -emit-library -static -emit-module \
  -module-name GhostermAppModel \
  -emit-module-path "${BUILD_DIR}/GhostermAppModel.swiftmodule" \
  -o "${BUILD_DIR}/libGhostermAppModel.a" \
  "${BUILD_DIR}/ModelStubs.swift" \
  apple/GhostermApp/Sources/GhostermAppModel/PluginResourceHTMLDecoder.swift \
  apple/GhostermApp/Sources/GhostermAppModel/PluginResourceMarkdownDecoder.swift
TMPDIR="${BUILD_DIR}/tool-tmp" swiftc -parse-as-library \
  -I "${BUILD_DIR}" -L "${BUILD_DIR}" -lGhostermAppModel \
  -framework AppKit -framework WebKit -framework ImageIO \
  -o "${BUILD_DIR}/probe" \
  apple/GhostermApp/Sources/GhostermApp/PluginResourceHTMLAssets.swift \
  scripts/plugin-html-webkit-probe.swift
DYLD_LIBRARY_PATH="${BUILD_DIR}" TMPDIR="${BUILD_DIR}/tool-tmp" "${BUILD_DIR}/probe"
