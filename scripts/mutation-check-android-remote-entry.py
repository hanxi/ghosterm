#!/usr/bin/env python3
"""Real compiled entry/authentication-policy regressions, inside the SSH fixture."""
import hashlib,signal
MARK='GT_ANDROID_REMOTE_ENTRY_MUTATION'
def execute(root,build,test):
 model=root/'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt'
 screen=root/'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
 policy=root/'android/data/src/main/java/com/hanxi/ghosterm/data/IdentityReferences.kt'
 route='com.hanxi.ghosterm.RemoteFilesProductTest#terminalFilesUsesConnectedHostSnapshotAndConfiguredKeyWithoutSelectingAgain'
 choices='com.hanxi.ghosterm.RemoteFilesProductTest#automaticIdentityPolicyDoesNotGuessSkipEncryptedOrRepeatRejectedKeys'
 cases=[
 ('remote-route',screen,'TerminalScreen(sessions, ::openTerminalFiles)','TerminalScreen(sessions, files::enter)',route),
 ('snapshot',model,'attempt.target.copy(user=attempt.username,identityFiles=','attempt.target.copy(user=attempt.username,host="192.0.2.123",identityFiles=',route),
 ('automatic-key',model,'if(attempt.manualCredentials || attempt.credentialBusy','if(true || attempt.manualCredentials || attempt.credentialBusy',route),
 ('local-route',screen,'HostsPage(hosts, sessions, files::enterLocal)','HostsPage(hosts, sessions, files::enter)',route),
 ('no-guess',policy,'val first=configured.identities.firstOrNull','val first=(if(configured.identities.isEmpty()) available else configured.identities).firstOrNull',choices),
 ('order',policy,'val first=configured.identities.firstOrNull','val first=configured.identities.reversed().firstOrNull',choices),
 ('tried',policy,'it.id !in tried','true',choices),
 ('passphrase',policy,'return first.takeUnless { it.needsPassphrase }','return first',choices),
 ('none',policy,'if(configured.issues.isNotEmpty()) return null','if(false) return null',choices),
 ('missing',policy,'if(configured.issues.isNotEmpty()) return null','if(configured.issues.any { !it.startsWith("配置身份已移除") }) return null',choices),
 ]
 originals={p:p.read_bytes() for p in (model,screen,policy)};hashes={p:hashlib.sha256(v).digest() for p,v in originals.items()}
 if any(MARK.encode() in v for v in originals.values()):raise RuntimeError('Residual entry mutation')
 old_signals={s:signal.getsignal(s) for s in (signal.SIGINT,signal.SIGTERM)}
 def interrupted(signum,frame):raise KeyboardInterrupt('Entry mutation interrupted')
 for signum in old_signals:signal.signal(signum,interrupted)
 try:
  for label,path,old,new,target in cases:
   text=originals[path].decode();assert text.count(old)==1,(label,text.count(old))
   path.write_text(text.replace(old,new)+'\n// '+MARK+'\n');build('entry-'+label)
   output=test('entry-'+label,target,False)
   method=target.split('#')[1]
   if ('at com.hanxi.ghosterm.RemoteFilesProductTest.'+method+'(' not in output or
       not any(x in output for x in ['AssertionError','ComparisonFailure','ComposeTimeoutException'])):raise RuntimeError('Entry mutation did not fail at its ordinary target assertion: '+label)
   test('entry-'+label+'-control','com.hanxi.ghosterm.NativeSmokeTest#corePreservesUtf8AndLineEndings',True)
   path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
   build('entry-'+label+'-restored');test('entry-'+label+'-restored',target,True)
   print('PASS entry-'+label+': compiled target red / same APK Core green / restored green / SHA256',flush=True)
 finally:
  for path,data in originals.items():
   if MARK.encode() in path.read_bytes():path.write_bytes(data)
  for signum,handler in old_signals.items():signal.signal(signum,handler)
  if any(hashlib.sha256(p.read_bytes()).digest()!=digest for p,digest in hashes.items()):raise RuntimeError('Entry source restoration failed')
 test('entry-final-product','com.hanxi.ghosterm.RemoteFilesProductTest,com.hanxi.ghosterm.NativeSmokeTest#corePreservesUtf8AndLineEndings',True)
 print('ANDROID REMOTE ENTRY MUTATIONS PASS: '+str(len(cases))+' groups',flush=True)
if __name__=='__main__':raise SystemExit('Run through ANDROID_REMOTE_ENTRY_MUTATE=1 and test-android-ssh.sh')
