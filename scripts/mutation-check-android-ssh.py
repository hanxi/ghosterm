#!/usr/bin/env python3
"""Real device/native auth mutations; invoked inside test-android-ssh.py's live fixture."""
import hashlib
from pathlib import Path
MARK='GT_ANDROID_SSH_MUTATION'

def execute(root, build, test):
    native=root/'android/bridge/src/main/cpp/ssh_session.c'
    session=root/'android/bridge/src/main/cpp/session_jni.c'
    kotlin=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/NativeSession.kt'
    binding=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteSession.kt'
    originals={p:p.read_bytes() for p in (native,session,kotlin,binding)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual mutation')
    vt='unknownHostTrustKeyAuthenticationAndRemotePtyReachVt'
    cases=[
      ('deferred',native,'    s->options.defer_credentials = true;\n','    s->options.defer_credentials = false;\n',vt),
      ('identity',native,'    gt_ssh_opts_t opts = {\n        .host = s->host, .port = s->port, .username = s->user, .known_hosts_path = s->known_hosts};\n','    gt_ssh_opts_t opts = {.host = "127.0.0.1",\n                          .port = s->port,\n                          .username = s->user,\n                          .known_hosts_path = s->known_hosts};\n',vt),
      ('rejection',native,'                status = gt_ssh_authenticator_decide(\n                    auth, response == 1 ? GT_HOSTKEY_ACCEPT : GT_HOSTKEY_REJECT);\n','                status = gt_ssh_authenticator_decide(auth, GT_HOSTKEY_ACCEPT);\n','rejectionStopsBeforeCredentialsAndDoesNotSaveTrust'),
      ('save-error',native,'                info.error = status;\n','                info.error = GT_ERR_HOST_KEY;\n','savingTrustFailureStopsBeforeCredentials'),
      ('revoked',binding,'SSHHostState.entries[f[3].toInt()]','SSHHostState.entries[if (f[3] == "4") 2 else f[3].toInt()]','revokedMismatchAndUnsupportedStayDistinct'),
      ('methods',native,'    snprintf(number[5], sizeof number[5], "%d", info.auth_methods);\n','    snprintf(number[5], sizeof number[5], "%d", 0);\n',vt),
      ('stale-request',native,'    bool valid = s->waiting && !s->response && (uint64_t)request == s->request && phase;\n','    bool valid = s->waiting && !s->response && request > 0 && phase;\n','wrongKeyRetriesWithNewRequestAndClearsSecrets'),
      ('request-generation',native,'        s->request++;\n','        s->request = 1;\n','wrongKeyRetriesWithNewRequestAndClearsSecrets'),
      ('owner',kotlin,' && prompt.owner == handle','', 'promptsCannotCrossSessionsAndUnsupportedPasswordsAreCleared'),
      ('wipe',kotlin,'first.fill(0); second.fill(0)','second.fill(0)',vt),
      ('deadline',native,'    s->options.timeout_ms = (uint32_t)timeout;\n','    s->options.timeout_ms = 60000;\n','deadlineContinuesWhileWaitingForCredentials'),
      ('feed',session,'gat_surface_feed(s->surface, output, (size_t)length);','if (!s->remote) gat_surface_feed(s->surface, output, (size_t)length);',vt),
      ('resize',session,'            if ((s->transport &&\n','            if ((!s->remote && s->transport &&\n',vt),
      ('cancel',session,'        atomic_store(&s->stop, true);\n','        atomic_store(&s->stop, false);\n','cancelInvalidatesHandleAndReleasesNativeOwner'),
    ]
    try:
        for label,path,old,new,method in cases:
            text=originals[path].decode()
            if text.count(old)!=1:raise RuntimeError('Ambiguous mutation: '+label)
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            build(label)
            test(label,'com.hanxi.ghosterm.RemoteTerminalTest#'+method,False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path])
            assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored','com.hanxi.ghosterm.RemoteTerminalTest#'+method,True)
            print('PASS Android SSH '+label+': compiled assertion red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for p,data in originals.items():
            if MARK.encode() in p.read_bytes():p.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Source restoration failed')
    print(f'ANDROID SSH MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    raise SystemExit('Use ANDROID_SSH_MUTATE=1 ANDROID_SERIAL=... bash scripts/test-android-ssh.sh')
