#!/usr/bin/env python3
"""Render the pinned bootstrap and the byte-exact stable channel index."""
import argparse
import hashlib
from pathlib import Path
import re


PLATFORMS = ("macos-arm64", "linux-x86_64", "linux-arm64")
DOMAIN = b"GTERM-DISTRIBUTION-INDEX-V1\0"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("assets", type=Path)
    parser.add_argument("--platform", choices=PLATFORMS, action="append")
    args = parser.parse_args()
    if not re.fullmatch(r"gterm-v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", args.tag):
        parser.error("expected a stable gterm-vMAJOR.MINOR.PATCH tag")
    platforms = args.platform or PLATFORMS
    if len(set(platforms)) != len(platforms):
        parser.error("platforms must be unique")

    template = Path(__file__).with_name("install-gterm.sh").read_text()
    bootstrap = template.replace("@GTERM_VERSION@", args.tag)
    lines = [f"gterm-stable-v1 {args.tag}\n"]
    for platform in platforms:
        archive = args.assets / f"gterm-{platform}.tar.gz"
        checksum = hashlib.sha256()
        with archive.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                checksum.update(chunk)
        digest = checksum.hexdigest()
        bootstrap = bootstrap.replace(f"@{platform.upper().replace('-', '_')}@", digest)
        lines.append(f"{platform} {digest}\n")
    index = "".join(lines).encode("ascii")
    (args.assets / "install-gterm.sh").write_text(bootstrap)
    (args.assets / "index").write_bytes(index)
    (args.assets / "index-message").write_bytes(DOMAIN + index)


if __name__ == "__main__":
    main()
