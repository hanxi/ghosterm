#!/usr/bin/env python3
"""Successful-key memory: compiled red, same APK Core green, restored green/SHA."""
import hashlib
import os
import signal

MARK = 'GT_ANDROID_REMEMBER_IDENTITY_MUTATION'

def execute(root, build, test):
    if 'RemoteFilesProductTest' not in os.environ.get('ANDROID_SSH_TEST_CLASS', ''):
        raise RuntimeError('Select RemoteFilesProductTest to prepare the real SFTP/key fixture')
    paths = {
        'store': root/'android/data/src/main/java/com/hanxi/ghosterm/data/RememberedSSHIdentities.kt',
        'sessions': root/'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt',
    }
    originals = {name: path.read_bytes() for name, path in paths.items()}
    hashes = {name: hashlib.sha256(value).hexdigest() for name, value in originals.items()}
    if any(MARK.encode() in value for value in originals.values()):
        raise RuntimeError('Residual remembered identity mutation')
    product = 'com.hanxi.ghosterm.RemoteFilesProductTest#'
    cases = [
        ('persist', 'store', 'if (remember)', 'if (false)',
         product+'successfulManualKeyIsRememberedAndNextConnectionNeedsNoSelection'),
        ('scope', 'store', '.digest(scope)', '.digest(byteArrayOf())',
         'com.hanxi.ghosterm.RememberedSSHIdentityTest#rememberedChoiceIsDurableScopedAndNeverMatchesAReplacementByName'),
        ('success-only', 'sessions', 'attempt.manuallySubmittedIdentity=!automatic',
         'attempt.manuallySubmittedIdentity=!automatic\n                rememberSuccessfulIdentity(attempt)',
         product+'rejectedOrCancelledKeyNeverBecomesRemembered'),
        ('opt-out', 'store', 'if (remember)', 'if (true)',
         product+'uncheckedRememberUsesKeyOnlyForThisConnection'),
    ]
    for name, filename, old, new, target in cases:
        if originals[filename].decode().count(old) != 1:
            raise RuntimeError('Missing/ambiguous remembered identity anchor: '+name)
    handlers = {s: signal.getsignal(s) for s in (signal.SIGINT, signal.SIGTERM)}
    def interrupted(signum, frame):
        raise KeyboardInterrupt('Remembered identity mutation interrupted')
    for signum in handlers:
        signal.signal(signum, interrupted)
    try:
        for name, filename, old, new, target in cases:
            path = paths[filename]
            label = 'remember-identity-'+name
            path.write_text(originals[filename].decode().replace(old, new)+'\n// '+MARK+'\n')
            build(label)
            output = test(label, target, False)
            if target.split('#')[1] not in output:
                raise RuntimeError('Remembered identity target not reached: '+name)
            test(label+'-control', 'com.hanxi.ghosterm.NativeSmokeTest#corePreservesUtf8AndLineEndings', True)
            path.write_bytes(originals[filename])
            if hashlib.sha256(path.read_bytes()).hexdigest() != hashes[filename]:
                raise RuntimeError('Remembered identity restore mismatch')
            build(label+'-restored')
            test(label+'-restored', target, True)
            print('PASS '+label+': compiled red / same APK Core green / restored green / SHA256', flush=True)
    finally:
        for name, path in paths.items():
            if MARK.encode() in path.read_bytes():
                path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).hexdigest() != hashes[name]:
                raise RuntimeError('Remembered identity source restoration failed')
        for signum, handler in handlers.items():
            signal.signal(signum, handler)
    print('ANDROID REMEMBER IDENTITY MUTATIONS PASS: 4 groups', flush=True)

if __name__ == '__main__':
    raise SystemExit('Use test-android-ssh.sh with ANDROID_REMEMBER_IDENTITY_MUTATE=1 and RemoteFilesProductTest selected')
