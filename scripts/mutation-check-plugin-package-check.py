#!/usr/bin/env python3
"""Exercise the Lua package checker with isolated compiled mutations."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BACKEND = 'src/extension/package-check-lua.inc'
FACADE = 'src/extension/package-check.c'
CASES = {
    'hash-required': (BACKEND,
        '!equals(doc, gt__bridge_wire_member(doc, record, "sha256"), member.sha256)',
        'false', 'hash-mismatch'),
    'catalog-size': (BACKEND, 'record, "bytes", member.bytes)',
        'record, "bytes", member.bytes + 1)', 'compatible'),
    'catalog-path': (BACKEND, 'gt__package_snapshot_find(snapshot, path, &index) != GT_PACKAGE_OK || seen[index]',
        'gt__package_snapshot_find(snapshot, "integrity.json", &index) != GT_PACKAGE_OK || seen[index]', 'compatible'),
    'source-consumed': (BACKEND, '        *snapshot = NULL;\n',
        '        /* Keep the source pointer. */\n', 'compatible'),
    'cancel-before-commit': (BACKEND, 'if (config->stop && atomic_load(config->stop)) {',
        'if (false && config->stop && atomic_load(config->stop)) {', 'late-cancel'),
    'package-digest': (BACKEND, 'memcpy(digest, integrity.sha256, 65);',
        "memset(digest, '0', 64);\n    digest[64] = 0;", 'compatible'),
    'unknown-not-unsupported': (BACKEND, 'GT_PACKAGE_CHECK_UNKNOWN, "HOST_REQUIRED"',
        'GT_PACKAGE_CHECK_UNSUPPORTED, "HOST_REQUIRED"', 'unknown-host'),
    'failure-info-wiped': (FACADE,
        'gt_package_check_info_t *out) {\n    if (out) {\n        memset(out, 0, sizeof(*out));\n    }',
        'gt_package_check_info_t *out) {', 'hash-mismatch'),
}
PUBLIC_CASES = {
    'capture-cancel': ('src/extension/package-api.c',
        '    if (p->info.phase != GT_EXT_PACKAGE_CAPTURING) {\n'
        '        return p->info.phase;\n    }\n',
        '    if (p->info.phase != GT_EXT_PACKAGE_CAPTURING) {\n'
        '        return p->info.phase;\n    }\n    atomic_store(&p->stopped, false);\n',
        'capture-stop'),
    'check-cancel': ('src/extension/package-api.c',
        '    if (p->info.phase != GT_EXT_PACKAGE_CAPTURED) {\n'
        '        return p->info.phase;\n    }\n',
        '    if (p->info.phase != GT_EXT_PACKAGE_CAPTURED) {\n'
        '        return p->info.phase;\n    }\n    atomic_store(&p->stopped, false);\n',
        'check-stop'),
    'public-unknown': ('src/extension/package-api.c', 'error = GT_EXT_PACKAGE_UNKNOWN;',
        'error = GT_EXT_PACKAGE_UNSUPPORTED;', 'unknown'),
    'public-digest': ('src/extension/package-api.c', 'COPY(package_digest);',
        'COPY(manifest_digest);', 'roundtrip'),
    'metadata-zero': ('src/extension/package-api.c',
        '    if (out) {\n        memset(out, 0, sizeof(*out));\n    }\n'
        '    if (!p || !out || p->info.phase != GT_EXT_PACKAGE_CHECKED)',
        '    if (!p || !out || p->info.phase != GT_EXT_PACKAGE_CHECKED)', 'roundtrip'),
    'stopped-adoption': ('src/extension/runtime-api.c',
        'if (atomic_load_explicit(&package->stopped, memory_order_relaxed))',
        'if (false && atomic_load_explicit(&package->stopped, memory_order_relaxed))', 'load-stop'),
    'admission-id': ('src/extension/session.c', '!strcmp(s->config.plugin_id, id) &&',
        '(!strcmp(s->config.plugin_id, id) || true) &&', 'wrong-id'),
    'admission-version': ('src/extension/session.c', '!strcmp(s->config.plugin_version, version) &&',
        '(!strcmp(s->config.plugin_version, version) || true) &&', 'wrong-version'),
    'admission-digest': ('src/extension/session.c',
        '!strcmp(role == GT_BRIDGE_HOST ? s->config.client_digest : s->config.executor_digest,\n'
        '                   digest) &&',
        'true &&', 'mismatch'),
    'admission-association': ('src/extension/package-runtime.c',
        '(info.association_required && !associated)', '(false && info.association_required && !associated)',
        'association'),
    'api-source-unknown': ('src/extension/registry-api.c', 'return GT_EXT_REGISTRY_UNKNOWN;',
        'return GT_EXT_REGISTRY_DENIED;', 'registry'),
    'api-source-denied': ('src/extension/registry-api.c', 'return GT_EXT_REGISTRY_DENIED;',
        'return GT_EXT_REGISTRY_UNKNOWN;', 'registry'),
    'api-stopped-put': ('src/extension/registry-api.c',
        'if (atomic_load_explicit(&p->stopped, memory_order_relaxed))',
        'if (false && atomic_load_explicit(&p->stopped, memory_order_relaxed))', 'registry'),
    'api-independent-stop': ('src/extension/registry-api.c', 'atomic_init(&p->stopped, false);',
        'atomic_init(&p->stopped, true);', 'registry'),
    'api-selected-stats': ('src/extension/registry-api.c', 'p->info.members = count;',
        'p->info.members = 0;', 'registry'),
    'api-command-schema': ('src/extension/registry-api.c',
        'memcpy(out->args_schema, c->args_schema, sizeof(c->args_schema));',
        'memset(out->args_schema, 0, sizeof(out->args_schema));', 'registry'),
    'api-command-zero': ('src/extension/registry-api.c',
        '    if (out) {\n        memset(out, 0, sizeof(*out));\n    }\n'
        '    if (!p || !out || p->info.phase != GT_EXT_PACKAGE_CHECKED)',
        '    if (!p || !out || p->info.phase != GT_EXT_PACKAGE_CHECKED)', 'registry'),
}
REGISTRY = 'src/extension/registry.c'
REGISTRY_CASES = {
    'registry-unknown': (REGISTRY, 'return GT_REGISTRY_UNKNOWN;', 'return GT_REGISTRY_DENIED;', 'registry'),
    'registry-denied': (REGISTRY, 'return GT_REGISTRY_DENIED;', 'return GT_REGISTRY_UNKNOWN;', 'registry'),
    'registry-conflict': (REGISTRY, 'if (!id && overlaps > 1)', 'if (false && !id && overlaps > 1)', 'registry'),
    'registry-replace-digest': (REGISTRY, 'if (strcmp(entry->digest, expected))',
        'if (false && strcmp(entry->digest, expected))', 'registry'),
    'registry-remove-digest': (REGISTRY, 'if (strcmp(e->digest, expected))',
        'if (false && strcmp(e->digest, expected))', 'registry'),
    'registry-selection-digest': (REGISTRY, 'if (strcmp(digest, e->digest))',
        'if (false && strcmp(digest, e->digest))', 'registry'),
    'registry-side': (REGISTRY, 'if (!e->package || e->executor != executor)',
        'if (!e->package || (false && e->executor != executor))', 'registry'),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lua-archive', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=sorted(CASES.keys() | PUBLIC_CASES.keys() | REGISTRY_CASES.keys()))
    profile = parser.add_mutually_exclusive_group()
    profile.add_argument('--public', action='store_true', help='verify public package and runtime entry points')
    profile.add_argument('--registry', action='store_true', help='verify internal package registry resolution')
    args = parser.parse_args()
    archive = args.lua_archive.resolve(strict=True)
    cases = REGISTRY_CASES if args.registry else PUBLIC_CASES if args.public else CASES
    names = args.case or cases
    if set(names) - cases.keys():
        parser.error('selected cases do not match the requested package profile')
    target = 'test_plugin_registry' if args.registry else 'test_ext_package' if args.public else 'test_package_check'
    probe_source = ROOT / f'core/tests/{target}.c'
    core = ROOT / 'core'
    originals = {name: (core / name).read_bytes() for name, *_ in cases.values()}
    inputs = {path: sha(path) for path in [archive, probe_source, *(core / name for name in originals)]}
    with tempfile.TemporaryDirectory(prefix='package-check-', dir=os.environ['TMPDIR']) as scratch:
        work = Path(scratch)
        source = work / 'core'
        build = work / 'build'
        env = {**os.environ, 'TMPDIR': scratch, 'TMP': scratch, 'TEMP': scratch,
               'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
               'UBSAN_OPTIONS': 'halt_on_error=1'}
        shutil.copytree(core, source, ignore=shutil.ignore_patterns('build', 'build-*'))
        (work / 'patches').symlink_to(ROOT / 'patches', target_is_directory=True)
        (work / 'tmp').mkdir()

        def run(command, diagnostic=None):
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=180)
            output = result.stdout + result.stderr
            if diagnostic:
                if (result.returncode != 1 or len(re.findall(diagnostic, output, re.M)) != 1 or
                        any(item in output for item in ('AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:'))):
                    raise RuntimeError(output)
            elif result.returncode:
                raise RuntimeError(output)

        def compile_core(filename=None):
            if filename:
                unit = BACKEND.removesuffix('-lua.inc') + '.c' if filename == BACKEND else filename
                (build / 'CMakeFiles/ghosterm_core.dir' / (unit + '.o')).unlink(missing_ok=True)
            (build / 'libghosterm_core.a').unlink(missing_ok=True)
            (build / target).unlink(missing_ok=True)
            run(['cmake', '--build', build, '-j4', '--target', target, 'test_config'])
            return sha(build / target)

        def probe(case=None, diagnostic=None):
            if args.registry:
                run([build/target, work], diagnostic)
                return
            if args.public:
                command = [build/target, case, work] if case else [
                    'ctest', '--test-dir', build, '--no-tests=error', '-R', '^lua_package_',
                    '-E', '_internal$', '--output-on-failure']
                run(command, diagnostic)
                return
            command = ['python3', '-B', ROOT/'scripts/test-plugin-package-native.py',
                       '--binary', build/'test_package_check']
            command += ['--case', case] if case else ['--oom-sweep']
            run(command, diagnostic)

        run(['cmake', '-S', source, '-B', build, '-DGHOSTERM_BUILD_TESTS=ON',
             '-DGHOSTERM_WITH_PLUGIN_LUA=ON', '-DGHOSTERM_WITH_LIBSSH2=OFF',
             f'-DGHOSTERM_LUA_ARCHIVE={archive}', '-DCMAKE_BUILD_TYPE=Debug',
             '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all'])
        compile_core()
        probe()
        for name in names:
            filename, old, new, case = cases[name]
            path = source / filename
            original = originals[filename]
            assert original.decode().count(old) == 1, name
            baseline = sha(build / target)
            try:
                path.write_text(original.decode().replace(old, new, 1))
                assert compile_core(filename) != baseline, name
                probe(case, r'^FAIL main:' if args.public or args.registry else r'^FAIL probe:')
                run([build / 'test_config', ROOT / 'core/tests/fixtures'])
            finally:
                path.write_bytes(original)
            compile_core(filename)
            probe(case)
            assert sha(path) == inputs[core / filename], name
            print(f'PASS {name}: compiled assertion / Core control / restore / SHA256', flush=True)
        probe()
        assert all(sha(path) == digest for path, digest in inputs.items())
        print(f'LUA PACKAGE CHECK MUTATIONS PASS: {len(names)} groups', flush=True)


if __name__ == '__main__':
    main()
