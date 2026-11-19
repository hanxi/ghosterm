#!/usr/bin/env python3
"""Compile the Node-only Claude SDK bridge; test isolated callbacks and optional real no-model startup."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'plugins/official/ai-claude/node'
MAIN = 'Claude approval binds scope and frozen input, resolves once, and replays only its identical live request'
CONTROL = 'Claude decline preserves hints but never grants suggested persistent permissions'
CASES = (
    ('startup-sdk', 'cli.ts', 'code === "ERR_MODULE_NOT_FOUND" || code === "MODULE_NOT_FOUND"',
     'code === "MODULE_NOT_FOUND"',
     'Claude Node CLI reports an unavailable SDK before reading the launch profile',
     'missing Claude SDK must be reported as unavailable'),
    ('startup-cli', 'cli.ts', 'code === "CLAUDE_CLI_UNAVAILABLE" || code === "CLAUDE_CLI_VERSION_MISMATCH"',
     'code === "CLAUDE_CLI_UNAVAILABLE"',
     'Claude Node CLI distinguishes missing executable and unsupported version without starting a session',
     'missing or incompatible Claude CLI must be reported as unavailable'),
    ('scope', 'approvals.ts', 'reference.scope === this.scope &&', 'true &&', MAIN,
     'foreign Claude scope must not authorize a callback'),
    ('snapshot', 'approvals.ts', 'const value = snapshot(input);', 'const value = input as unknown as Json;', MAIN,
     'Claude approval must retain its captured input'),
    ('expired', 'approvals.ts', 'entry?.state === "pending"', 'entry !== undefined',
     'Claude backend cancellation expires a prompt without synthesizing a decline',
     'cancelled Claude request must never accept a late decision'),
    ('collision', 'approvals.ts', 'previous.identity !== identity', 'false',
     'Claude request identity collisions close all pending work while distinct Unicode ids remain separate',
     'Claude reused request IDs must not bind changed input'),
    ('unread-budget', 'approvals.ts',
     '!this.events.some(event => event.kind === "requested" && event.request === entry.request)', 'true',
     'Claude retained budgets charge unread event payloads and never reuse evicted approval tokens',
     'unread Claude request events must retain their byte reservation'),
    ('question', 'approvals.ts', '(allow && entry.request.kind === "question")', 'false',
     'Claude questions require explicit complete answers and cannot be approved as ordinary tools',
     'Claude questions cannot accept an empty generic approval'),
    ('provenance', 'approvals.ts', 'source: text(context.mcpServer.source, 128)', 'source: "sdk"',
     'Claude MCP provenance stays immutable and changed sources cannot reuse an approval',
     'Claude MCP provenance must not upgrade an unknown source'),
    ('permission-mode', 'profile.ts', 'permissionMode: "default"', 'permissionMode: "bypassPermissions"',
     'Claude profile requires a live explicit owner and preserves selected configuration boundaries',
     'Claude profile must not bypass backend permissions'),
    ('spawn-once', 'profile.ts', 'if (launched || launchedScopes.has(approvals))', 'if (false)',
     'Claude profile requires a live explicit owner and preserves selected configuration boundaries',
     'Claude profile must not replay a process launch'),
    ('rebuilt-profile', 'profile.ts', 'launchedScopes.has(approvals)', 'false',
     'Claude profile requires a live explicit owner and preserves selected configuration boundaries',
     'Claude scopes must not respawn through rebuilt options'),
    ('broker-generation', 'approvals.ts', '`approval-${this.generation}-${++this.serial}`', '`approval-${++this.serial}`',
     'Claude approval tokens do not survive a new broker even when caller scope and request id repeat',
     'Claude references must not cross broker generations'),
    ('session-correlation', 'session.ts', 'value.user_message_uuid !== this.active', 'false',
     'Claude session preserves uncorrelated backend authentication diagnostics without claiming a result',
     'Claude foreign result must not complete the active input'),
    ('session-turn-expiry', 'session.ts',
     'this.profile.approvals.expirePending();\n        if', '/* mutation: retain old turn prompts */\n        if',
     'Claude session streams correlated input and ordered immutable SDK events across turns',
     'Claude turn boundary must expire pending prompts'),
    ('session-interrupt-expiry', 'session.ts',
     'this.profile.approvals.expirePending();\n    if', '/* mutation: retain interrupted prompts */\n    if',
     'Claude session interrupt expires prompts and preserves receipt without fabricating turn completion',
     'Claude interrupt must expire pending approval immediately'),
    ('session-receipt', 'session.ts', 'if (!this.terminal) this.publish({ kind: "interrupt-receipt"',
     'this.phaseValue = "ready"; if (!this.terminal) this.publish({ kind: "interrupt-receipt"',
     'Claude session interrupt expires prompts and preserves receipt without fabricating turn completion',
     'Claude interrupt receipt must not complete a turn'),
    ('session-eof', 'session.ts', 'this.finish("lost", new Error("Claude SDK output ended"))',
     'this.finish("stopped", new Error("Claude SDK output ended"))',
     'Claude session EOF and explicit close preserve an unresolved input as unknown',
     'Claude EOF must not be treated as successful completion'),
    ('session-queue-bytes', 'session.ts', 'this.bytes + bytes > this.limits.queuedBytes - 512', 'false',
     'Claude session enforces unread events aggregate output and lifetime budgets',
     'Claude bytes budget must stop the session'),
    ('session-total-output', 'session.ts', 'this.output += bytes;', 'this.output += 0;',
     'Claude session enforces unread events aggregate output and lifetime budgets',
     'Claude output budget must stop the session'),
    ('session-close-identity', 'session.ts', 'const pendingMessageID = this.active ?? null;', 'const pendingMessageID = null;',
     'Claude session EOF and explicit close preserve an unresolved input as unknown',
     'Claude close must preserve unresolved input identity'),
    ('stdio-retry', 'stdio.ts', 'return previous.reply;', 'return Promise.resolve("{}");',
     'Claude stdio dispatches explicit actions and identical retries never send twice',
     'Claude stdio pending retry must share its request'),
    ('stdio-old-sequence', 'stdio.ts', 'sequence !== this.sequence + 1', 'sequence > this.sequence + 1',
     'Claude stdio changed or evicted sequences cannot authorize another action',
     'Claude stdio stale or changed sequence must fail closed'),
    ('stdio-changed-replay', 'stdio.ts', 'previous.identity !== line', 'false',
     'Claude stdio changed or evicted sequences cannot authorize another action',
     'Claude stdio stale or changed sequence must fail closed'),
    ('stdio-canonical', 'stdio.ts', 'JSON.stringify(value) !== line', 'false',
     'Claude stdio refuses configuration injection ambiguous JSON and nonexplicit decisions',
     'Claude stdio invalid frame must not dispatch'),
    ('stdio-method-scope', 'stdio.ts', 'this.validate(method, params!);', '/* mutation: skip method shape */',
     'Claude stdio refuses configuration injection ambiguous JSON and nonexplicit decisions',
     'Claude stdio invalid frame must not dispatch'),
    ('stdio-frame-budget', 'stdio.ts',
     'if (used + size > FRAME_MAX) throw new ClaudeStdioProtocolError("Claude stdio frame limit");',
     'if (used + size > FRAME_MAX) return "eof";',
     'Claude stdio framing preserves split UTF8 and rejects malformed or incomplete byte frames',
     'Claude stdio invalid bytes must fail'),
    ('stdio-utf8', 'stdio.ts', 'new TextDecoder("utf-8", { fatal: true })', 'new TextDecoder("utf-8", { fatal: false })',
     'Claude stdio framing preserves split UTF8 and rejects malformed or incomplete byte frames',
     'Claude stdio invalid bytes must fail'),
    ('stdio-backpressure', 'stdio.ts',
     'await new Promise<void>((resolve, reject) => output.write(response + "\\n", error => error ? reject(error) : resolve()));',
     'await Promise.resolve(output.write(response + "\\n", () => {}));',
     'Claude stdio write backpressure prevents dispatching the next action',
     'Claude stdio must respect response backpressure before dispatch'),
    ('stdio-eof', 'stdio.ts', 'return "eof";', 'return "stopped";',
     'Claude stdio transport loss and broken output close their SDK scope without a stop receipt',
     'Claude stdio EOF must not become an explicit stop'),
    ('executor-sha', 'executor.ts', 'if (hash.digest("hex") !== expected)', 'if (false)',
     'real Node executor pipes initialize stop and EOF against the locked no-network CLI',
     'real Node executor must distinguish stop EOF and invalid profile'),
    ('approval-turn', 'approvals.ts', 'turnID: this.turnID ?? null', 'turnID: null',
     'Claude backend request ids cannot replay an approval across explicit user turns',
     'Claude approval identity must include the submitted user turn'),
    ('session-approval-turn', 'session.ts', 'this.profile.approvals.beginTurn(uuid);', '/* mutation: omit submitted turn binding */',
     'Claude session streams correlated input and ordered immutable SDK events across turns',
     'Claude session must bind approval identity to its submitted input'),
    ('poll-batch-count', 'stdio.ts', 'events.length < Number(params.maxEvents)', 'events.length < 33',
     'Claude stdio poll batches bound count and bytes without losing fair ordered events',
     'Claude batch must honor requested count and alternate event sources'),
    ('poll-batch-bytes', 'stdio.ts', 'bytes < 4096', 'bytes < 600000',
     'Claude stdio poll batches bound count and bytes without losing fair ordered events',
     'Claude batch must bound the prefix before one complete large event'),
    ('poll-batch-range', 'stdio.ts', 'params.maxEvents > 32', 'params.maxEvents > 33',
     'Claude stdio rejects invalid explicit poll batch limits before consuming events',
     'Claude poll batch limits must reject out-of-range and noninteger values'),
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, env, *, seconds=120, assertion=None):
    result = subprocess.run(['bash', '-c',
        '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require claude-node; shift; gt_timeout "$@"',
        'claude-node', str(ROOT), str(seconds), *map(str, command)], cwd=ROOT, env=env, capture_output=True, text=True)
    output = result.stdout + result.stderr
    valid = result.returncode == 0 if assertion is None else (
        result.returncode == 1 and 'ℹ fail 1' in output and 'AssertionError [ERR_ASSERTION]' in output and assertion in output)
    if not valid: raise RuntimeError(f'Unexpected Claude test result:\n{output}')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    dependencies = parser.add_mutually_exclusive_group()
    dependencies.add_argument('--fetch', action='store_true', help='fetch the integrity-locked Node dependency closure')
    dependencies.add_argument('--reuse-dependencies', type=Path,
                              help='reuse a previously installed dependency tree with matching package and lock files')
    parser.add_argument('--mutate', action='store_true', help='compile isolated protection mutations')
    parser.add_argument('--mutation-case', action='append', choices=[case[0] for case in CASES],
                        help='run only named mutations after the baseline')
    parser.add_argument('--real-cli', action='store_true', help='also initialize the locked native CLI without a prompt, under macOS no-network sandbox')
    args = parser.parse_args()
    if args.mutation_case and not args.mutate:
        parser.error('--mutation-case requires --mutate')
    if args.mutation_case and 'executor-sha' in args.mutation_case and not args.real_cli:
        parser.error('executor-sha requires --real-cli')
    if args.real_cli and sys.platform != 'darwin':
        parser.error('--real-cli requires the macOS no-network test profile; no fallback or skip is allowed')
    existing = args.reuse_dependencies.resolve() if args.reuse_dependencies else None
    if existing:
        for name in ('package.json', 'package-lock.json'):
            candidate = existing / name
            if not candidate.is_file() or candidate.read_bytes() != (SOURCE / name).read_bytes():
                parser.error(f'--reuse-dependencies: {name} does not match the Claude lock')
        if not (existing / 'node_modules/typescript/bin/tsc').is_file():
            parser.error('--reuse-dependencies requires installed dev dependencies')
    (ROOT / 'tmp').mkdir(exist_ok=True)
    inputs = [*SOURCE.rglob('*.ts'), SOURCE / 'package.json', SOURCE / 'package-lock.json', ROOT / 'scripts/test-plugin-claude-node.mjs']
    before = {str(path): sha(path) for path in inputs}
    with tempfile.TemporaryDirectory(prefix='claude-node-test-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        package = work / 'node'
        shutil.copytree(SOURCE, package)
        environment = dict(os.environ, TMPDIR=str(work))
        if existing:
            (package / 'node_modules').symlink_to(existing / 'node_modules', target_is_directory=True)
        else:
            install = ['npm', 'ci', '--prefix', package, '--ignore-scripts', '--no-audit', '--no-fund',
                '--cache', ROOT / 'tmp/claude-npm-cache', '--registry', 'https://registry.npmjs.org']
            if not args.fetch: install.append('--offline')
            run(install, environment, seconds=300)
        test_environment = {key: os.environ[key] for key in ('PATH', 'LANG', 'LC_ALL') if key in os.environ}
        for key, name in (('HOME', 'home'), ('CLAUDE_CONFIG_DIR', 'config'), ('XDG_CONFIG_HOME', 'xdg')):
            directory = work / name; directory.mkdir(); test_environment[key] = str(directory)
        test_environment.update(TMPDIR=str(work), GHOSTERM_CLAUDE_NODE_PACKAGE=str(package))
        compiler = ['node', package / 'node_modules/typescript/bin/tsc', '-p', package / 'tsconfig.json', '--noEmitOnError']
        tests = ['node', '--test', ROOT / 'scripts/test-plugin-claude-node.mjs']
        run(compiler, environment)
        if args.real_cli: test_environment['GHOSTERM_CLAUDE_REAL_CLI'] = '1'
        output = run(tests, test_environment, seconds=90)
        expected = 31 if args.real_cli else 29
        if f'ℹ pass {expected}\n' not in output or 'ℹ fail 0' not in output or 'ℹ skipped 0' not in output:
            raise RuntimeError('Claude test count/skip receipt mismatch:\n' + output)
        print(output, end='', flush=True)
        test_environment.pop('GHOSTERM_CLAUDE_REAL_CLI', None)
        cases = [case for case in CASES if args.real_cli or case[0] != 'executor-sha']
        if args.mutation_case:
            cases = [case for case in cases if case[0] in args.mutation_case]
        if args.mutate:
            for name, file, old, new, target, assertion in cases:
                if name == 'executor-sha': test_environment['GHOSTERM_CLAUDE_REAL_CLI'] = '1'
                else: test_environment.pop('GHOSTERM_CLAUDE_REAL_CLI', None)
                source = package / 'src' / file
                original = source.read_bytes()
                if original.count(old.encode()) != 1: raise RuntimeError(f'{name}: mutation anchor changed')
                def one(label, expected_assertion=None):
                    command = ['node', '--test', '--test-name-pattern=^' + re.escape(label) + '$', ROOT / 'scripts/test-plugin-claude-node.mjs']
                    transcript = run(command, test_environment, assertion=expected_assertion)
                    if expected_assertion is None and 'ℹ pass 1\n' not in transcript:
                        raise RuntimeError('Independent Claude target did not execute:\n' + transcript)
                try:
                    source.write_bytes(original.replace(old.encode(), new.encode(), 1))
                    run(compiler, environment)
                    one(target, assertion)
                    one(CONTROL)
                finally:
                    source.write_bytes(original)
                if sha(source) != before[str(SOURCE / 'src' / file)]: raise RuntimeError('Claude source restore SHA256 mismatch')
                run(compiler, environment); one(target)
                print(f'PASS {name}: compiled assertion red / same build independent control green / restored green / SHA256', flush=True)
    if {str(path): sha(path) for path in inputs} != before: raise RuntimeError('Original Claude inputs changed during isolated tests')
    print(f'CLAUDE NODE BRIDGE PASS: {expected} tests; {len(cases) if args.mutate else 0} mutation groups; no model request', flush=True)


if __name__ == '__main__':
    main()
