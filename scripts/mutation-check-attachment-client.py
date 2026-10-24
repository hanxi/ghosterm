#!/usr/bin/env python3
"""Shared attachment client: ordinary isolated failures, no engine/PTY linkage."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'tmp/attachment-client-mutations'
MARK = 'GT_ATTACHMENT_CLIENT_MUTATION_MARK'
NAMES = ('ipc/attachment-client.c', 'ipc/attachment-client.h', 'ipc/connection.c',
         'ipc/connection.h', 'ipc/control-info.c')
PATHS = {name: ROOT / 'core/src' / name for name in NAMES}
PATHS['test.c'] = ROOT / 'core/tests/test_attachment_client.c'
PATHS['check.py'] = ROOT / 'scripts/check-attachment-client-deps.py'
MODES = ('flow', 'readonly', 'commands', 'malformed', 'applied', 'deadlines', 'error',
         'invalid', 'backpressure', 'replies', 'clocks', 'encoded', 'deps')


def run(args, timeout=30):
    p = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, start_new_session=True)
    try:
        out, _ = p.communicate(timeout=timeout)
    except BaseException:
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        p.wait()
        raise
    return p.returncode, out


def main():
    original = {name: path.read_bytes() for name, path in PATHS.items()}
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in original.items()}
    if any(MARK.encode() in b for b in original.values()) or any(
            MARK in p.read_text() for p in WORK.rglob('*.[ch]')):
        return 2

    def interrupted(signum, frame):
        raise RuntimeError('attachment client mutation interrupted: ' + str(signum))

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    os.environ['TMPDIR'] = str(ROOT / 'tmp')
    os.umask(0o077)
    copies = {name: WORK / name for name in PATHS}
    for p in copies.values():
        p.parent.mkdir(parents=True, exist_ok=True)

    def restore():
        for name, p in copies.items():
            p.write_bytes(original[name])
            if hashlib.sha256(p.read_bytes()).hexdigest() != hashes[name] or PATHS[name].read_bytes() != original[name]:
                raise RuntimeError('attachment client SHA256 rollback failed/' + name)

    restore()
    rc, out = run(['bash', str(ROOT / 'scripts/build-core.sh')], 120)
    if rc:
        raise RuntimeError('client dependency build failed: ' + out)
    archive = ROOT / 'core/build/libghosterm_core.a'
    archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()

    def build():
        rc, out = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
                       '-D_DARWIN_C_SOURCE' if sys.platform == 'darwin' else '-D_POSIX_C_SOURCE=200809L',
                       '-Wall', '-Wextra', '-Werror', '-I', str(WORK), '-I', str(WORK / 'ipc'),
                       '-I', str(ROOT / 'core/src'), '-I', str(ROOT / 'core/src/ipc'),
                       '-I', str(ROOT / 'core/include'),
                       *[str(copies[n]) for n in NAMES if n.endswith('.c')],
                       str(copies['test.c']), str(archive), '-pthread',
                       *(['-lutil'] if sys.platform.startswith('linux') else []), '-o', str(WORK / 'test')])
        if rc:
            raise RuntimeError('attachment client compilation failed: ' + out)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != archive_hash:
            raise RuntimeError('attachment client archive changed')

    def probe(mode, reject=False):
        args = ([sys.executable, str(copies['check.py']), str(WORK / 'test')] if mode == 'deps'
                else [str(WORK / 'test'), mode])
        rc, out = run(args)
        if reject:
            if rc != 1 or 'FAIL ' not in out:
                raise RuntimeError('client mutation needs ordinary FAIL/exit 1/' + mode + ': ' + out)
        elif rc:
            raise RuntimeError('client baseline/control failed/' + mode + ': ' + out)

    specs = (
        ('encoded generation ignored','if(encoded && (generation!=c->config.generation || ','(void)generation; if(encoded && (','encoded','invalid'),
        ('encoded pre-barrier accepted','sequence<c->cut || ','','encoded','invalid'),
        ('encoded future state accepted',' || sequence>c->info.sequence','','encoded','invalid'),
        ('encoded state silently restamped','encoded?sequence:c->info.sequence','c->info.sequence','encoded','invalid'),

        ('known zero mistaken for unknown', 'c->info.has_sequence=true;', 'c->info.has_sequence=a.sequence!=0;', 'flow', 'invalid'),
        ('S admission mistaken for application', 'c->stage==WAIT_S && c->info.has_sequence && c->info.sequence>=c->snapshot', 'c->stage==WAIT_S', 'flow', 'invalid'),
        ('L admission mistaken for application', 'c->stage==WAIT_L && c->info.has_sequence && c->info.sequence>=c->cut', 'c->stage==WAIT_L', 'flow', 'invalid'),
        ('input unlocked before READY L flush', 'c->stage=READY_L_PENDING;', 'c->stage=READY_L_PENDING;c->info.phase=GT_CLIENT_LIVE;', 'flow', 'invalid'),
        ('readonly input allowed', 'c->info.phase!=GT_CLIENT_LIVE || c->info.readonly', '(c->info.phase!=GT_CLIENT_LIVE && c->info.phase!=GT_CLIENT_READONLY)', 'readonly', 'invalid'),
        ('terminal published before engine applies', 'c->terminal_pending=true;', 'c->terminal_pending=true;c->info.has_terminal=true;', 'flow', 'invalid'),
        ('terminal input remains enabled', 'c->info.terminal=m.terminal;c->info.readonly=true;', 'c->info.terminal=m.terminal;', 'flow', 'invalid'),
        ('frame generation ignored', ' || m.generation!=c->config.generation', '', 'malformed', 'invalid'),
        ('snapshot allocation precedes quota check', ' || m.total>c->config.snapshot_limit', '', 'malformed', 'invalid'),
        ('snapshot offset ignored', ' || m.offset!=c->offset', '', 'malformed', 'invalid'),
        ('event gap allowed', ' || m.sequence!=c->submitted+1', '', 'malformed', 'invalid'),
        ('L accepts missing deltas', 'c->stage!=WAIT_CUT || !c->ready_s_sent || m.sequence!=c->submitted', 'c->stage!=WAIT_CUT || !c->ready_s_sent', 'malformed', 'invalid'),
        ('wrong session terminal accepted', 'm.terminal.id!=c->config.session || ', '', 'malformed', 'invalid'),
        ('old sink generation applied', 'if(a.has_sequence && a.generation<c->config.generation)return true;', '', 'applied', 'invalid'),
        ('future sink generation accepted', 'a.generation!=c->config.generation || ', '', 'applied', 'invalid'),
        ('sink regression accepted', ' || (c->info.has_sequence && a.sequence<c->info.sequence)', '', 'applied', 'invalid'),
        ('sink ahead of submission accepted', ' || a.sequence>c->submitted', '', 'applied', 'invalid'),
        ('client trusts caller stamp', 'r.stamp=(gt_worker_stamp_t){c->config.generation,c->last_id+1,encoded?sequence:c->info.sequence};', 'r.stamp=request->stamp;', 'flow', 'invalid'),
        ('backpressure burns command ID', 'if(gt__connection_wants_write(c->connection))return GT_CLIENT_BACKPRESSURE;', 'if(gt__connection_wants_write(c->connection)){c->last_id++;return GT_CLIENT_BACKPRESSURE;}', 'backpressure', 'invalid'),
        ('pending command never tracked', 'c->commands[index]=(command){.used=true,', 'c->commands[index]=(command){.used=false,', 'commands', 'invalid'),
        ('unconfirmed input presented as not executed', '.delivery=GT_USER_OUTCOME_UNKNOWN', '.delivery=GT_USER_NOT_EXECUTED', 'commands', 'invalid'),
        ('completed command not removed', 'memset(p,0,sizeof(*p));c->config.sink.result(c->config.sink.context,&reply);return true;', 'c->config.sink.result(c->config.sink.context,&reply);return true;', 'replies', 'invalid'),
        ('result type mismatch accepted', 'p->kind!=reply.kind || ', '', 'replies', 'invalid'),
        ('result stamp mismatch accepted', ' || p->stamp.applied_sequence!=reply.stamp.applied_sequence', '', 'replies', 'invalid'),
        ('clock rollback ignored', ' || now<c->now', '', 'clocks', 'invalid'),
        ('restore timeout boundary ignored', 'now-c->start>=c->config.restore_timeout_ms', 'now-c->start>c->config.restore_timeout_ms', 'deadlines', 'readonly'),
        ('application deadline disabled', 'c->applying && now-c->apply_started>=c->config.apply_timeout_ms', 'false && c->applying && now-c->apply_started>=c->config.apply_timeout_ms', 'deadlines', 'readonly'),
        ('busy consumer deadline disabled', 'c->blocked && now-c->blocked_started>=c->config.apply_timeout_ms', 'false && c->blocked && now-c->blocked_started>=c->config.apply_timeout_ms', 'deadlines', 'readonly'),
        ('heartbeat deadline disabled', 'c->ping_pending && now-c->ping_started>=c->config.peer_timeout_ms', 'false && c->ping_pending && now-c->ping_started>=c->config.peer_timeout_ms', 'deadlines', 'readonly'),
        ('command deadline disabled', 'c->commands[i].used && now-c->commands[i].started>=c->config.command_timeout_ms', 'false && c->commands[i].used && now-c->commands[i].started>=c->config.command_timeout_ms', 'deadlines', 'readonly'),
        ('sink refusal treated as success', 'fail(c,error,GT_CLIENT_REASON_SINK);return false;', '(void)c;return false;', 'clocks', 'invalid'),
        ('error ACK closes before server reads', 'else if(marker==GT_MSG_RESTORE_ERROR_ACK)c->error_ack_sent=true;', 'else if(marker==GT_MSG_RESTORE_ERROR_ACK)fail(c,c->info.error,GT_CLIENT_REASON_SERVER);', 'error', 'invalid'),
        ('snapshot byte erasure omitted', 'gt__secure_erase(c->snapshot_bytes,(size_t)c->total);', '', 'flow', 'invalid'),
        ('client storage erasure omitted', 'gt__secure_erase(c,sizeof(*c));', '', 'flow', 'deps'),
        ('invalid constructor limit accepted', ' || config->commands>16', '', 'invalid', 'readonly'),
        ('maximum snapshot limit ignored', ' || config->snapshot_limit>64*1024*1024', '', 'invalid', 'readonly'),
        ('zero restore timeout allowed', '!config->restore_timeout_ms || ', '', 'invalid', 'readonly'),
        ('error drain deadline ignored', 'now-c->error_started>=c->config.error_timeout_ms', 'now-c->error_started>c->config.error_timeout_ms', 'error', 'readonly'),
        ('old PONG nonce accepted', ' || m.nonce!=c->ping_nonce', '', 'deadlines', 'readonly'),
        ('peer ping nonce not echoed', 'm.nonce=c->pong_nonce;', 'm.nonce=c->pong_nonce+1;', 'deadlines', 'readonly'),
        ('receive capacity unchecked', ' || limits.receive_limit<65552', '', 'invalid', 'readonly'),
        ('send capacity unchecked', ' || limits.send_limit<65560', '', 'invalid', 'readonly'),
        ('bad envelope mistaken for transient IO', 'state==GT_CONNECTION_PROTOCOL_ERROR', 'false && state==GT_CONNECTION_PROTOCOL_ERROR', 'malformed', 'readonly'),
        ('frame deadline mistaken for IO error', 'state==GT_CONNECTION_TIMEOUT', 'false && state==GT_CONNECTION_TIMEOUT', 'clocks', 'readonly'),
    )
    try:
        build()
        for mode in MODES:
            probe(mode)
        for label, old, new, target, control in specs:
            source = original['ipc/attachment-client.c'].decode()
            if source.count(old) != 1:
                raise RuntimeError('attachment client mutation anchor drift/' + label + ': ' + str(source.count(old)))
            try:
                copies['ipc/attachment-client.c'].write_text(source.replace(old, new) + '\n/* ' + MARK + ' */\n')
                build(); probe(target, True); probe(control)
                print('PASS attachment client mutation/' + label + ': ordinary red, same-mutant control green', flush=True)
            finally:
                restore()
        try:
            source = original['ipc/control-info.c'].decode()
            copies['ipc/control-info.c'].write_text(source + '\n/* ' + MARK + ' */\nvoid gt__client_dependency_mutation(gt_session_manager_t *m){gt_managed_info_t info;(void)gt__manager_info(m,1,&info);}\n')
            build(); probe('deps', True); probe('readonly')
            print('PASS attachment client mutation/host runtime linked: ordinary red, same-mutant readonly green', flush=True)
        finally:
            restore()
        build()
        for mode in MODES:
            probe(mode)
        (ROOT / 'tmp/attachment-client-proof.json').write_text(json.dumps(
            {'sources': hashes, 'archive': archive_hash, 'baselines': MODES, 'mutations': len(specs)+1}, indent=2) + '\n')
        print(f'PASS {len(specs)+1} attachment client mutations, {len(MODES)} baselines and SHA256 rollback', flush=True)
    finally:
        restore()
    return 0


if __name__ == '__main__':
    sys.exit(main())
