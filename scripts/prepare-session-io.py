#!/usr/bin/env python3
"""Stage pinned Zig Threaded sources in an isolated P0 engine; never edit Zig/vendor."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.16.0'
THREADED_SHA256 = 'eb7bbb4ddf590ec3d0d2a1ee5c3845ef4984d9e440965a3e7c920cd0c906df94'
TEST_SHA256 = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
LICENSE_SHA256 = '5c537d6853e005298a285d508cff9ac7192cea23576c840d485b2b586a7ff177'


def prepare(engine):
    engine = engine.resolve()
    if not engine.is_relative_to(ROOT / 'tmp'):
        raise RuntimeError('generated engine must be under project tmp/')
    zig = (os.environ.get('ZIG') or shutil.which('zig') or
           str(ROOT / 'tmp/zig/zig-aarch64-macos-0.16.0/zig'))
    result = subprocess.run([zig, 'env'], cwd=ROOT, capture_output=True, text=True,
                            check=True, timeout=10)
    def field(name):
        match = re.search(r'\.' + name + r' = ("(?:\\.|[^"\\])*"),', result.stdout)
        if not match:
            raise RuntimeError('missing Zig environment field: ' + name)
        return json.loads(match.group(1))
    if field('version') != VERSION:
        raise RuntimeError('Zig 0.16.0 required')
    std = Path(field('std_dir'))
    if not std.is_absolute():
        std = ROOT / std
    sources = ((std / 'Io/Threaded.zig', engine / 'src/lib/p0_threaded_io.zig', THREADED_SHA256),
               (std / 'Io/Threaded/test.zig', engine / 'src/lib/Threaded/test.zig', TEST_SHA256),
               (ROOT / 'patches/zig/0.16.0/LICENSE', engine / 'src/lib/p0_threaded_io.LICENSE', LICENSE_SHA256))
    staged = []
    for source, output, digest in sources:
        if output.is_symlink() or not output.resolve().is_relative_to(engine):
            raise RuntimeError('generated source path escapes isolated engine: ' + str(output))
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise RuntimeError('pinned Zig source SHA256 mismatch: ' + str(source))
        if output.exists() and output.read_bytes() != data:
            raise RuntimeError('refusing to overwrite modified generated source: ' + str(output))
        staged.append((output, data))
    for output, data in staged:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', type=Path)
    args = parser.parse_args()
    prepare(args.engine)
