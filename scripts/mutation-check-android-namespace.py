#!/usr/bin/env python3
"""Explicit unknown-namespace acknowledgement; invoke through the SSH fixture."""
import hashlib
import os
import re

MARK = 'GT_ANDROID_NAMESPACE_MUTATION'

def execute(root,build,test):
    domain=root/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FilePasteCheck.kt'
    preflight=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FilePastePreflight.kt'
    executor=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileCopyExecutor.kt'
    model=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksViewModel.kt'
    screen=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksScreen.kt'
    originals={p:p.read_bytes() for p in (domain,preflight,executor,model,screen)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual namespace mutation')
    product='RemoteFileTasksProductTest#differentRemoteIdentityRequiresFreshAcknowledgementBeforeRealCopy'
    cases=[
        ('request-binding',domain,'candidate === request','true','FilePastePreflightTest#unknownRemoteConsentIsBoundToOneIntentAndRemainsExplicit'),
        ('explicit-consent',preflight,'namespaceConsent?.appliesTo(request)!=true','false','FilePastePreflightTest#unknownRemoteConsentIsBoundToOneIntentAndRemainsExplicit'),
        ('known-descendant',preflight,'stop(FilePasteCheckFailureKind.INSIDE_SOURCE,"不能将目录复制到自身或其子目录")','Unit','FilePastePreflightTest#namespaceConsentCannotAuthorizeKnownDescendantOrLocalUnknown'),
        ('execution-consent',executor,'namespaceConsent=plan.check.namespaceConsent','namespaceConsent=null',product),
        ('review-ownership',model,'task.namespaceReview!==review','false',product),
        ('screen-action',screen,'model.confirmNamespace(task,review)','Unit',product),
    ]
    selected=os.environ.get('ANDROID_FILE_NAMESPACE_CASES','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown namespace mutation')
        cases=[c for c in cases if c[0] in selected]
    for name,path,old,new,target in cases:
        if originals[path].decode().count(old)!=1:raise RuntimeError('Ambiguous mutation '+name)
    def one(label,target,green):
        output=test(label,'com.hanxi.ghosterm.'+target,green)
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output:raise RuntimeError(output)
        valid=('OK (1 test)' in output and 'FAILURES' not in output) if green else (bool(re.search(r'Tests run: 1,\s+Failures: 1',output)) and any(t in output for t in ('java.lang.AssertionError','org.junit.ComparisonFailure')))
        if not valid:raise RuntimeError(output)
    try:
        for name,path,old,new,target in cases:
            path.write_text(originals[path].decode().replace(old,new)+'\n// '+MARK+'\n')
            label='namespace-'+name
            build(label);one(label,target,False)
            one(label+'-control','ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');one(label+'-restored',target,True)
            print('PASS '+label+': compiled assertion red / same APK Core green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('Namespace source restoration failed')
    print(f'ANDROID NAMESPACE MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_FILE_NAMESPACE_MUTATE=1 with scripts/test-android-ssh.sh')
