#!/usr/bin/env python3
"""Native runtime catalog identity, integrity and bounded metadata."""
import hashlib, os, shutil, signal, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'tmp/session-runtime-catalog-mutations'
MARK = 'GT_SESSION_RUNTIME_CATALOG_MUTATION_MARK'
REL = Path('Sources/GhostermAppModel/SessionRuntimeCatalog.swift')

def run(args):
    p = subprocess.Popen([str(x) for x in args], cwd=ROOT, start_new_session=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        out, _ = p.communicate(timeout=180)
        return p.returncode, out
    finally:
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            try: p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL); p.wait()

def main():
    source = ROOT / 'apple/GhostermApp' / REL
    original = source.read_bytes(); digest = hashlib.sha256(original).digest()
    package = WORK / 'apple/GhostermApp'; copy = package / REL
    if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()): return 2
    def interrupted(sig, frame): raise RuntimeError('runtime catalog mutation interrupted: ' + str(sig))
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, interrupted)
    os.environ['TMPDIR'] = str(ROOT / 'tmp')
    os.environ['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
    if WORK.exists(): shutil.rmtree(WORK)
    (WORK / 'apple').mkdir(parents=True)
    for name in ('GhostermApp', 'GhostermCore', 'GhostermTerminal'):
        shutil.copytree(ROOT / 'apple' / name, WORK / 'apple' / name,
                        symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
    for name in ('core', 'terminal', 'build', 'assets'):
        (WORK / name).symlink_to(ROOT / name, target_is_directory=True)
    def restore():
        copy.write_bytes(original)
        if hashlib.sha256(copy.read_bytes()).digest() != digest or hashlib.sha256(source.read_bytes()).digest() != digest:
            raise RuntimeError('runtime catalog SHA256 rollback failed')
    def probe(test, red=False):
        rc, out = run(['xcrun', 'swift', 'test', '--package-path', package, '--filter', test])
        (WORK / ('last-red.log' if red else 'last-green.log')).write_text(out)
        if red:
            if rc != 1 or ('XCTAssert' not in out and ': failed - ' not in out) or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:
                raise RuntimeError('ordinary record assertion red required: ' + out[-6000:])
        elif rc or 'with 0 failures' not in out:
            raise RuntimeError('record baseline/control failed: ' + out[-6000:])
    specs = [
        ('future format accepted', 'table.format == 1', 'table.format >= 1', 'testFutureCatalogAndUnsafeReleaseRejected'),
        ('release path unchecked', 'token(table.local)', 'true', 'testFutureCatalogAndUnsafeReleaseRejected'),
        ('native digest ignored', 'actual == sha', 'true', 'testNativeDigestPermissionsAndSymlinkRejected'),
        ('native manifest subset accepted', 'Set(native.files.keys) == localFiles', 'Set(native.files.keys).isSubset(of: localFiles)', 'testNativeManifestFilesMatchExactly'),
        ('Linux contract unchecked', 'header[5] == Substring(table.contract)', 'true', 'testLinuxIdentityDigestAndDuplicateInventoryRejected'),
        ('Linux inventory duplicated', 'seen.insert(String(row[0])).inserted', 'true', 'testLinuxIdentityDigestAndDuplicateInventoryRejected'),
        ('catalog byte bound omitted', 'bytes(index, limit: 8192)', 'bytes(index, limit: 16384)', 'testBoundedMetadataPreservesOriginal'),
        ('payload symlink followed', '// avoid loading any runtime file as one UI-sized allocation.\n        let fd = Darwin.open(path.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)', '// avoid loading any runtime file as one UI-sized allocation.\n        let fd = Darwin.open(path.path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)', 'testNativeDigestPermissionsAndSymlinkRejected'),
    ]
    try:
        restore(); probe('SessionRuntimeCatalogTests')
        for label, old, new, test in specs:
            text = original.decode()
            if text.count(old) != 1: raise RuntimeError('record mutation anchor drift/' + label)
            try:
                copy.write_text(text.replace(old, new) + '\n// ' + MARK + '\n')
                probe('SessionRuntimeCatalogTests.' + test, red=True)
                probe('HostListTests.testReadingDoesNotMutateConfig')
                print('PASS runtime catalog mutation/' + label + ': ordinary red, same-mutant control green', flush=True)
            finally: restore()
        probe('SessionRuntimeCatalogTests')
        print('PASS 8 runtime catalog mutations and SHA256 rollback', flush=True)
    finally: restore()
    return 0
if __name__ == '__main__': sys.exit(main())
