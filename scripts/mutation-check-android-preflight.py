#!/usr/bin/env python3
"""Destination checks, request conflicts and namespace uncertainty: real device assertions, same APK config control and SHA restore."""
import argparse,hashlib,importlib.util,os,signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_PREFLIGHT_MUTATION'
spec=importlib.util.spec_from_file_location('helper',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[ROOT/'build/android-toolchain/sdk/platform-tools/adb','-s',serial];require(run([*adb,'get-state']))
    domain=ROOT/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FilePasteCheck.kt'
    scanner=ROOT/'android/data/src/main/java/com/hanxi/ghosterm/data/FilePastePreflight.kt'
    access=ROOT/'android/data/src/main/java/com/hanxi/ghosterm/data/FileDestinationAccess.kt'
    originals={p:p.read_bytes() for p in (domain,scanner,access)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Existing scan mutation marker')
    work=ROOT/'tmp/android-preflight-mutations';work.mkdir(exist_ok=True)
    real='FilePastePreflightTest#realPrivateDirectoriesReportConflictsWithoutChangingFiles'
    subtree='FilePastePreflightTest#sameFolderAndSelectedSubtreesAreRejectedButSiblingIsAllowed'
    unknown='FilePastePreflightTest#incompleteScanAndUnknownRelationshipStopBeforeTargetReads'
    read='FilePastePreflightTest#targetReadFailuresAndChangedLocationsAreNotEmptySuccess'
    invalid='FilePastePreflightTest#ambiguousUnsafeForeignAndOversizedTargetsFailExplicitly'
    remote='FilePastePreflightTest#remoteLogicalIdentityIgnoresAliasButDoesNotInventNamespaceEquivalence'
    saf='SafPublicationTest#targetPreflightUsesRealGrantFlagsAndConflictMetadataWithoutCreating'
    safRelation='SafPublicationTest#targetPreflightSafRelationshipsUseOpaqueIdsAndRejectForeignTrees'
    cases=[
        ('target-readonly',domain,'Collections.unmodifiableList(ArrayList(targetEntries))','ArrayList(targetEntries)',real),
        ('conflicts-readonly',domain,'Collections.unmodifiableList(ArrayList(conflicts))','ArrayList(conflicts)',real),
        ('incomplete',scanner,'if(!scan.complete)','if(false && !scan.complete)',unknown),
        ('same-parent',scanner,'FileLocationRelation.SAME -> stop(FilePasteCheckFailureKind.SAME_DIRECTORY','FileLocationRelation.SAME -> if(false) stop(FilePasteCheckFailureKind.SAME_DIRECTORY',subtree),
        ('inside-source',scanner,'stop(FilePasteCheckFailureKind.INSIDE_SOURCE','if(false) stop(FilePasteCheckFailureKind.INSIDE_SOURCE',subtree),
        ('parent-unknown',scanner,'FileLocationRelation.UNKNOWN -> stop(FilePasteCheckFailureKind.RELATION_UNKNOWN,"无法确认来源与目标','FileLocationRelation.UNKNOWN -> if(false) stop(FilePasteCheckFailureKind.RELATION_UNKNOWN,"无法确认来源与目标',unknown),
        ('child-unknown',scanner,'FileLocationRelation.UNKNOWN -> stop(FilePasteCheckFailureKind.RELATION_UNKNOWN,"无法确认目录包含关系','FileLocationRelation.UNKNOWN -> if(false) stop(FilePasteCheckFailureKind.RELATION_UNKNOWN,"无法确认目录包含关系',unknown),
        ('target-location',scanner,'if(target.directory.location!=request.destination)','if(false && target.directory.location!=request.destination)',read),
        ('denial',scanner,'if(target.create==FileCreateCapability.DENIED)','if(false && target.create==FileCreateCapability.DENIED)',read),
        ('capability',scanner,'capability=target.create','capability=FileCreateCapability.ALLOWED','FilePastePreflightTest#conflictsStayBoundToRequestAndUnknownWritePermissionIsPreserved'),
        ('page',scanner,'candidates.size>4096','(false && candidates.size>4096)',invalid),
        ('duplicate-id',scanner,'candidates.map { it.id }.toSet().size!=candidates.size','(false && candidates.map { it.id }.toSet().size!=candidates.size)',invalid),
        ('duplicate-name',scanner,'candidates.map { it.name }.toSet().size!=candidates.size','(false && candidates.map { it.name }.toSet().size!=candidates.size)',invalid),
        ('name',scanner,"'/' !in name","(true || '/' !in name)",invalid),
        ('unicode',scanner,'Character.isLowSurrogate(c)','(false && Character.isLowSurrogate(c))',invalid),
        ('membership',scanner,'if(expected!=null && item.id!=expected)','if(false && expected!=null && item.id!=expected)',invalid),
        ('metadata',scanner,'if(metadata>maxMetadataBytes)','if(false && metadata>maxMetadataBytes)',invalid),
        ('conflict-match',scanner,'conflicts=request.items.mapNotNull','conflicts=emptyList<BrowserEntry>().mapNotNull',real),
        ('read-failure',scanner,'failure=FilePasteCheckFailure(FilePasteCheckFailureKind.TARGET_READ,error.message ?: "无法检查目标文件夹")','failure=null',read),
        ('timeout',scanner,'if(done==null) failure=','if(false && done==null) failure=','FilePastePreflightTest#deadlineKeepsIncompleteStateAndCancelsCurrentRelationshipCheck'),
        ('cancel-target',scanner,'val target=inspect(request.destination)','val target=withContext(NonCancellable) { inspect(request.destination) }','FilePastePreflightTest#callerCancellationReachesCurrentTargetReadAndDoesNotDeliverResult'),
        ('local-denied',access,'if(error.errno==OsConstants.EACCES || error.errno==OsConstants.EROFS) false','if(error.errno==OsConstants.EACCES || error.errno==OsConstants.EROFS) true','FilePastePreflightTest#privateReadOnlyMissingAndSymlinkDestinationsNeverBecomeWritable'),
        ('saf-write-grant',access,'if(!resolver.persistedUriPermissions.any { it.uri==tree && it.isWritePermission })','if(false && !resolver.persistedUriPermissions.any { it.uri==tree && it.isWritePermission })',saf),
        ('saf-create',access,'if(flags and Document.FLAG_DIR_SUPPORTS_CREATE!=0)','if(true || flags and Document.FLAG_DIR_SUPPORTS_CREATE!=0)',saf),
        ('saf-root-id',access,'check(Documents.getTreeDocumentId(uri)==endpoint.rootId)','check(true)',safRelation),
        ('saf-membership',access,'check(root==document || Documents.isChildDocument(resolver,root,document))','check(root==document || Documents.isChildDocument(resolver,document,root))',saf),
        ('saf-type',access,'cursor.getString(1)==Document.MIME_TYPE_DIR','true',safRelation),
        ('saf-descendant',access,'Documents.isChildDocument(resolver,parent,child) ->','false ->',safRelation),
        ('path-boundary',access,"target.startsWith(source.trimEnd('/')+\"/\")","target.startsWith(source.trimEnd('/'))",subtree),
        ('remote-capability',access,'FileCreateCapability.UNKNOWN // SFTP','FileCreateCapability.ALLOWED // SFTP',remote),
        ('remote-user',access,'left.user!=right.user','(false && left.user!=right.user)',remote),
        ('remote-port',access,'left.port!=right.port','(false && left.port!=right.port)',remote),
        ('remote-host',access,'!left.host.equals(right.host,ignoreCase=true)','(false && !left.host.equals(right.host,ignoreCase=true))',remote),
        ('remote-unknown',access,'return FileLocationRelation.UNKNOWN','return FileLocationRelation.SEPARATE',remote),
    ]
    if args.case:
        selected=set(args.case.split(','))
        if not selected.issubset({case[0] for case in cases}):raise RuntimeError('Unknown case')
        cases=[case for case in cases if case[0] in selected]
    prefix='com.hanxi.ghosterm.'
    def build(label):
        result=run(['bash',ROOT/'scripts/build-android.sh'],900)
        (work/(label+'-build.log')).write_text(result.stdout+result.stderr);require(result)
        for apk in ('apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk'):
            require(run([*adb,'install','-r','-t',ROOT/'android/app/build/outputs'/apk]))
    def test(label,selection,green,count=1):
        result=run([*adb,'shell','am','instrument','-w','-r','-e','class',','.join(prefix+n for n in selection.split(',')),'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        output=result.stdout+result.stderr;(work/(label+'.log')).write_text(output)
        okay=result.returncode==0 and f'OK ({count} '+('test)' if count==1 else 'tests)') in output
        if okay!=green or (not green and ('FAILURES' not in output or not any(s in output for s in ('AssertionError','ComparisonFailure')))):
            print(output,flush=True);raise RuntimeError('Expected compiled assertion '+('green' if green else 'red'))
    build('baseline');test('baseline','FilePastePreflightTest,SafPublicationTest#targetPreflightUsesRealGrantFlagsAndConflictMetadataWithoutCreating,SafPublicationTest#targetPreflightSafRelationshipsUseOpaqueIdsAndRejectForeignTrees',True,12)
    control='ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    test('baseline-control',control,True)
    try:
        for label,path,before,after,method in cases:
            clean=originals[path].decode()
            start,end=0,len(clean)
            piece=clean[start:end]
            if piece.count(before)!=1:raise RuntimeError(label+': missing/ambiguous anchor '+str(piece.count(before)))
            path.write_text(clean[:start]+piece.replace(before,after)+clean[end:]+'\n// '+MARK+'\n')
            target=method
            build(label);test(label,target,False);test(label+'-control',control,True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored',target,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Source restoration failed')
    print(f'ANDROID PREFLIGHT MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    signal.signal(signal.SIGINT,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
