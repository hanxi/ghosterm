#!/usr/bin/env python3
"""Compiled product/storage mutations inside the live Android SSH fixture runner."""
import hashlib
MARK='GT_ANDROID_SSH_PRODUCT_MUTATION'

def execute(root,build,test):
    vault=root/'android/data/src/main/java/com/hanxi/ghosterm/data/IdentityVault.kt'
    model=root/'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt'
    dialog=root/'android/app/src/main/java/com/hanxi/ghosterm/SSHConnectionDialog.kt'
    screen=root/'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    config=root/'android/bridge/src/main/cpp/config_jni.c'
    originals={p:p.read_bytes() for p in (vault,model,dialog,screen,config)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual product mutation')
    def v(method):return 'IdentityVaultTest#'+method
    def p(method):return 'SSHProductTest#'+method
    cases=[
      ('factory',model,'@JvmOverloads constructor','constructor',v('defaultFactoryHasApplicationConstructor'),1),
      ('ciphertext',vault,'val encrypted = cipher.doFinal(material)','val encrypted = material.copyOf()',v('encryptedRoundTripConsumesMaterialAndRemovesAccess'),1),
      ('aad',vault,'cipher.updateAAD(aad(identity))','Unit',v('changedMetadataAndCiphertextCannotUnlock'),2),
      ('wipe',vault,'material.fill(0)','Unit',v('encryptedRoundTripConsumesMaterialAndRemovesAccess'),1),
      ('cleanup',vault,'if (!committed && id != null)','if (false && !committed && id != null)',v('failedCommitRemovesNewKeyAndTemporaryContainer'),1),
      ('commit-state',vault,'committed = true','committed = false',v('syncFailureReportsCommittedIdentityWithoutDeletingItsKey'),1),
      ('missing-key',vault,'?: error("安全密钥不可用，请重新导入身份")','?: return@synchronized Base64.getDecoder().decode(json.getString("ciphertext"))',v('missingKeystoreKeyNeverRegeneratesOrFallsBack'),1),
      ('format',vault,'check(requirement in 0..1)','check(requirement in -1..1)',v('invalidOrNulMaterialIsRejectedAndCleared'),1),
      ('prepare',config,'gt_config_check_connection(cfg,alias,0,&issue)!=GT_OK','gt_config_check_connection(cfg,alias,0,&issue)==GT_ERR_INVALID_ARG','SSHConnectionModelTest#preparationRechecksIncludedConfigurationAndPreservesRawBytes',1),
      ('budget',model,'withTimeout(connectionBudgetMillis)','withTimeout(60000)','SSHConnectionModelTest#timeoutCancelsDnsAndShowsFailure',1),
      ('username',model,'if (target.user.isNotBlank()) startConnection(attempt)','startConnection(attempt)','SSHConnectionModelTest#missingUsernameDoesNotResolveUntilValidInputAndDoesNotMutateTarget',1),
      ('host-route',screen,'model.checkConnection(host, sessions::connect)','model.checkConnection(host) { _ -> }',p('homeTrustKeyAuthenticationAndRotationKeepOneRemoteShell'),1),
      ('reject-button',dialog,'model.decide(attempt.id,prompt!!,false)','model.decide(attempt.id,prompt!!,true)',p('refusalNeverRequestsCredentialsOrWritesTrust'),1),
      ('fingerprint',dialog,'Text(prompt.fingerprint,','Text("fingerprint unavailable",',p('homeTrustKeyAuthenticationAndRotationKeepOneRemoteShell'),1),
      ('multisession',model,'else choosingRemote = target.alias','else show(existing.last().id)',p('explicitNewConnectionAndHostChoiceDoNotReplaceLocalOrRemoteOwners'),1),
    ]
    try:
        for label,path,old,new,method,count in cases:
            text=originals[path].decode()
            if text.count(old)!=count:raise RuntimeError('Ambiguous product mutation '+label)
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            label='product-'+label
            build(label)
            test(label,'com.hanxi.ghosterm.'+method,False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored','com.hanxi.ghosterm.'+method,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Product source restoration failed')
    print(f'ANDROID SSH PRODUCT MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_SSH_PRODUCT_MUTATE=1 ANDROID_SERIAL=... bash scripts/test-android-ssh.sh')
