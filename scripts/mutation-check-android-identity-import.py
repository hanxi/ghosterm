#!/usr/bin/env python3
"""Real SAF/SSH product regressions with compiled mutations and same-APK config controls."""
import hashlib
MARK='GT_ANDROID_IDENTITY_IMPORT_MUTATION'

def execute(root,build,test):
    dialog=root/'android/app/src/main/java/com/hanxi/ghosterm/SSHConnectionDialog.kt'
    model=root/'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt'
    reader=root/'android/data/src/main/java/com/hanxi/ghosterm/data/IdentityDocumentReader.kt'
    originals={p:p.read_bytes() for p in (dialog,model,reader)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual import mutation')
    cases=[
      ('rotation',dialog,'val attempt = model.connecting ?: return',
       'DisposableEffect(model) { onDispose { model.completeIdentityImport(null) } }; val attempt = model.connecting ?: return',
       'realPickerImportSurvivesActivityRecreationAndAuthenticates',1),
      ('stale-result',model,'importIdentity(pending.first,pending.second,uri)',
       'connecting?.let { importIdentity(it.id,checkNotNull(it.auth),uri) }','oldPickerResultCannotImportIntoANewConnection',1),
      ('job-cancel',model,'attempt.credentialJob?.cancel();','Unit;',
       'cancelledImportClosesBlockedProviderPipeWithoutSaving',3),
      ('pipe-cancel',reader,'if (!waiting.isActive) return@dispatch\n                                try',
       'if (false && !waiting.isActive) return@dispatch\n                                try',
       'cancelledImportClosesBlockedProviderPipeWithoutSaving',1),
      ('readonly',reader,'openAssetFileDescriptor(uri,"r",signal)','openAssetFileDescriptor(uri,"rw",signal)',
       'realPickerImportSurvivesActivityRecreationAndAuthenticates',1),
      ('oversize',reader,'check(used in 1..1024*1024)','check(used >= 0)',
       'invalidDeniedAndOversizedDocumentsStayVisibleAndDoNotSave',1),
      ('cancel-result',model,'pendingIdentityImport = null; pendingIdentityPresenter = null; identityPickerPending = false',
       'pendingIdentityImport = null; pendingIdentityPresenter = null; identityPickerPending = true','pickerCancelDoesNotSaveAndAllowsAnotherImport',3),
    ]
    try:
        for label,path,old,new,method,count in cases:
            source=originals[path].decode()
            if source.count(old)!=count:raise RuntimeError('Ambiguous import mutation '+label)
            path.write_text(source.replace(old,new)+'\n// '+MARK+'\n')
            label='import-'+label
            build(label)
            test(label,'com.hanxi.ghosterm.SSHIdentityImportTest#'+method,False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored','com.hanxi.ghosterm.SSHIdentityImportTest#'+method,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Import source restoration failed')
    print(f'ANDROID IDENTITY IMPORT MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_SSH_IMPORT_MUTATE=1 ANDROID_SERIAL=... bash scripts/test-android-ssh.sh')
