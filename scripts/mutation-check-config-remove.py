#!/usr/bin/env python3
"""Compiled removal regressions in an isolated Core, with independent API controls."""
import hashlib
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--apple', action='store_true')
args = parser.parse_args()
ROOT = Path(__file__).resolve().parents[1]
logs = ROOT / 'tmp/config-remove-mutations'
logs.mkdir(parents=True, exist_ok=True)
production = ROOT / 'core/src/config.c'
original = production.read_bytes()
def run(command, label):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
    (logs / (label + '.log')).write_text(result.stdout + result.stderr)
    return result
with tempfile.TemporaryDirectory(prefix='config-remove-', dir=ROOT / 'tmp') as directory:
    work = Path(directory)
    (work / 'tmp').mkdir()
    shutil.copytree(ROOT / 'core', work / 'core', ignore=shutil.ignore_patterns('build', 'build-*'))
    source = work / 'core/src/config.c'
    build = work / 'core/build'
    configured = run(['cmake', '-S', work / 'core', '-B', build, '-DCMAKE_BUILD_TYPE=Debug'], 'configure')
    assert configured.returncode == 0, configured.stderr
    def compile(label):
        result = run(['cmake', '--build', build, '--clean-first', '--target', 'test_config_remove', 'test_api'], label + '-build')
        assert result.returncode == 0, result.stderr
    compile('baseline')
    assert run([build / 'test_config_remove', work / 'tmp'], 'baseline').returncode == 0
    cases = [
        ('no-op', 'return config_replace_checked(cfg, text);', 'free(text); return GT_OK;', 'non-target bytes survive'),
        ('shared-header', 'if (header->nargs > 1)', 'if (false && header->nargs > 1)', 'non-target bytes survive'),
        ('match-boundary', ' || kw_eq(node->keyword, "match")', '', 'non-target bytes survive'),
        ('include-source', 'if (info.included || info.matching_blocks != 1)', 'if (false || info.matching_blocks != 1)', 'included source'),
        ('include-body', 'if (kw_eq(node->keyword, "include"))', 'if (false && kw_eq(node->keyword, "include"))', 'rejected edit is atomic'),
        ('duplicate-block', 'info.matching_blocks != 1', 'false', 'rejected edit is atomic'),
        ('repeated-token', 'if (matches < 1 || matches > 1)', 'if (matches < 1)', 'rejected edit is atomic'),
    ]
    for label, anchor, replacement, reason in cases:
        clean = original.decode()
        prefix, body = clean.split('int gt_config_remove_host', 1)
        assert body.count(anchor) == 1, label
        source.write_text(prefix + 'int gt_config_remove_host' + body.replace(anchor, replacement))
        compile(label)
        target = run([build / 'test_config_remove', work / 'tmp'], label)
        assert target.returncode == 1 and 'FAIL remove: ' + reason in target.stderr, target.stdout + target.stderr
        assert run([build / 'test_api'], label + '-control').returncode == 0
        source.write_bytes(original)
        compile(label + '-restored')
        assert run([build / 'test_config_remove', work / 'tmp'], label + '-restored').returncode == 0
        assert source.read_bytes() == original and production.read_bytes() == original
        print('PASS ' + label + ': compiled target red / independent API green / restored green / SHA256', flush=True)
    if args.apple:
        package = work / 'apple/GhostermCore'
        shutil.copytree(ROOT / 'apple/GhostermCore', package, symlinks=True,
                        ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        binding = package / 'Sources/GhostermCore/GhostermCore.swift'
        clean = binding.read_bytes()
        environment = dict(os.environ, TMPDIR=str(work / 'tmp'),
                           DEVELOPER_DIR=os.environ.get('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer'))
        def swift(label, selection, red=False):
            result = subprocess.run(['swift', 'test', '--package-path', str(package), '--filter', selection],
                                    cwd=ROOT, env=environment, capture_output=True, text=True)
            (logs / (label + '.log')).write_text(result.stdout + result.stderr)
            if red:
                assert result.returncode == 1 and 'XCTAssertEqual failed' in result.stdout + result.stderr, result.stdout + result.stderr
            else: assert result.returncode == 0, result.stdout + result.stderr
        swift('swift-baseline', 'testConfigRemovePreservesSiblingAndRejectsAmbiguity')
        try:
            text = clean.decode()
            anchor = 'try check(alias.withCString { gt_config_remove_host(handle, $0) })'
            assert text.count(anchor) == 1
            binding.write_text(text.replace(anchor, 'return'))
            swift('swift-no-op', 'testConfigRemovePreservesSiblingAndRejectsAmbiguity', red=True)
            swift('swift-no-op-control', 'testConfigRoundTripIsByteIdentical')
        finally: binding.write_bytes(clean)
        swift('swift-restored', 'testConfigRemovePreservesSiblingAndRejectsAmbiguity')
        assert binding.read_bytes() == clean
        print('PASS Swift binding: compiled target red / independent roundtrip green / restored green / SHA256', flush=True)
assert production.read_bytes() == original
print('CONFIG REMOVE MUTATIONS PASS: 7 groups; SHA256 ' + hashlib.sha256(original).hexdigest())
