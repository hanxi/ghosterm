#!/usr/bin/env python3
"""Offline checks for the trusted metadata capsule; never runs JS/Node."""
import argparse,ast,hashlib,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
INPUTS={'packages/plugin-sdk/schema/'+s+'-v1.candidate.json' for s in ['bridge','manifest','integrity']}|{'packages/plugin-sdk/testkit/package-metadata.mjs','scripts/generate-plugin-package-validator.mjs','scripts/fixtures/plugin-package-tools/package.json','scripts/fixtures/plugin-package-tools/package-lock.json'}
BASE='core/src/extension/package-validator'
def pairs(items):
 d={}
 for k,v in items:
  if k in d:raise ValueError('duplicate metadata capsule key: '+k)
  d[k]=v
 return d
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--root',type=Path,default=ROOT);args=parser.parse_args();root=args.root.resolve();manifest=json.loads((root/(BASE+'-inputs.json')).read_text(),object_pairs_hook=pairs)
 assert manifest['format']==1 and set(manifest['inputs'])==INPUTS
 assert set(manifest['outputs'])=={BASE+'-code.inc',BASE+'-LICENSES.txt'}
 for group in ['inputs','outputs']:
  for name,digest in manifest[group].items():assert re.fullmatch('[0-9a-f]{64}',digest) and hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,'metadata capsule drift: '+name
 assert manifest['versions']=={'ajv':'8.17.1','semver':'7.7.3','fast-deep-equal':'3.1.3'}
 code=(root/(BASE+'-code.inc')).read_text();chunks=[]
 for line in code.splitlines():
  if line.startswith('"'):
   value=ast.literal_eval(line.removesuffix(','));assert isinstance(value,str);chunks.append(value.encode('latin1'))
 blob=b''.join(chunks);assert len(blob)==manifest['bundleBytes'] and 1<=len(blob)<=1048576 and b'\0' not in blob
 assert hashlib.sha256(blob).hexdigest()==manifest['bundleSHA256'] and '#define GT_PACKAGE_VALIDATOR_BYTES '+str(len(blob))+'u' in code
 for name,digest in manifest['modules'].items():assert not name.startswith(('/', '../')) and '..' not in name.split('/') and re.fullmatch('[0-9a-f]{64}',digest)
 print('PACKAGE VALIDATOR OFFLINE INPUTS PASS: '+str(len(chunks))+' chunks / '+str(len(manifest['modules']))+' closed source modules')
if __name__=='__main__':main()
