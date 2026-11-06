#!/usr/bin/env python3
"""Offline source/generated checks for the host-owned portable SDK."""
import argparse, ast, hashlib, json, re
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
BASE = 'core/src/extension/sdk-'
NAMES = ['codec', 'index', 'resources', 'session', 'wire-contract']
INPUTS = {'packages/plugin-sdk/portable/'+n+'.ts' for n in NAMES} | {
    'packages/plugin-sdk/tsconfig.json', 'packages/plugin-sdk/sdk-version.json',
    'scripts/fixtures/plugin-sdk-tools/package.json', 'scripts/fixtures/plugin-sdk-tools/package-lock.json',
    'scripts/generate-plugin-runtime-sdk.mjs'}
def sha(data): return hashlib.sha256(data).hexdigest()
def pairs(items):
    out = {}
    for k, v in items:
        if k in out: raise ValueError('duplicate SDK input key: '+k)
        out[k] = v
    return out
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    root = parser.parse_args().root.resolve()
    manifest = json.loads((root/(BASE+'inputs.json')).read_text(), object_pairs_hook=pairs)
    assert manifest['format'] == 1 and manifest['typescript'] == '5.9.3'
    assert manifest['sdkApiVersion'] == '1.0.0' and manifest['bridgeProtocol'] == 1 and manifest['candidate'] is True
    assert set(manifest['inputs']) == INPUTS
    assert set(manifest['outputs']) == {BASE+'code.inc', BASE+'identity.h'}
    for group in ['inputs', 'outputs']:
        for name, digest in manifest[group].items():
            assert re.fullmatch('[0-9a-f]{64}', digest) and sha((root/name).read_bytes()) == digest, name
    records = manifest['modules']
    assert [m['name'] for m in records] == ['sdk:/'+n+'.js' for n in NAMES]
    code = (root/(BASE+'code.inc')).read_text()
    groups = re.findall(r'static const char \*const sdk_chunks_\d+\[\]=\{\n(.*?)\n\};', code, re.S)
    assert len(groups) == len(records)
    for block, record in zip(groups, records):
        blob = b''.join(ast.literal_eval(line.removesuffix(',')).encode('latin1') for line in block.splitlines())
        assert len(blob) == record['bytes'] and 0 < len(blob) <= 131072 and b'\0' not in blob
        assert sha(blob) == record['sha256']
    framing = dict(sdkApiVersion=manifest['sdkApiVersion'], bridgeProtocol=manifest['bridgeProtocol'], modules=records)
    digest = sha(json.dumps(framing, separators=(',', ':')).encode())
    assert digest == manifest['sdkDigest']
    header = (root/(BASE+'identity.h')).read_text()
    assert '#define GT_PLUGIN_SDK_DIGEST "'+digest+'"' in header
    print('RUNTIME SDK OFFLINE INPUTS PASS: 5 modules / '+digest)
if __name__ == '__main__': main()
