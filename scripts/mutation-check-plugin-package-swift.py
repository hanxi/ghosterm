#!/usr/bin/env python3
"""Isolated Swift package facade mutations against an explicit enabled Core build."""
import argparse,hashlib,json,os,re,shutil,signal,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
FILE='apple/GhostermCore/Sources/GhostermCore/ExtensionPackage.swift'
MARK='GT_PACKAGE_SWIFT_MUTATION'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 global FILE
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--build',required=True,type=Path);parser.add_argument('--registry',action='store_true');args=parser.parse_args();build=args.build.resolve()
 cases=[('entry','entry: packageString(v.entry)','entry: "truncated"','testFixedPackageAdoptionAndRPC'),('capabilities','requires: names(v.requires, v.requires_count)','requires: []','testFixedPackageAdoptionAndRPC'),('grant','authorizationGranted: v.authorization_granted','authorizationGranted: true','testFixedPackageAdoptionAndRPC'),('stop','if let raw { gt_ext_package_stop(raw) }','if let raw { _ = raw }','testCapabilityAndOwnerLifetime')]
 if args.registry:
  FILE='apple/GhostermCore/Sources/GhostermCore/ExtensionRegistry.swift'
  cases=[
   ('registry-unknown','Status(rawValue: value.rawValue) ?? .invalid','value == GT_EXT_REGISTRY_UNKNOWN ? .denied : (Status(rawValue: value.rawValue) ?? .invalid)','testFixedPackageAdoptionAndRPC'),
   ('registry-denied','Status(rawValue: value.rawValue) ?? .invalid','value == GT_EXT_REGISTRY_DENIED ? .unknown : (Status(rawValue: value.rawValue) ?? .invalid)','testFixedPackageAdoptionAndRPC'),
   ('registry-command','command: ExtensionPackage.Command(contribution)','command: nil','testFixedPackageAdoptionAndRPC'),
  ]
 original=(ROOT/FILE).read_text();assert MARK not in original
 for _,old,_,_ in cases:assert original.count(old)==1,old
 hashes={str(p):sha(p) for p in [build/'libghosterm_core.a',build/'libghosterm_quickjs.a',build/'test_config']}
 work=Path(tempfile.mkdtemp(prefix='package-swift-',dir=ROOT/'tmp'));source=work/'source';logs=work/'logs';logs.mkdir();commands=[]
 shutil.copytree(ROOT/'apple/GhostermCore',source/'apple/GhostermCore',symlinks=True,ignore=shutil.ignore_patterns('.build'))
 (source/'core').mkdir();shutil.copytree(ROOT/'core/include',source/'core/include');(source/'core/build').symlink_to(build,target_is_directory=True);(source/'tmp').mkdir()
 inputs={str(p.relative_to(ROOT)):sha(p) for folder in ['apple/GhostermCore/Sources','apple/GhostermCore/Tests','core/include'] for p in (ROOT/folder).rglob('*') if p.is_file()}
 env={**os.environ,'TMPDIR':str(work),'DEVELOPER_DIR':'/Applications/Xcode.app/Contents/Developer','GHOSTERM_CORE_TEST_PLUGIN_JS':'1'}
 def run(label,cmd,red=False):
  shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require package-swift; shift; gt_timeout 240 "$@"'
  p=subprocess.run(['bash','-c',shell,'package-swift',str(ROOT),*map(str,cmd)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=250);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode))
  if red:assert p.returncode==1 and re.search(r'Executed 1 test, with 1 failure(?:s)? \(0 unexpected\)',out) and 'XCTAssert' in out and not any(x in out for x in ['emit-module command failed','Fatal error:','Signal ']),label+'\n'+out
  else:assert p.returncode==0,label+'\n'+out
  return out
 def test(label,method=None,red=False):
  out=run(label,['swift','test','--package-path',source/'apple/GhostermCore','--filter','ExtensionPackageTests'+('.'+method if method else '')],red)
  if not red:assert 'Executed '+str(1 if method else 2)+' tests' in out or 'Executed 1 test,' in out;assert 'skipped' not in out.lower()
 try:
  test('baseline')
  for label,old,new,method in cases:
   path=source/FILE
   try:
    path.write_text(original.replace(old,new)+'\n// '+MARK+'\n');test(label,method,True);run(label+'-control',[build/'test_config',ROOT/'core/tests/fixtures'])
   finally:path.write_text(original);assert sha(path)==sha(ROOT/FILE)
   test(label+'-restored',method);print(label+': XCTest assertion red / same archive config green / restore / SHA256',flush=True)
  test('final')
  for path,digest in hashes.items():assert sha(Path(path))==digest,path
  for path,digest in inputs.items():assert sha(ROOT/path)==digest,path
  (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=[c[0] for c in cases],inputs_sha256=inputs,prebuilt_sha256=hashes,commands=commands),indent=2)+'\n');print('PACKAGE SWIFT MUTATIONS PASS: '+str(logs),flush=True)
 except BaseException as error:(logs/'failure.txt').write_text(repr(error));raise
def stop(number,frame):raise KeyboardInterrupt(str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
