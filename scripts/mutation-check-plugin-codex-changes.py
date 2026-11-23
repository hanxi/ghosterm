#!/usr/bin/env python3
"""Compile isolated Lua Executor snapshot mutations and verify each targeted assertion."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'plugins/official/ai-codex/executor.lua'
FIXTURE = ROOT / 'scripts/probes/codex-lua-changes.lua'
CASES = (
    ('thread-policy', '      result.approvalsReviewer ~= params.approvalsReviewer or\n', ''),
    ('identity', 'if candidate.id == input.snapshotID then', 'if candidate then'),
    ('size', 'local snapshot_limit = 524288', 'local snapshot_limit = 1048576'),
    ('wire', 'local change_chunk_bytes = 1300', 'local change_chunk_bytes = 2000'),
    ('approval-scope', 'local current = changes[approval.change]',
     'local current\n    for _, candidate in pairs(changes) do\n'
     '      if candidate.id == input.snapshotID then current = candidate end\n    end'),
    ('approval-revision', 'current.id ~= input.snapshotID)',
     '(current.id ~= input.snapshotID and input.snapshotID ~= "change-1"))'),
)


def run(lua, arguments, env, expected):
    result = subprocess.run([lua, *arguments], cwd=ROOT, env=env, capture_output=True, text=True, timeout=30)
    if expected.startswith('CODEX CHANGE ASSERTION '):
        valid = result.returncode == 1 and result.stderr.count(expected) == 1 and 'stack traceback:' in result.stderr
    else:
        valid = result.returncode == 0 and result.stdout.strip() == expected
    if not valid:
        raise RuntimeError(f'Unexpected Lua result for {arguments}: {result.stdout}{result.stderr}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lua', type=Path, required=True, help='path to a Lua 5.5.1 interpreter')
    parser.add_argument('--case', action='append', choices=[case[0] for case in CASES])
    args = parser.parse_args()
    lua = args.lua.resolve(strict=True)
    version = subprocess.run([lua, '-v'], capture_output=True, text=True, check=True, timeout=5)
    if not (version.stdout or version.stderr).startswith('Lua 5.5.1 '):
        parser.error('expected Lua 5.5.1')
    workspace_tmp = ROOT / 'tmp'
    workspace_tmp.mkdir(exist_ok=True)
    parent = Path(os.environ.get('TMPDIR', str(workspace_tmp))).resolve()
    if not parent.is_relative_to(workspace_tmp) or not parent.is_dir():
        parent = workspace_tmp
    original = SOURCE.read_bytes()
    digest = hashlib.sha256(original).digest()
    selected = [case for case in CASES if not args.case or case[0] in args.case]

    with tempfile.TemporaryDirectory(prefix='agent-', dir=parent) as temporary:
        env = dict(os.environ, TMPDIR=temporary, TMP=temporary, TEMP=temporary)
        env.pop('LUA_INIT', None)
        env.pop('LUA_INIT_5_5', None)
        target = Path(temporary) / 'executor.lua'
        for name, old, new in selected:
            anchor = old.encode()
            if original.count(anchor) != 1:
                raise RuntimeError(f'{name}: mutation anchor changed')
            invocation = [str(FIXTURE), str(target), name]
            passed = 'CODEX CHANGE CASE PASS ' + name
            target.write_bytes(original)
            run(lua, invocation, env, passed)
            target.write_bytes(original.replace(anchor, new.encode(), 1))
            run(lua, ['-e', 'assert(loadfile(arg[0])); os.exit(0)', '--', str(target)], env, '')
            run(lua, invocation, env, 'CODEX CHANGE ASSERTION ' + name)
            run(lua, [str(FIXTURE), str(target), 'control'], env, 'CODEX CHANGE CONTROL PASS')
            target.write_bytes(original)
            if hashlib.sha256(target.read_bytes()).digest() != digest:
                raise RuntimeError(f'{name}: restore digest mismatch')
            run(lua, invocation, env, passed)
            print(f'PASS {name}: compiled assertion red / independent control green / restored green / SHA256')
    if hashlib.sha256(SOURCE.read_bytes()).digest() != digest:
        raise RuntimeError('Original source changed during isolated mutations')
    print(f'CODEX CHANGE MUTATIONS PASS: {len(selected)} groups')


if __name__ == '__main__':
    main()
