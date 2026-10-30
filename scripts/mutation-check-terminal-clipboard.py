#!/usr/bin/env python3
"""剪贴板接线：编译成功、目标红、同变异网格对照绿、复原 SHA256。手工闸门。"""
import hashlib
import os
from pathlib import Path
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VIEW = ROOT / 'apple/GhostermTerminal/Sources/GhostermTerminal/TerminalSurfaceView.swift'
MARK = 'GT_CLIPBOARD_MUTATION_MARK'
original = VIEW.read_bytes()
if MARK.encode() in original:
    raise SystemExit('残留变异标记，拒绝执行')
digest = hashlib.sha256(original).digest()
os.environ['TMPDIR'] = str(ROOT / 'tmp')

def interrupted(sig, frame):
    raise RuntimeError(f'interrupted: {sig}')

for sig in (signal.SIGINT, signal.SIGTERM):
    signal.signal(sig, interrupted)

def probe(test, red=False):
    result = subprocess.run(['bash', '-c',
        'source scripts/lib/gt-timeout.sh\ngt_timeout_require\ngt_timeout 180 swift test --package-path apple/GhostermTerminal --filter "$1"',
        'clipboard', test], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (ROOT / 'tmp/clipboard-mutation.log').write_text(result.stdout)
    valid = (result.returncode == 1 and 'Build complete!' in result.stdout and
             'XCTAssert' in result.stdout and 'Executed 1 test' in result.stdout) if red else (
                 result.returncode == 0 and 'with 0 failures' in result.stdout)
    if not valid:
        raise RuntimeError(result.stdout[-7000:])

target = 'TerminalClipboardTests'
control = 'TerminalSurfaceTests.testOutputReachesGhosttyGrid'
try:
    probe(target)
    for old, new in [
        ('@objc public func copy(_ sender: Any?)', '@objc public func disconnectedCopy(_ sender: Any?)'),
        ('@objc public func paste(_ sender: Any?)', '@objc public func disconnectedPaste(_ sender: Any?)'),
        ('NSPasteboard.general.setString(text, forType: .string)', '_ = text'),
        ('surface.sendText(text)', '_ = text'),
    ]:
        source = original.decode()
        assert source.count(old) == 1
        VIEW.write_text(source.replace(old, new) + '\n// ' + MARK + '\n')
        probe(target, red=True)
        probe(control)
        VIEW.write_bytes(original)
        assert hashlib.sha256(VIEW.read_bytes()).digest() == digest
        probe(target)
        print('PASS target red / control green / restored green:', old, flush=True)
finally:
    VIEW.write_bytes(original)
    assert hashlib.sha256(VIEW.read_bytes()).digest() == digest
