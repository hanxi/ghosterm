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
    parser.add_argument("--dev", action="store_true", help="render a rolling dev bootstrap")
    args = parser.parse_args()
    if args.dev:
        if not re.fullmatch(r"gterm-dev-[1-9][0-9]*-[1-9][0-9]*", args.tag):
            parser.error("expected a gterm-dev-RUN_ID-ATTEMPT build identifier")
    elif not re.fullmatch(r"gterm-v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", args.tag):
        parser.error("expected a stable gterm-vMAJOR.MINOR.PATCH tag")
    platforms = args.platform or PLATFORMS
    if len(set(platforms)) != len(platforms):
        parser.error("platforms must be unique")

    template = Path(__file__).with_name("install-gterm.sh").read_text()
    version = args.tag
    if args.dev:
        run_id, attempt = args.tag.removeprefix("gterm-dev-").split("-")
        if int(attempt) >= 1_000_000:
            parser.error("dev build attempt is out of range")
        # Older stable installers recognize this version and can replace it.
        version = f"gterm-v0.0.{run_id}{int(attempt):06d}"
        template = template.replace("bootstrap_source=''", "bootstrap_source='gterm-dev'")
    elif version.startswith("gterm-v0.0."):
        parser.error("gterm-v0.0.* is reserved for development builds")
    bootstrap = template.replace("@GTERM_VERSION@", version)
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
    (args.assets / "install-gterm.sh").write_text(bootstrap)
    if args.dev:
        return
    index = "".join(lines).encode("ascii")
    (args.assets / "index").write_bytes(index)
    (args.assets / "index-message").write_bytes(DOMAIN + index)


if __name__ == "__main__":
    main()
