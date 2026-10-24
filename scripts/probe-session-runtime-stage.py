#!/usr/bin/env python3
"""Runtime inventory publication semantics (metadata fixtures, not binary proof)."""
import contextlib,hashlib,json,os,runpy,shutil,stat,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MODULE=runpy.run_path(os.environ.get('GHOSTERM_RUNTIME_STAGE_SCRIPT',str(ROOT/'scripts/stage-session-runtime.py')))
@contextlib.contextmanager
def fixture():
 root=Path(tempfile.mkdtemp(prefix='runtime-stage-test-',dir=ROOT/'tmp'))
 try:
  native=root/'native';linux=root/'linux';native.mkdir();linux.mkdir();contract='a'*64;release='fixture-release'
  hashes={}
  for name in MODULE['MAC_FILES']:
   path=native/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(('native '+name).encode());path.chmod(0o600);hashes[name]=hashlib.sha256(path.read_bytes()).hexdigest()
  (native/'runtime.json').write_text(json.dumps(dict(format=1,platform='macos',arch='arm64',contract=contract,release=release,files=hashes)));(native/'runtime.json').chmod(0o600)
  lines=['GTB1\t'+release+'\taarch64\t2.34\t/lib/ld-linux-aarch64.so.1\t'+contract]
  for name in MODULE['LINUX_FILES']:
   path=linux/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(('linux '+name).encode());path.chmod(0o600);lines.append(name+'\t'+str(path.stat().st_size)+'\t'+hashlib.sha256(path.read_bytes()).hexdigest())
  (linux/'manifest.tsv').write_text('\n'.join(lines)+'\n');(linux/'manifest.tsv').chmod(0o600)
  (linux/'manifest.json').write_text('{}');(linux/'manifest.json').chmod(0o600)
  yield root,native,linux,contract
 finally:shutil.rmtree(root)
class RuntimeStageTests(unittest.TestCase):
 def test_matching_inventory(self):
  with fixture() as (root,native,linux,contract):
   self.assertEqual(MODULE['macos'](native)['contract'],contract)
   self.assertEqual(MODULE['linux'](linux,contract),('aarch64','fixture-release'))
 def test_native_manifest_digest_and_extra_file_rejected(self):
  with fixture() as (root,native,linux,contract):
   path=native/'runtime.json';meta=json.loads(path.read_text());del meta['files']['SOURCE-INFO.json'];path.write_text(json.dumps(meta))
   with self.assertRaises(RuntimeError):MODULE['macos'](native)
 def test_native_tamper_and_unsafe_file(self):
  with fixture() as (root,native,linux,contract):
   path=native/'ghosterm-sessiond';kept=path.read_bytes();path.write_bytes(b'bad')
   with self.assertRaises(RuntimeError):MODULE['macos'](native)
   path.write_bytes(kept);path.chmod(0o666)
   with self.assertRaises(RuntimeError):MODULE['macos'](native)
   path.chmod(0o600);target=root/'kept';path.rename(target);path.symlink_to(target)
   with self.assertRaises(RuntimeError):MODULE['macos'](native)
 def test_linux_contract_and_duplicate_inventory(self):
  with fixture() as (root,native,linux,contract):
   with self.assertRaises(RuntimeError):MODULE['linux'](linux,'b'*64)
   path=linux/'manifest.tsv';lines=path.read_text().splitlines();lines[1]=lines[2];path.write_text('\n'.join(lines)+'\n')
   with self.assertRaises(RuntimeError):MODULE['linux'](linux,contract)
 def test_linux_checksum_and_size(self):
  with fixture() as (root,native,linux,contract):
   path=linux/'ghosterm-sessiond';data=path.read_bytes();path.write_bytes(b'x'*len(data))
   with self.assertRaises(RuntimeError):MODULE['linux'](linux,contract)
 def test_existing_release_never_overwritten(self):
  with fixture() as (root,native,linux,contract):
   target=root/'published';MODULE['copy_release'](native,target,True)
   self.assertEqual((target/'ghosterm-sessiond').stat().st_mode&0o777,0o755)
   self.assertEqual((target/'LICENSE').stat().st_mode&0o777,0o644)
   MODULE['copy_release'](native,target,True)
   file=target/'ghosterm-session-proxy';file.write_bytes(b'changed')
   with self.assertRaises(RuntimeError):MODULE['copy_release'](native,target,True)
   self.assertEqual(file.read_bytes(),b'changed')
 def test_release_token_and_destination_symlink(self):
  with fixture() as (root,native,linux,contract):
   path=native/'runtime.json';meta=json.loads(path.read_text());meta['release']='../outside';path.write_text(json.dumps(meta))
   with self.assertRaises(RuntimeError):MODULE['macos'](native)
   alias=root/'alias';alias.symlink_to(native,target_is_directory=True)
   with self.assertRaises(RuntimeError):MODULE['copy_release'](native,alias,True)
if __name__=='__main__':unittest.main()
