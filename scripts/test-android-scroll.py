#!/usr/bin/env python3
"""Host protocol tests for Android scroll routing using the pinned, patched VT engine."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('android_native', ROOT / 'scripts/build-android-native.py')
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)
work = ROOT / 'tmp/android-scroll'
work.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, TMPDIR=str(work), ZIG_GLOBAL_CACHE_DIR=str(ROOT / 'tmp/zig-global'))
env.pop('TMUX', None)
zig = os.environ.get('ZIG') or shutil.which('zig') or str(ROOT / 'tmp/zig/zig-aarch64-macos-0.16.0/zig')
assert subprocess.check_output([zig, 'version'], text=True).strip() == '0.16.0'
assert subprocess.check_output(['git', '-C', ROOT / 'vendor/ghostty', 'rev-parse', 'HEAD'], text=True).strip() == native.PIN
inputs = {'pin': native.PIN, 'patches': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in native.VT_PATCHES},
          'build': 'ReleaseFast/simd=false/native/zig-0.16.0'}
receipt = work / 'receipt.json'
archive = work / 'out/lib/libghostty-vt.a'
cache = json.loads(receipt.read_text()) if receipt.exists() else {}
if cache.get('inputs') != inputs or not archive.exists() or cache.get('archive') != hashlib.sha256(archive.read_bytes()).hexdigest():
    source = work / 'source'
    if source.exists(): shutil.rmtree(source)
    source.mkdir()
    tar = work / 'source.tar'
    with tar.open('wb') as output:
        subprocess.run(['git', '-C', ROOT / 'vendor/ghostty', 'archive', native.PIN], stdout=output, check=True)
    with tarfile.open(tar) as package: package.extractall(source, filter='data')
    for patch in native.VT_PATCHES:
        subprocess.run(['patch', '--batch', '--forward', '-p1', '-i', patch], cwd=source, check=True)
    subprocess.run([zig, 'build', '-Demit-lib-vt=true', '-Demit-xcframework=false', '-Demit-docs=false',
                    '-Doptimize=ReleaseFast', '-Dsimd=false', '-p', work / 'out'], cwd=source, env=env, check=True)
    receipt.write_text(json.dumps({'inputs': inputs, 'archive': hashlib.sha256(archive.read_bytes()).hexdigest()}))
java = Path(os.environ.get('JAVA_HOME', ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'))
platform = 'darwin' if os.uname().sysname == 'Darwin' else 'linux'
subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', work / 'out/include',
                '-I', java / 'include', '-I', java / 'include' / platform, '-I', ROOT / 'terminal/android/native',
                ROOT / 'terminal/android/native/tests/scroll.c', ROOT / 'terminal/android/native/tests/graphics-stub.c',
                ROOT / 'terminal/android/native/surface.c',
                archive, '-lm', '-o', work / 'scroll-test'], env=env, check=True)
subprocess.run([work / 'scroll-test'], env=env, check=True)

# Reproduce the former history-only behavior in an isolated, compiled copy.
# The mouse protocol test must fail, while ordinary history still passes.
import sys
if '--mutate' in sys.argv[1:]:
    production = ROOT / 'terminal/android/native/surface.c'
    original = production.read_bytes()
    altered = work / 'history-only-surface.c'
    source = original.decode()
    anchor = '    if (!delta) return true;'
    assert source.count(anchor) == 1
    altered.write_text(source.replace(anchor, anchor + '\n    gat_surface_scroll(s, delta); return true;'))
    mutant = work / 'history-only-test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', work / 'out/include',
                    '-I', java / 'include', '-I', java / 'include' / platform, '-I', ROOT / 'terminal/android/native',
                    ROOT / 'terminal/android/native/tests/scroll.c', ROOT / 'terminal/android/native/tests/graphics-stub.c',
                    altered, archive, '-lm', '-o', mutant], env=env, check=True)
    target = subprocess.run([mutant], env=env, capture_output=True, text=True)
    assert target.returncode != 0 and 'expected 20 bytes, got 0' in target.stderr, target.stderr
    subprocess.run([mutant, '--history'], env=env, check=True)
    subprocess.run([work / 'scroll-test'], env=env, check=True)
    assert production.read_bytes() == original
    print('ANDROID SCROLL MUTATION PASS: compiled former routing red / same binary history green / original green / SHA256 ' + hashlib.sha256(original).hexdigest())

# Load the same production surface into a real, isolated tmux PTY.
import ctypes
import fcntl
import pty
import select
import struct
import time
import errno

tmux = shutil.which('tmux')
if not tmux: raise RuntimeError('tmux is required for the actual scroll/copy-mode test')
library = work / ('scroll-surface.dylib' if platform == 'darwin' else 'scroll-surface.so')
subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fPIC',
                '-dynamiclib' if platform == 'darwin' else '-shared',
                '-I', work / 'out/include', '-I', java / 'include', '-I', java / 'include' / platform,
                '-I', ROOT / 'terminal/android/native', ROOT / 'terminal/android/native/tests/graphics-stub.c',
                ROOT / 'terminal/android/native/surface.c', archive, '-lm', '-o', library], env=env, check=True)
lib = ctypes.CDLL(str(library))
callback = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t)
lib.gat_surface_new.argtypes = [callback, ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]
lib.gat_surface_new.restype = ctypes.c_void_p
lib.gat_surface_free.argtypes = [ctypes.c_void_p]
lib.gat_surface_feed.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
lib.gat_surface_resize.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16, ctypes.c_uint16, ctypes.c_uint16]
lib.gat_surface_resize.restype = ctypes.c_bool
lib.gat_surface_wheel.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
lib.gat_surface_wheel.restype = ctypes.c_bool
master, slave = pty.openpty()
fcntl.ioctl(slave, __import__('termios').TIOCSWINSZ, struct.pack('HHHH', 12, 80, 640, 192))
os.set_blocking(master, False)
socket = work / ('tmux-' + str(os.getpid()) + '.sock')
config = work / 'tmux.conf'
config.write_text('set -g mouse on\nset -g default-shell /bin/sh\nset -g status off\n')
command = [tmux, '-S', str(socket)]
@callback
def terminal_write(_context, data, length):
    try: return os.write(master, ctypes.string_at(data, length)) == length
    except OSError: return False
error = ctypes.c_char_p()
surface = lib.gat_surface_new(terminal_write, None, ctypes.byref(error))
assert surface, error.value
assert lib.gat_surface_resize(surface, 80, 12, 8, 16)
process = subprocess.Popen(command + ['-f', str(config), 'new-session', '-s', 'scroll-proof'],
                           stdin=slave, stdout=slave, stderr=slave, env=dict(env, TERM='xterm-256color', TMUX_TMPDIR=str(work)))
os.close(slave)
def drain(duration):
    until = time.monotonic() + duration
    while time.monotonic() < until:
        if select.select([master], [], [], min(.02, max(0, until-time.monotonic())))[0]:
            try: data = os.read(master, 65536)
            except OSError as failure:
                if failure.errno == errno.EIO: break
                raise
            if not data: break
            lib.gat_surface_feed(surface, data, len(data))
def tmux_command(*args):
    return subprocess.check_output(command + list(args), env=env, text=True).strip()
try:
    deadline = time.monotonic() + 10
    while not socket.exists() and time.monotonic() < deadline: drain(.05)
    assert socket.exists(), 'isolated tmux socket did not start'
    drain(.5)
    tmux_command('send-keys', '-t', 'scroll-proof', "i=1; while [ $i -le 100 ]; do echo SCROLL_$i; i=$((i+1)); done", 'Enter')
    drain(.5)
    assert int(tmux_command('display-message', '-p', '-t', 'scroll-proof', '#{history_size}')) > 20
    assert tmux_command('display-message', '-p', '-t', 'scroll-proof', '#{pane_in_mode}') == '0'
    assert lib.gat_surface_wheel(surface, -3, 16, 48, 0)
    drain(.3)
    assert tmux_command('display-message', '-p', '-t', 'scroll-proof', '#{pane_in_mode}') == '1', 'wheel must enter real tmux copy-mode'
    first = int(tmux_command('display-message', '-p', '-t', 'scroll-proof', '#{scroll_position}'))
    assert first > 0
    assert lib.gat_surface_wheel(surface, -3, 16, 48, 0)
    drain(.3)
    second = int(tmux_command('display-message', '-p', '-t', 'scroll-proof', '#{scroll_position}'))
    assert second > first
    assert lib.gat_surface_wheel(surface, 1, 16, 48, 0)
    drain(.3)
    assert first <= int(tmux_command('display-message', '-p', '-t', 'scroll-proof', '#{scroll_position}')) < second
    print('ANDROID SCROLL TMUX PASS: real PTY / isolated server / wheel enters copy-mode / history moves')
finally:
    subprocess.run(command + ['kill-server'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    process.wait(timeout=5)
    lib.gat_surface_free(surface)
    os.close(master)
    socket.unlink(missing_ok=True)
