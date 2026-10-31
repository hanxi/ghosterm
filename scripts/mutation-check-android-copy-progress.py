#!/usr/bin/env python3
"""Actual source/write bytes, phase UI and monotonic estimates with same APK controls."""
import hashlib
import os
import re
MARK='GT_ANDROID_COPY_PROGRESS_MUTATION'


def execute(root,build,test):
    domain=root/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FileCopyProgress.kt'
    executor=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileCopyExecutor.kt'
    store=root/'android/data/src/main/java/com/hanxi/ghosterm/data/TransferStagingStore.kt'
    local=root/'android/data/src/main/java/com/hanxi/ghosterm/data/LocalFilePublisher.kt'
    saf=root/'android/data/src/main/java/com/hanxi/ghosterm/data/SafFilePublisher.kt'
    ui=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksScreen.kt'
    originals={p:p.read_bytes() for p in (domain,executor,store,local,saf,ui)}
    if any(MARK.encode() in value for value in originals.values()):raise RuntimeError('Residual copy progress mutation')
    staging='com.hanxi.ghosterm.TransferStagingTest#sourceProgressReportsActualBytesAndKeepsUnknownSizeDistinct'
    mean='com.hanxi.ghosterm.FileCopyExecutorTest#phaseAverageUsesElapsedTimeWithoutInventingUnknownTotals'
    write='com.hanxi.ghosterm.FileCopyExecutorTest#localWriteBytesStayUncommittedUntilPublication'
    read='com.hanxi.ghosterm.SafCopyExecutorTest#partialSourceProgressSurvivesTheCoalescingWindow'
    partial='com.hanxi.ghosterm.SafCopyExecutorTest#partialWriteProgressIsVisibleWhileProviderIsStalled'
    reading_ui='com.hanxi.ghosterm.SafFileTasksProductTest#partialReadShowsActualBytesAndPhaseAverageOnTheTaskPage'
    verify_ui='com.hanxi.ghosterm.SafFileTasksProductTest#writeProgressStaysVerifyingUntilRealProviderReadbackCompletes'
    cases=[
        ('source-zero',store,'progress(0,total)','progress(1,total)',staging,'source progress initial zero'),
        ('source-bytes',store,'progress(size,total)','progress(0,total)',staging,'source progress real final bytes'),
        ('source-unknown',store,'val total=if(length>=0) length else expected','val total=if(length>=0) length else expected ?: 0L',staging,'source progress preserves unknown total'),
        ('source-thread',store,'progress(size,total)','kotlinx.coroutines.runBlocking { kotlinx.coroutines.withContext(Dispatchers.Main) { progress(size,total) } }',staging,'source progress belongs to IO'),
        ('read-phase',executor,'observe(source,FileCopyStage.READING,source.entry.size)','observe(source,FileCopyStage.WRITING,source.entry.size)',read,'partial source phase'),
        ('read-forward',executor,'reading(bytes.toULong(),total?.toULong())','reading(0uL,total?.toULong())',read,'source bytes become visible during stall'),
        ('write-forward',executor,'writingProgress(bytes.toULong(),total.toULong())','writingProgress(0uL,total.toULong())',write,'local final bytes await publication'),
        ('verify-phase',executor,'FileCopyStage.VERIFYING else stage','FileCopyStage.WRITING else stage',write,'local final bytes await publication'),
        ('activity-bytes',executor,'FileCopyActivity(source.relativePath,next,bytes,knownTotal,began,now)','FileCopyActivity(source.relativePath,next,bytes+1uL,knownTotal,began,now)',write,'local observed write byte count'),
        ('local-written-bytes',local,'checkpoint(copied+offset,size)','checkpoint(0,size)',write,'local final bytes await publication'),
        ('saf-short-write',saf,'checkpoint(copied+offset,size)','Unit',partial,'partial write bytes become visible while stalled'),
        ('mean-window',domain,'elapsed<1000','elapsed<0',mean,'speed needs a time window'),
        ('mean-bytes',domain,'bytes.toDouble()*1000/elapsed','bytes.toDouble()*500/elapsed',mean,'phase speed uses real elapsed time'),
        ('mean-phase',domain,'stage==FileCopyStage.VERIFYING || elapsed<1000','elapsed<1000',mean,'verification is not byte transfer'),
        ('estimate-unknown',domain,'val total=totalBytes ?: return null','val total=totalBytes ?: return 0L',mean,'unknown size has no time estimate'),
        ('estimate-remaining',domain,'(total-bytes).toDouble()/rate','total.toDouble()/rate',mean,'remaining time is for this phase'),
        ('estimate-finished',domain,'bytes>=total || nowMillis-observedMillis>2000','nowMillis-observedMillis>2000',mean,'reached byte total is not zero seconds to completion'),
        ('estimate-stall',domain,'bytes>=total || nowMillis-observedMillis>2000','bytes>=total',mean,'stalled IO has no remaining-time estimate'),
        ('ui-pane',ui,'FileTaskByteProgress(activity,result.state==FileCopyState.RUNNING && task.active)','Unit',verify_ui,'file-task-byte-stage'),
        ('ui-reading',ui,'FileCopyStage.READING -> "读取来源"','FileCopyStage.READING -> "写入目标"',reading_ui,'正在读取来源'),
        ('ui-verifying',ui,'FileCopyStage.VERIFYING -> "核对并提交"','FileCopyStage.VERIFYING -> "复制完成"',verify_ui,'正在核对并提交'),
        ('ui-bytes',ui,'Text("${activity.bytes} / ','Text("0 / ',reading_ui,'4 / 8 字节'),
        ('ui-speed',ui,'Text("本阶段平均 ${String.format(java.util.Locale.ROOT,"%.0f",rate)} 字节/秒",Modifier.testTag("file-task-byte-speed"))','Unit',reading_ui,'file-task-byte-speed'),
        ('ui-finished',ui,'takeIf { result.state!=FileCopyState.COMPLETE && result.state!=FileCopyState.WITH_SKIPS }','takeIf { true }',verify_ui,'file-task-byte-progress'),
    ]
    selected=os.environ.get('ANDROID_COPY_PROGRESS_CASES','').split(',')
    wanted=None if selected==[''] else set(selected)
    names={case[0] for case in cases}|{'ticker-tail'}
    if wanted is not None and wanted-names:raise RuntimeError('Unknown copy progress mutation')
    def one(label,method,green,message=None):
        out=test(label,method,green)
        if 'INSTRUMENTATION_STATUS: numtests=1' not in out or (not green and not re.search(r'Tests run: 1,\s+Failures: 1',out)):
            print(out,flush=True);raise RuntimeError('Expected exactly one actual JUnit result')
        if message and message not in out:print(out,flush=True);raise RuntimeError('Wrong target assertion: '+message)
    def restore():
        for path,value in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(value)
        if any(hashlib.sha256(path.read_bytes()).digest()!=hashlib.sha256(value).digest() for path,value in originals.items()):
            raise RuntimeError('Copy progress restoration failed')
    count=0
    try:
        for name,path,old,new,method,message in cases:
            if wanted is not None and name not in wanted:continue
            clean=originals[path].decode();expected=2 if name=='write-forward' else 1
            assert clean.count(old)==expected,(name,clean.count(old))
            path.write_text(clean.replace(old,new)+'\n// '+MARK+'\n')
            label='copy-progress-'+name
            build(label);one(label,method,False,message)
            one(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            restore();build(label+'-restored');one(label+'-restored',method,True)
            count+=1;print('PASS '+label+': compiled target assertion red / same APK config green / restored green / SHA256',flush=True)
        if wanted is None or 'ticker-tail' in wanted:
            clean=originals[executor].decode()
            immediate='now-emittedAt>=250 ||';guard='if(!finalized.get() && activity!=mutable.value.activity)'
            assert clean.count(immediate)==1 and clean.count(guard)==1
            # Force a buffered final partial observation in BOTH branches. The
            # real provider pipe then stalls: only the production ticker can flush.
            buffered=clean.replace(immediate,'false ||')
            executor.write_text(buffered+'\n// '+MARK+'\n')
            build('copy-progress-tail-baseline');one('copy-progress-tail-baseline',read,True)
            executor.write_text(buffered.replace(guard,'if(false && !finalized.get() && activity!=mutable.value.activity)')+'\n// '+MARK+'\n')
            build('copy-progress-tail');one('copy-progress-tail',read,False,'source bytes become visible during stall')
            one('copy-progress-tail-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            restore();build('copy-progress-tail-restored');one('copy-progress-tail-restored',read,True)
            count+=1;print('PASS copy-progress ticker tail: forced window baseline green / compiled assertion red / config green / restored green / SHA256',flush=True)
    finally:restore()
    print(f'ANDROID COPY PROGRESS MUTATIONS PASS: {count} groups',flush=True)


if __name__=='__main__':raise SystemExit('Use ANDROID_COPY_PROGRESS_MUTATE=1 with test-android-ssh.sh')
