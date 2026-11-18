#!/usr/bin/env python3
"""Explicit local app-server protocol fixture. No model, network, CLI impersonation evidence or patch application."""
import json
import os
from pathlib import Path
import select
import sys

THREAD = 'thread-fixture'
TURN = 'turn-fixture'
ITEM = 'patch-fixture'
APPROVAL = 'file:审批'


def diff(revision):
    return '@@ -1 +1 @@\n' + ''.join(f'+REV-{revision} row-{row} 中文😀\n' for row in range(600)) + f'+FINAL-REV-{revision}\n'


def changes(revision):
    return [dict(path=('nested/' + 'x' * 4096 + '/source.swift') if index == 0 else f'src/file-{index}.swift',
                 kind={'type': 'update', 'move_path': None}, diff=diff(revision)) for index in range(10)]


def turn(status):
    return dict(id=TURN, rootTurnId=None, items=[], itemsView='full', status=status, error=None,
                startedAt=1, completedAt=None if status == 'inProgress' else 2,
                durationMs=None if status == 'inProgress' else 1000)


def send(value):
    sys.stdout.write(json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n')
    sys.stdout.flush()


def notification(method, params):
    send(dict(method=method, params=params))


def receipt(name, value):
    pending = Path(name + '.pending')
    pending.write_text(json.dumps(value, ensure_ascii=False))
    pending.replace(name)


def main():
    if sys.argv[1:] == ['--version']:
        print('Ghosterm isolated app-server protocol fixture (no model)')
        return
    workspace = Path.cwd()
    if sys.argv[1:] != ['app-server'] or not workspace.parent.name.startswith('codex-patch-') or \
            json.loads(Path('fixture.json').read_text()) != {'fixture': 'ghosterm-patch-rpc', 'version': 1}:
        raise RuntimeError('Run only in the explicit Ghosterm disposable fixture')
    revision = 1
    running = False
    interrupt_fixture = Path("interrupt-turn").exists()
    interrupted = False
    pending = bytearray()
    receipt('started.json', {'fixture': 'ghosterm-patch-rpc'})
    while True:
        if interrupted and Path('complete-interrupt').exists():
            notification('turn/completed', dict(threadId=THREAD, turn=turn('interrupted')))
            running = False
            interrupted = False
        if running and revision == 1 and Path('request-update').exists():
            revision = 2
            notification('item/fileChange/patchUpdated', dict(threadId=THREAD, turnId=TURN, itemId=ITEM, changes=changes(revision)))
            receipt('updated.json', {'revision': revision})
        newline = pending.find(b'\n')
        if newline < 0:
            readable, _, _ = select.select([sys.stdin.fileno()], [], [], .02)
            if not readable:
                continue
            chunk = os.read(sys.stdin.fileno(), 16384)
            if not chunk:
                return
            pending.extend(chunk)
            if len(pending) > 1048576:
                raise RuntimeError('Fixture request exceeded its line budget')
            continue
        line = bytes(pending[:newline])
        del pending[:newline + 1]
        message = json.loads(line)
        method = message.get('method')
        if method == 'initialize':
            send(dict(id=message['id'], result={'userAgent': 'codex/0.166.0 (Ghosterm protocol fixture; no model)'}))
        elif method == 'initialized':
            pass
        elif method == 'thread/start':
            thread = dict(id=THREAD, sessionId=THREAD, forkedFromId=None, parentThreadId=None, preview='', ephemeral=True,
                          section=None, sectionEnteredAt=None, projectId=None, historyMode='legacy', modelProvider='fixture',
                          model='fixture-no-model', reasoningEffort=None, createdAt=1, updatedAt=1, recencyAt=None,
                          status={'type': 'idle'}, path=None, cwd=str(workspace), cliVersion='0.166.0', originator='fixture',
                          source='appServer', threadSource=None, agentNickname=None, agentRole=None, gitInfo=None, name=None, turns=[])
            send(dict(id=message['id'], result=dict(thread=thread, model='fixture-no-model', modelProvider='fixture',
                serviceTier=None, disabledPluginIds=[], cwd=str(workspace), instructionSources=[], approvalPolicy='on-request',
                approvalsReviewer='user', sandbox={'type': 'readOnly', 'networkAccess': False}, reasoningEffort=None)))
            notification('thread/started', {'thread': thread})
        elif method == 'turn/start':
            if Path('drop-turn-reply').exists():
                receipt('turn-received.json', {'received': True})
                return
            if running or message['params']['threadId'] != THREAD:
                raise RuntimeError('Fixture accepts one explicit turn')
            running = True
            notification('turn/started', dict(threadId=THREAD, turn=turn('inProgress')))
            if interrupt_fixture:
                send(dict(id=message['id'], result={'turn': turn('inProgress')}))
                continue
            notification('item/started', dict(threadId=THREAD, turnId=TURN, startedAtMs=1,
                item=dict(type='fileChange', id=ITEM, changes=changes(revision), status='inProgress')))
            send(dict(id=APPROVAL, method='item/fileChange/requestApproval',
                params=dict(threadId=THREAD, turnId=TURN, itemId=ITEM, startedAtMs=1)))
            send(dict(id=message['id'], result={'turn': turn('inProgress')}))
        elif method == 'turn/interrupt' and interrupt_fixture:
            if not running or interrupted or message['params'] != {'threadId': THREAD, 'turnId': TURN}:
                raise RuntimeError('Unexpected fixture interrupt')
            interrupted = True
            receipt('interrupt-received.json', {'received': True})
            send(dict(id=message['id'], result={}))
        elif message.get('id') == APPROVAL and 'result' in message:
            if not running or revision != 2 or message['result'] != {'decision': 'accept'}:
                raise RuntimeError('Unexpected approval before the current fixture revision')
            receipt('approved.json', {'request': message, 'revision': revision})
            notification('serverRequest/resolved', dict(threadId=THREAD, requestId=APPROVAL))
            notification('item/completed', dict(threadId=THREAD, turnId=TURN, completedAtMs=2,
                item=dict(type='fileChange', id=ITEM, changes=changes(revision), status='completed')))
            notification('turn/completed', dict(threadId=THREAD, turn=turn('completed')))
            running = False
        else:
            raise RuntimeError('Unexpected fixture protocol input: ' + repr(message))


if __name__ == '__main__':
    main()
