#!/usr/bin/env python3
"""Run the native macOS channel client against an owned loopback TLS server."""
import base64
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('channel_tls_fixture', ROOT / 'scripts/test-plugin-channel-android-tls.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    with tempfile.TemporaryDirectory(dir=ROOT / 'tmp', prefix='macos-channel-tls-') as path:
        env = dict(os.environ, TMPDIR=path, GHOSTERM_AUTOMATION_OFFSCREEN='1')
        with helper.tls_fixture(Path(path), env, '127.0.0.1') as (origin, certificate, root):
            der = subprocess.check_output(['openssl', 'x509', '-in', certificate, '-outform', 'DER'], env=env)
            env.update(GHOSTERM_TEST_CHANNEL_ORIGIN=origin, GHOSTERM_TEST_CHANNEL_ROOT=root,
                       GHOSTERM_TEST_CHANNEL_CERT=base64.b64encode(der).decode(),
                       GHOSTERM_TEST_CHANNEL_RELEASE_VECTOR=str(helper.ASSETS / "vector.json"))
            result = subprocess.run(['swift', 'test', '--package-path', 'apple/GhostermApp',
                                     '--filter', 'PluginChannelTLSLoopbackTests'], cwd=ROOT, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
            print(result.stdout, end='')
            if result.returncode != 0 or 'Executed 1 test, with 0 failures' not in result.stdout:
                raise RuntimeError('macOS TLS test did not pass')


if __name__ == '__main__':
    main()
