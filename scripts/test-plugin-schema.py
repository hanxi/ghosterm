#!/usr/bin/env python3
"""Validate plugin protocol schemas and mutations using Python jsonschema."""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def frame_bytes(frame):
    return len(json.dumps(frame, ensure_ascii=False, separators=(',', ':')).encode('utf-8'))


def check_views(validator):
    table = {'kind': 'table', 'viewId': 'a', 'methods': {'describe': 'table.describe', 'read': 'table.page'}}
    diff = {'kind': 'diff', 'viewId': 'b', 'methods': {'describe': 'diff.describe', 'read': 'diff.page'}}
    task = {'kind': 'task', 'viewId': 'c', 'methods': {
        'describe': 'task.describe', 'read': 'task.read', 'cancel': 'task.cancel'}}
    valid = {'table': table, 'diff': diff, 'task': task}
    invalid = {
        'empty': {}, 'null': None,
        'unknown-kind': table | {'kind': 'chat'},
        'object-kind': table | {'kind': {}},
        'empty-id': table | {'viewId': ''},
        'id-newline': table | {'viewId': 'x\n'},
        'non-ascii-id': table | {'viewId': '😀'},
        'long-id': table | {'viewId': 'x' * 65},
        'extra-member': table | {'extra': True},
        'table-cancel': table | {'methods': table['methods'] | {'cancel': 'task.cancel'}},
        'task-no-cancel': task | {'methods': table['methods']},
        'uppercase-method': table | {'methods': table['methods'] | {'describe': 'Node.describe'}},
        'long-method': table | {'methods': table['methods'] | {'describe': 'a' * 129}},
    }
    for name, view in valid.items():
        assert validator.is_valid(view), name
    for name, view in invalid.items():
        assert not validator.is_valid(view), name
    return len(valid) + len(invalid)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutate', action='store_true')
    args = parser.parse_args()
    try:
        from fixtures.plugin_wire_schema import validator_for
    except ImportError:
        parser.error('the Python jsonschema package is required')
    schema_dir = ROOT / 'packages/plugin-protocol/schema'
    schema_file = schema_dir / 'bridge-v1.candidate.json'
    case_file = schema_dir / 'bridge-v1.cases.json'
    views_file = schema_dir / 'views-v1.candidate.json'
    inputs = (schema_file, case_file, views_file, Path(__file__), ROOT / 'scripts/fixtures/plugin_wire_schema.py')
    originals = {path: path.read_bytes() for path in inputs}
    schema = json.loads(originals[schema_file])
    cases = json.loads(originals[case_file])
    frames = {'valid-' + name: (deepcopy(cases['base'] | message), True)
              for name, message in cases['messages'].items()}
    for patch in cases['patches']:
        frame = deepcopy(cases['base'] | cases['messages'][patch['from']])
        target = frame
        for segment in patch['path'][:-1]:
            target = target[segment]
        field = patch['path'][-1]
        if patch.get('remove'):
            del target[field]
        else:
            target[field] = deepcopy(patch['value'])
        assert patch['name'] not in frames
        frames[patch['name']] = (frame, patch['valid'])

    def matches(validator, name):
        frame, expected = frames[name]
        return (frame_bytes(frame) <= 65536 and validator.is_valid(frame)) == expected

    validator = validator_for(schema)
    for name in frames:
        assert matches(validator, name), name
    large = deepcopy(frames['valid-request'][0])
    large['payload']['input'] = '界' * 22000
    assert frame_bytes(large) > 65536 and validator.is_valid(large)
    offer_bytes = frame_bytes(frames['valid-offer'][0])
    assert offer_bytes <= 768
    print(f'PASS schema {len(frames)} instances; UTF-8 envelope budget; offer {offer_bytes} bytes')
    view_instances = check_views(validator_for(json.loads(originals[views_file])))
    print(f'PASS Views schema {view_instances} instances')

    changes = {
        'unknown-root-field': ('additionalProperties', True),
        'wire-version': ('properties', schema['properties'] | {'wireVersion': {'type': 'integer'}}),
        'leading-zero-epoch': ('$defs', schema['$defs'] | {'counter': schema['$defs']['counter'] | {'pattern': '^[0-9]+$'}}),
        'valid-response-unknown': ('$defs', schema['$defs'] | {
            'outcome': schema['$defs']['outcome'] | {'oneOf': schema['$defs']['outcome']['oneOf'][:-1]}
        }),
    }
    if args.mutate:
        for name, (key, replacement) in changes.items():
            altered = validator_for(schema | {key: replacement})
            assert not matches(altered, name), f'schema mutation survived: {name}'
            assert matches(altered, 'valid-offer') and matches(validator, name)
            print(f'PASS schema mutation {name}: named assertion red / offer green / restored green')
    for path, original in originals.items():
        assert path.read_bytes() == original
    print(json.dumps(dict(wireVersion=1, frozenEnvelope=True, instances=len(frames), views=view_instances, offerBytes=offer_bytes,
                          mutations=len(changes) if args.mutate else 0,
                          inputSHA256={str(p.relative_to(ROOT)): hashlib.sha256(b).hexdigest()
                                       for p, b in originals.items()}), sort_keys=True))


if __name__ == '__main__':
    main()
