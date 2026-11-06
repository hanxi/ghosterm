#!/usr/bin/env python3
"""Compiled product/storage mutations inside the live Android SSH fixture runner."""
import hashlib
import os
import signal
MARK='GT_ANDROID_SSH_PRODUCT_MUTATION'

def execute(root,build,test):
    vault=root/'android/data/src/main/java/com/hanxi/ghosterm/data/IdentityVault.kt'
    model=root/'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt'
    dialog=root/'android/app/src/main/java/com/hanxi/ghosterm/SSHConnectionDialog.kt'
    screen=root/'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    layout=root/'android/app/src/main/java/com/hanxi/ghosterm/SSHConnectionLayout.kt'
    config=root/'android/bridge/src/main/cpp/config_jni.c'
    originals={p:p.read_bytes() for p in (vault,model,dialog,screen,layout,config)}
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
      ('prepare',config,'    if (gt_config_check_connection(cfg, alias, 0, &issue) != GT_OK) {\n','    if (gt_config_check_connection(cfg, alias, 0, &issue) == GT_ERR_INVALID_ARG) {\n','SSHConnectionModelTest#preparationRechecksIncludedConfigurationAndPreservesRawBytes',1),
      ('budget',model,'withTimeout(connectionBudgetMillis) {\n                    val addresses =','withTimeout(60000) {\n                    val addresses =','SSHConnectionModelTest#timeoutCancelsDnsAndShowsFailure',1),
      ('username',model,'if (target.user.isNotBlank()) startConnection(attempt)','startConnection(attempt)','SSHConnectionModelTest#missingUsernameDoesNotResolveUntilValidInputAndDoesNotMutateTarget',1),
      ('host-route',screen,'model.checkConnection(host, sessions::connect)','model.checkConnection(host) { _ -> }',p('homeTrustKeyAuthenticationAndRotationKeepOneRemoteShell'),1),
      ('reject-button',dialog,'model.decide(attempt.id,prompt!!,false)','model.decide(attempt.id,prompt!!,true)',p('refusalNeverRequestsCredentialsOrWritesTrust'),1),
      ('fingerprint',dialog,'Text(prompt.fingerprint,','Text("fingerprint unavailable",',p('homeTrustKeyAuthenticationAndRotationKeepOneRemoteShell'),1),
      ('multisession',model,'else choosingRemote = target.alias','else show(existing.last().id)',p('explicitNewConnectionAndHostChoiceDoNotReplaceLocalOrRemoteOwners'),1),
      ('passive-dialog',dialog,'if (attempt.passiveProgress) return',
       'if (attempt.filesReady != null && attempt.passiveProgress) return','SSHConnectionPresentationTest#passiveStagesStayInOnePageIndicatorAndCancelCreatesNoSession',1),
      ('progress-route',screen,'SSHConnectionLayout(sessions, fileTasks.authentication.takeIf { fileTasks.presentsAuthentication }) {','SSHConnectionLayout(fileTasks.authentication) {',
       'SSHConnectionPresentationTest#passiveStagesStayInOnePageIndicatorAndCancelCreatesNoSession',1),
      ('progress-cancel',layout,'onClick = { owner.cancelConnection(attempt.id) }','onClick = { }',
       'SSHConnectionPresentationTest#passiveStagesStayInOnePageIndicatorAndCancelCreatesNoSession',1),
      ('progress-back',layout,'BackHandler(attempt != null)','BackHandler(false)',
       'SSHConnectionPresentationTest#backCancelsTheAttemptBeforePageNavigation',1),
      ('automatic-progress',model,'val automaticAuthentication = auth.phase','val automaticAuthentication = attempt.filesReady != null && auth.phase',
       p('configuredKeyKeepsCredentialStagesInlineAndCreatesOneShell'),1),
    ]
    cases += [
      ('task-automatic-progress',model,'publishAuthentication(attempt,auth,listed)\n                        val state',
       'attempt.auth = auth; attempt.message = auth.description\n                        val state',
       'RemoteFileTasksProductTest#rememberedBrowserIdentityAuthenticatesIndependentTaskWithoutSelectingAgain',1),
      ('task-passive-dialog',dialog,'if (attempt.passiveProgress) return',
       'if (attempt.taskLabel == null && attempt.passiveProgress) return',
       'FileConnectionPresentationTest#copyRolesUseInlineProgressAndCancelTheTask',1),
      ('task-progress-route',layout,'taskModel != null) taskModel else model','taskModel != null) model else model',
       'FileConnectionPresentationTest#copyRolesUseInlineProgressAndCancelTheTask',1),
      ('task-progress-cancel',layout,'onClick = { owner.cancelConnection(attempt.id) }','onClick = { model.cancelConnection(attempt.id) }',
       'FileConnectionPresentationTest#copyRolesUseInlineProgressAndCancelTheTask',1),
      ('tmux-passive-dialog',dialog,'if (attempt.passiveProgress) return',
       'if (attempt.tmuxReady == null && attempt.passiveProgress) return',
       'SSHConnectionPresentationTest#tmuxManagementUsesInlineProgressAndBackCancels',1),
      ('tmux-progress-route',layout,'it.filesReady == null && it.passiveProgress','it.forTerminal && it.passiveProgress',
       'SSHConnectionPresentationTest#tmuxManagementUsesInlineProgressAndBackCancels',1),
    ]
    wanted = set(os.environ.get('ANDROID_SSH_PRODUCT_CASES', '').split(',')) - {''}
    if wanted - {case[0] for case in cases}: raise RuntimeError('Unknown SSH product mutation')
    if wanted: cases = [case for case in cases if case[0] in wanted]
    def interrupted(signum, frame): raise KeyboardInterrupt('SSH product mutation interrupted')
    handlers = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        for label,path,old,new,method,count in cases:
            text=originals[path].decode()
            if text.count(old)!=count:raise RuntimeError('Ambiguous product mutation '+label)
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            label='product-'+label
            build(label)
            output=test(label,'com.hanxi.ghosterm.'+method,False)
            if ('INSTRUMENTATION_STATUS: numtests=1' not in output or 'Tests run: 1,  Failures: 1' not in output or
                'Process crashed' in output or 'INSTRUMENTATION_FAILED' in output):
                print(output,flush=True);raise RuntimeError('Expected one normal SSH product assertion')
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored','com.hanxi.ghosterm.'+method,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Product source restoration failed')
        for sig, handler in handlers.items(): signal.signal(sig, handler)
    print(f'ANDROID SSH PRODUCT MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_SSH_PRODUCT_MUTATE=1 ANDROID_SERIAL=... bash scripts/test-android-ssh.sh')
