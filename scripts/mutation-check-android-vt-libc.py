#!/usr/bin/env python3
"""Allocator-symbol guard: real Android archives, isolated mutations, independent control."""
import hashlib
import importlib.util
import os
from pathlib import Path
import signal
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_VT_LIBC_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    source = ROOT / 'scripts/build-android-native.py'
    original = source.read_bytes()
    digest = hashlib.sha256(original).digest()
    if MARK.encode() in original: raise RuntimeError('mutation marker present')
    sdk = Path(os.environ.get('ANDROID_HOME', ROOT / 'build/android-toolchain/sdk'))
    host = 'darwin-x86_64' if os.uname().sysname == 'Darwin' else 'linux-x86_64'
    tools = sdk / 'ndk/28.2.13676358/toolchains/llvm/prebuilt' / host / 'bin'
    nm = tools / 'llvm-nm'
    archive = ROOT / 'build/android-native/arm64-v8a/lib/libghostty-vt.a'
    if not archive.is_file(): raise RuntimeError('build Android native dependencies first')
    with tempfile.TemporaryDirectory(prefix='android-vt-libc-', dir=ROOT / 'tmp') as directory:
        work = Path(directory)
        def load(text):
            scope = {'__file__': str(source), '__name__': 'isolated_android_builder'}
            exec(compile(text, str(source), 'exec'), scope)
            return scope['verify_vt_libc']
        verify = load(original)
        verify(archive, nm)
        fixtures = {}
        for name, text in {
            'clean': 'int fixture(void) { return 1; }',
            'calloc': '#include <stddef.h>\nvoid *calloc(size_t n, size_t s) { (void)n; (void)s; return 0; }',
            'free': 'void free(void *p) { (void)p; }',
        }.items():
            c, obj, lib = (work / (name + suffix) for suffix in ('.c', '.o', '.a'))
            c.write_text(text)
            helper.require(helper.run([tools / 'aarch64-linux-android29-clang', '-Werror', '-c', c, '-o', obj]))
            helper.require(helper.run([tools / 'llvm-ar', 'rcs', lib, obj]))
            fixtures[name] = lib
        def reject(function, name):
            try: function(fixtures[name], nm)
            except RuntimeError as error:
                if 'VT archive defines libc allocator stubs' in str(error): return
                raise
            raise AssertionError(f'allocator guard accepted {name} stub')
        for name in ('calloc', 'free'): reject(verify, name)
        verify(fixtures['clean'], nm)
        for index, name in enumerate(('calloc', 'free'), 1):
            before = "{'calloc', 'free'} & set(symbols.splitlines())"
            after = ("{'free'}" if name == 'calloc' else "{'calloc'}") + ' & set(symbols.splitlines())'
            text = original.decode()
            if text.count(before) != 1: raise RuntimeError('missing/ambiguous mutation anchor')
            changed = text.replace(before, after) + '\n# ' + MARK + '\n'
            mutated = load(changed)
            try: reject(mutated, name)
            except AssertionError as error: print(f'L{index} target red: {error}', flush=True)
            else: raise RuntimeError('mutation did not make target assertion red')
            mutated(fixtures['clean'], nm)
            reject(load(original), name)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('source SHA256 changed')
            print(f'L{index}: independent green / restored green / SHA256 identical', flush=True)
    print('ANDROID VT LIBC MUTATIONS PASS: 2 groups')


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
