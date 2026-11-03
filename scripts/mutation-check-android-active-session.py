#!/usr/bin/env python3
"""Prove active-session snapshot and selected-UUID checks with real Android threads."""
from pathlib import Path
import hashlib,importlib.util,json,os,signal
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_ACTIVE_SESSION_MUTATION'
spec=importlib.util.spec_from_file_location('helper',ROOT/'scripts/mutation-check-android-native.py')
h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
def main():
 serial=os.environ.get('ANDROID_SERIAL')
 if not serial:raise RuntimeError('Set ANDROID_SERIAL')
 adb=['adb','-s',serial];path=ROOT/'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt';original=path.read_bytes();digest=hashlib.sha256(original).hexdigest()
 if MARK.encode() in original:raise RuntimeError('Residual active-session mutation')
 out=ROOT/'tmp/android-active-session-mutations';out.mkdir(parents=True,exist_ok=True)
 def build(label):
  p=h.run(['bash',ROOT/'scripts/build-android.sh'],900);(out/(label+'-build.log')).write_text(p.stdout+p.stderr);h.require(p)
  for file in ['apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk']:
   h.require(h.run([*adb,'install','-r','-t',ROOT/'android/app/build/outputs'/file]))
 def test(label,target,green,reason=None):
  p=h.run([*adb,'shell','am','instrument','-w','-r','-e','class','com.hanxi.ghosterm.'+target,'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],120)
  text=p.stdout+p.stderr;(out/(label+'.log')).write_text(text)
  ok=p.returncode==0 and 'OK (1 test)' in text and 'FAILURES' not in text
  if ok!=green or (not green and (p.returncode!=0 or 'FAILURES' not in text or 'AssertionError' not in text)) or (reason and reason not in text):raise RuntimeError(label+': '+text)
 target='SessionsSnapshotTest#activeSelectionSurvivesConcurrentMainThreadListChanges'
 control='NativeSmokeTest#corePreservesUtf8AndLineEndings'
 build('baseline');test('baseline',target,True);test('baseline-control',control,True)
 old='return sessions.toList().firstOrNull { it.id == id }'
 if original.decode().count(old)!=1:raise RuntimeError('Ambiguous active getter anchor')
 results=[]
 try:
  for label,new,reason in [
   ('live-iterator','return sessions.firstOrNull { it.id == id }','ConcurrentModificationException'),
   ('wrong-selection','return sessions.toList().firstOrNull { it.id != id }',None)]:
   path.write_text(original.decode().replace(old,new)+'\n// '+MARK+'\n');build(label)
   test(label+'-target',target,False,reason)
   if label=='live-iterator':test(label+'-second-target',target,False,reason)
   test(label+'-control',control,True)
   path.write_bytes(original)
   if hashlib.sha256(path.read_bytes()).hexdigest()!=digest:raise RuntimeError('Restore SHA256 mismatch')
   build(label+'-restored');test(label+'-restored',target,True)
   results.append(label);print(label+': compiled assertion red / same APK independent Core green / restored green / SHA256',flush=True)
 finally:
  if MARK.encode() in path.read_bytes():path.write_bytes(original)
  if hashlib.sha256(path.read_bytes()).hexdigest()!=digest:raise RuntimeError('Concurrent source change')
 (out/'result.json').write_text(json.dumps(dict(accepted=True,groups=results,source_sha256=digest),indent=2)+'\n')
 print('ANDROID ACTIVE SESSION MUTATIONS PASS: '+str(len(results))+' groups')
if __name__=='__main__':
 def interrupted(signum,frame):raise KeyboardInterrupt('signal '+str(signum))
 signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
 main()
