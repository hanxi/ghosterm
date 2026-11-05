#!/usr/bin/env python3
"""Independent hashlib reference for private C SHA-256, not a crypto certification.
The C executable also runs real anonymous-file storage tests in Core CTest.
"""
from pathlib import Path
import argparse, hashlib, json, os, random, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,default=ROOT/'core/build/test_bridge_resource')
    args=parser.parse_args();binary=args.binary.resolve()
    work=Path(tempfile.mkdtemp(prefix='plugin-resource-hash-',dir=ROOT/'tmp'))
    random_bytes=random.Random(0x5245534f55524345)
    sizes=sorted(set([0,1,2,3,7,31,32,55,56,57,63,64,65,119,120,127,128,129,997,998,1024,16383,16384,65536,262144,1000000]+[random_bytes.randrange(65537) for _ in range(40)]))
    cases=[('bytes-'+str(n),random_bytes.randbytes(n)) for n in sizes]+[('million-a',b'a'*1000000),('abc',b'abc')]
    commands=[]
    for name,data in cases:
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-resource-hash; shift; gt_timeout 15 "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-resource-hash',str(ROOT),str(binary),'--hash'],input=data,capture_output=True,env={**os.environ,'TMPDIR':str(work)},timeout=25)
        expected=hashlib.sha256(data).hexdigest().encode()+b'\n'
        if p.returncode or p.stderr or p.stdout!=expected:
            (work/'failure.log').write_bytes(p.stdout+p.stderr)
            raise RuntimeError(f'{name}: C/hashlib mismatch rc={p.returncode}; {p.stdout!r} {p.stderr!r}')
        commands.append(dict(name=name,bytes=len(data),matched=True))
    result=dict(accepted=True,cases=commands,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),inputs_sha256={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in ['core/src/base/sha256.c','core/src/base/sha256.h','core/tests/test_bridge_resource.c','scripts/test-plugin-resource.py']})
    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(f'PLUGIN RESOURCE HASH PASS: {len(cases)} independent hashlib cases; logs: {work}',flush=True)
if __name__=='__main__':main()
