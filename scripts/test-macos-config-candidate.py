#!/usr/bin/env python3
"""Compile and execute the native config-apply bookkeeping fixture under repo tmp/."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
scratch = ROOT / 'tmp'
scratch.mkdir(exist_ok=True)
env = {**os.environ, 'TMPDIR': str(scratch)}
frameworks = ['CoreFoundation', 'Foundation', 'AppKit', 'Metal', 'MetalKit', 'QuartzCore',
              'CoreText', 'CoreGraphics', 'IOKit', 'Carbon', 'UniformTypeIdentifiers',
              'UserNotifications', 'CoreServices', 'Security', 'IOSurface']
with tempfile.TemporaryDirectory(prefix='native-config-apply-', dir=scratch) as directory:
    output = Path(directory) / 'probe'
    sdk = ROOT / 'build/GhosttyKit.xcframework/macos-arm64'
    command = ['xcrun', '--sdk', 'macosx', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-I', str(ROOT / 'apple/GhostermTerminal/Sources/CGhostty/include'),
               '-I', str(sdk / 'Headers'), str(ROOT / 'scripts/probes/macos-config-candidate.c'),
               '-L', str(sdk), '-lghostty-internal', '-lc++', '-o', str(output)]
    for name in frameworks:
        command += ['-framework', name]
    subprocess.run(command, env=env, check=True)
    print('NATIVE_CONFIG_PROBE_COMPILED', flush=True)
    result = subprocess.run([str(output)], cwd=directory, env=env)
    raise SystemExit(result.returncode)
