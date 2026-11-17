#!/usr/bin/env python3
"""Compile guarded config view/parser mutations in an owned, isolated worktree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

SOURCE_ROOT = Path(__file__).resolve().parents[1]
ROOT = Path(subprocess.check_output(['git', '-C', SOURCE_ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], text=True).strip()).parent
CASES = [
    ('equals-space', "            while (q < le && (*q == ' ' || *q == '\\t')) {\n                q++;\n            }\n            if (q < le && *q == '=')", "            if (q < le && *q == '=')"),
    ('long-token', "        buffer[length] = '\\0';", "        buffer[length > 1023 ? 1023 : length] = '\\0';"),
    ('ambiguous', '    if (matching_blocks != 1) {', '    if (matching_blocks == 0) {'),
    ('match', '        if (kw_eq(cfg->nodes[i].keyword, "match")) {\n            gt__set_error("config_directive_at:', '        if (false) {\n            gt__set_error("config_directive_at:'),
    ('include-owner', 'node->block != block || node->included != header->included ||\n            strcmp(node->source_path, header->source_path) ||', 'node->block != block || (header->included && false) ||'),
    ('raw-bytes', '        out->raw = node->raw;\n        out->source_path = node->source_path;', '        out->raw = "";\n        out->source_path = node->source_path;'),
    ('failed-candidate', '    if (candidate->include_error) {', '    if (false) {'),
    ('nul', '    if (memchr(text, 0, len)) {', '    if (false) {'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkout', required=True, type=Path)
    parser.add_argument('--case', action='append', choices=[x[0] for x in CASES])
    args = parser.parse_args()
    checkout = args.checkout.resolve()
    registered = subprocess.check_output(['git', '-C', ROOT, 'worktree', 'list', '--porcelain'], text=True)
    if ROOT / 'tmp' not in checkout.parents or f'worktree {checkout}\n' not in registered:
        raise SystemExit('requires registered worktree beneath project tmp/')
    source = checkout / 'core/src/config.c'
    original = source.read_bytes()
    main_bytes = (ROOT / 'core/src/config.c').read_bytes()
    tests = (checkout / 'core/tests/test_config_directives.c').read_bytes()
    directory = Path(tempfile.mkdtemp(prefix='directive-mutations-', dir=checkout / 'tmp'))
    records = []
    env = dict(os.environ, TMPDIR=str(checkout / 'tmp'))

    def build(label):
        # BSD make compares seconds; force compilation even for same-second writes.
        for relative in ['core/build/libghosterm_core.a', 'core/build/test_config_directives', 'core/build/test_config',
                         'core/build/CMakeFiles/ghosterm_core.dir/src/config.c.o',
                         'core/build/CMakeFiles/test_config_directives.dir/tests/test_config_directives.c.o']:
            (checkout / relative).unlink(missing_ok=True)
        with (directory / f'{label}-build.log').open('w') as output:
            subprocess.run(['cmake', '--build', 'core/build', '--target', 'test_config_directives', 'test_config'], cwd=checkout, env=env, stdout=output, stderr=subprocess.STDOUT, check=True)

    def run(label, red=False, control=False):
        command = ['ctest', '--test-dir', 'core/build', '-R', '^sshconfig_golden$', '--output-on-failure'] if control else [str(checkout / 'core/build/test_config_directives'), str(checkout / 'tmp')]
        result = subprocess.run(command, cwd=checkout, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (directory / f'{label}.log').write_text(result.stdout)
        if red:
            assert result.returncode == 1 and result.stdout.count('FAIL directives: ') == 1, (label, result.returncode, result.stdout)
            assert not any(marker in result.stdout for marker in ['AddressSanitizer', 'runtime error:', 'Segmentation']), label
        else:
            assert result.returncode == 0, (label, result.stdout)
        records.append({'case': label, 'returncode': result.returncode})

    try:
        build('baseline'); run('baseline')
        for name, anchor, replacement in CASES:
            if args.case and name not in args.case:
                continue
            text = original.decode()
            assert text.count(anchor) == 1, (name, 'anchor count', text.count(anchor))
            source.write_text(text.replace(anchor, replacement, 1))
            build(name); run(name + '-red', red=True); run(name + '-control', control=True)
            source.write_bytes(original)
            build(name + '-restored'); run(name + '-restored')
        assert source.read_bytes() == original
        assert (ROOT / 'core/src/config.c').read_bytes() == main_bytes
        assert (checkout / 'core/tests/test_config_directives.c').read_bytes() == tests
        (directory / 'receipt.json').write_text(json.dumps({'source_sha256': hashlib.sha256(original).hexdigest(), 'tests_sha256': hashlib.sha256(tests).hexdigest(), 'records': records}, indent=2) + '\n')
    finally:
        source.write_bytes(original)
        build('final-restored')
    print(f'PASS: {directory / "receipt.json"}')


if __name__ == '__main__':
    main()
