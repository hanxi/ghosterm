#!/usr/bin/env python3
"""Compile owned CLI mutations; normal assertion red / same archive config /
restore green / SHA256. Run only in a dedicated source checkout under tmp/.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CAT = 'cli/gterm/catalog.c'
STORE = 'cli/gterm/catalog-store.c'


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    # The surrounding workspace may have parallel native app work. Never
    # patch it or rebuild its archives; this runner owns a detached checkout.
    assert '/tmp/' in str(ROOT) and (ROOT / '.git').is_file()
    build = args.build.resolve(); out = args.out.resolve()
    assert out.is_relative_to(ROOT / 'tmp') and build.is_relative_to(ROOT / 'tmp')
    out.mkdir(parents=True, exist_ok=True)
    cases = [
        ('revision', 'stale-revision', [(CAT, 'c.revision != expected || ', ''),
                                       (STORE, 'c->revision != expected || ', '')]),
        ('replace-digest', 'stale-replace', [(CAT, '!expected_digest || strcmp(c.entries[index].digest, expected_digest)', '!expected_digest')]),
        ('implicit-replace', 'duplicate', [(CAT, '!expected_digest || strcmp(c.entries[index].digest, expected_digest)', 'expected_digest && strcmp(c.entries[index].digest, expected_digest)')]),
        ('package-digest', 'bad-digest', [(CAT, 'if (strcmp(metadata->package_digest, entry->digest))', 'if (false && strcmp(metadata->package_digest, entry->digest))')]),
        ('launch-digest', 'digest-change', [(CAT, 'if (strcmp(metadata->package_digest, entry->digest))', 'if (false && strcmp(metadata->package_digest, entry->digest))')]),
        ('metadata-identity', 'identity', [(CAT, 'if (entry->id &&', 'if (false && entry->id &&')]),
        ('file-mode', 'data-mode', [(STORE, '(s->st_mode & 07777) == 0600 && ', '')]),
        ('file-links', 'data-hardlink', [(STORE, 's->st_nlink == 1', 'true')]),
        ('trailing-data', 'trailing', [(STORE, 'return b->at == b->size;', 'return true;')]),
        ('record-magic', 'magic', [(STORE, 'memcmp(b->bytes, magic, 8)', 'false')]),
        ('zero-revision', 'revision-zero', [(STORE, 'if (!c->revision)', 'if (false && !c->revision)')]),
        ('file-budget', 'catalog-size', [(STORE, 'c->original.st_size <= CATALOG_BYTES', 'true')]),
        ('profile-budget', 'profile-bytes', [(STORE, 'if (total > 32768)', 'if (false && total > 32768)')]),
        ('string-budget', 'string-limit', [(STORE, 'n > limit || ', '(n > limit && false) || ')]),
        ('utf8', 'bad-utf8', [(STORE, ' || !utf8(b->bytes + b->at, n)', '')]),
        ('file-sync', 'file-sync', [(STORE, ' && !fsync(fd)', '')]),
        ('directory-sync', 'directory-sync', [(STORE, 'return fsync(c->directory) ? 5 : 0;', 'return 0;')]),
        ('close-result', 'directory-close', [(CAT, 'if (!gterm_catalog_release(&c))', 'if (false && !gterm_catalog_release(&c))')]),
        ('fixed-root', 'relative-root', [(CAT, 'if (path && !gterm_catalog_path(path))', 'if (false && path && !gterm_catalog_path(path))')]),
        ('argument-schema', 'invalid-arguments', [(CAT, 'if (gt_ext_package_decode_command_arguments(selected,', 'if (false && gt_ext_package_decode_command_arguments(selected,')]),
        ('argv-boundary', 'multi-token', [(CAT, 'consumed = n;', 'consumed = 0;')]),
        ('explicit-conflict', 'conflict', [(CAT, 'if (matches != 1)', 'if (!matches)'),
          (CAT, 'registry, chosen, true, id, digest, &selected, &contribution',
           'registry, chosen, true, id ? id : c.entries[index].id, digest ? digest : c.entries[index].digest, &selected, &contribution')]),
        ('explicit-command', 'wrong-command', [(CAT, 'if (canonical && strcmp(canonical, contribution.path))', 'if (false && canonical && strcmp(canonical, contribution.path))')]),
        ('invocation-cwd', 'cwd-preview', [(CAT, 'args[4 + i] = ".";', 'args[4 + i] = e->profile[i + 2];')]),
    ]
    originals = {n: (ROOT / n).read_bytes() for n in [CAT, STORE]}
    for label, _test, changes in cases:
        for name, old, _new in changes:
            assert originals[name].decode().count(old) == 1, (label, old)
    names = set(subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=ROOT).decode().split('\0'))
    inputs = {n: sha(ROOT / n) for n in sorted(names) if n and n.startswith(('core/', 'cli/', 'scripts/', 'packages/')) and (ROOT / n).is_file() and not (ROOT / n).is_symlink()}
    archives = {str(p): sha(p) for p in [build / 'core/libghosterm_core.a', build / 'core/libghosterm_quickjs.a']}
    host = sha(build / 'test_gterm_stdio'); control = sha(build / 'core/test_config')
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    commands = []

    def run(label, command, red=False):
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require catalog; shift; gt_timeout 300 "$@"'
        process = subprocess.run(['bash', '-c', shell, 'catalog', str(ROOT), *map(str, command)],
                                 cwd=ROOT, env=env, capture_output=True, timeout=310)
        data = process.stdout + process.stderr
        (out / (label + '.log')).write_bytes(data)
        if red:
            assert process.returncode == 1 and data.count(b'ASSERTION: ') == 1 and process.stdout.startswith(b'ASSERTION: '), (label, data[-5000:])
            assert not any(s in data for s in [b'AddressSanitizer', b'runtime error:', b'Traceback', b'ABNORMAL']), (label, data[-5000:])
        else: assert process.returncode == 0, (label, data[-5000:])
        commands.append(dict(label=label, status=process.returncode, sha256=hashlib.sha256(data).hexdigest()))

    def test(label, case=None, red=False):
        run(label, ['python3', ROOT / 'scripts/test-gterm-catalog.py', '--build', build,
                    *(['--case', case] if case else ['--faults'])], red)

    def rebuild(label, files):
        for name in files:
            (build / 'CMakeFiles/gterm.dir' / (Path(name).name + '.o')).unlink(missing_ok=True)
        (build / 'gterm').unlink(missing_ok=True)
        run(label, ['cmake', '--build', build, '--target', 'gterm', '-j4'])

    rebuild('baseline-build', [*originals, 'cli/gterm/main.c', 'cli/gterm/stdio.c', 'cli/gterm/ipc.c'])
    test('baseline')
    for label, case, changes in cases:
        objects = {}
        for name, _old, _new in changes:
            obj = build / 'CMakeFiles/gterm.dir' / (Path(name).name + '.o')
            objects[obj] = sha(obj)
        try:
            for name in {n for n, _, _ in changes}:
                source = originals[name].decode()
                for filename, old, new in changes:
                    if filename == name: source = source.replace(old, new)
                (ROOT / name).write_text(source)
            rebuild(label + '-build', {name for name, _, _ in changes})
            assert all(sha(p) != before for p, before in objects.items()), label
            test(label + '-red', case, True)
            run(label + '-control', [build / 'core/test_config', ROOT / 'core/tests/fixtures'])
        finally:
            for name in {n for n, _, _ in changes}: (ROOT / name).write_bytes(originals[name])
        rebuild(label + '-restore-build', {name for name, _, _ in changes})
        test(label + '-restored', case)
        assert all((ROOT / n).read_bytes() == raw for n, raw in originals.items())
        assert all(sha(Path(n)) == digest for n, digest in archives.items())
        assert sha(build / 'test_gterm_stdio') == host and sha(build / 'core/test_config') == control
        print('PASS mutation ' + label, flush=True)
    test('final')
    assert all(sha(ROOT / n) == digest for n, digest in inputs.items())
    receipt = dict(scope='owned detached checkout; actual ordinary CLI/Native HOST; injected catalog IO failures; no model',
                   inputs=inputs, archives=archives, hostSHA256=host, controlSHA256=control,
                   binarySHA256=sha(build / 'gterm'), mutations=[c[0] for c in cases], commands=commands,
                   sanitizers=['address', 'undefined'], leakSanitizer=False)
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('CATALOG MUTATIONS PASS: ' + str(len(cases)), flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise KeyboardInterrupt(str(number))
    for number in (signal.SIGINT, signal.SIGTERM): signal.signal(number, interrupted)
    main()
