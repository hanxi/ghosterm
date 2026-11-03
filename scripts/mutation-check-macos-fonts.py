#!/usr/bin/env python3
"""Isolated native font mutations: compiled assertion red, independent config green, restore green and SHA256."""
import argparse,hashlib,json,os,shutil,signal,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
PKG=ROOT/'apple/GhostermApp'
MARK='GT_MACOS_FONT_MUTATION'
FONTS='Sources/GhostermAppModel/TerminalFonts.swift'
MODEL='Sources/GhostermAppModel/AppModel.swift'
PICKER='Sources/GhostermApp/TerminalFontPicker.swift'
CASES=[
 ('monospace-validation',FONTS,'CTFontGetSymbolicTraits(font).contains(.traitMonoSpace) else','true else','testProportionalDefaultFaceCannotBeSelectedAsMonospace'),
 ('style-integrity',FONTS,'for face in manifest.faces {\n            guard','for face in manifest.faces {\n            if face.postscript == "SourceCodePro-Bold" { continue }\n            guard','testEveryStyleIsRequiredAndIndividuallyVerified'),
 ('default-clears-fallbacks',FONTS,'while SettingsStore.value(of: "font-family", in: result) != nil','while family != nil && SettingsStore.value(of: "font-family", in: result) != nil','testChoosingOneFaceReplacesLocalFallbacksButPreservesUnrelatedBytes'),
 ('search',FONTS,'query.isEmpty || $0.name.localizedCaseInsensitiveContains(query) || $0.source.rawValue.contains(query)','true || $0.name.isEmpty','testPickerPreviewIsDraftUntilApplyAndSearchKeepsSelection'),
 ('draft-until-apply',PICKER,'@Published var selection: String?','@Published var selection: String? { didSet { _ = apply() } }','testPickerPreviewIsDraftUntilApplyAndSearchKeepsSelection'),
 ('default-preview-honesty',FONTS,'name: "默认 · JetBrains Mono", previewName: nil','name: "默认 · JetBrains Mono", previewName: "Menlo"','testCatalogKeepsEngineDefaultAndOneBundledFaceAndFiltersActualMonospacedFonts'),
 ('saved-font-choice',MODEL,'TerminalFontSelection.replacing(in: terminalConfigText, family: family)','TerminalFontSelection.replacing(in: terminalConfigText, family: nil)','testPickerPreviewIsDraftUntilApplyAndSearchKeepsSelection'),
 ('live-engine-route',MODEL,'try replaceTerminalConfig(TerminalFontSelection.replacing(in: terminalConfigText, family: family))','try settings.saveTerminalConfig(TerminalFontSelection.replacing(in: terminalConfigText, family: family))','testFontChangeRepaintsExistingSurfaceWithoutReplacingTheShell'),
 ('appearance-height','Sources/GhostermApp/SettingsView.swift','case .appearance: return 580','case .appearance: return 640','SettingsUITests/testPaneHeightsMatchContent'),
 ('keyboard-height','Sources/GhostermApp/SettingsView.swift','case .keyboard: return 640','case .keyboard: return 420','SettingsUITests/testPaneHeightsMatchContent'),
]
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--case',type=int,action='append');args=parser.parse_args()
 chosen=args.case or list(range(1,len(CASES)+1));assert len(set(chosen))==len(chosen) and all(1<=i<=len(CASES) for i in chosen)
 originals={name:(PKG/name).read_bytes() for name in {c[1] for c in CASES}}
 assert not any(MARK.encode() in v for v in originals.values()),'Residual mutation'
 for i in chosen:
  _,name,old,_,_=CASES[i-1];assert originals[name].decode().count(old)==1,(i,'ambiguous source anchor')
 work=Path(tempfile.mkdtemp(prefix='macos-font-mutations-',dir=ROOT/'tmp'));package=work/'apple/GhostermApp'
 for name in ['GhostermApp','GhostermCore','GhostermTerminal']:
  shutil.copytree(ROOT/'apple'/name,work/'apple'/name,ignore=shutil.ignore_patterns('.build','.swiftpm'))
 for name in ['core','build']:(work/name).symlink_to(ROOT/name,target_is_directory=True)
 print('MACOS FONT RECEIPTS',work,flush=True)
 env=dict(os.environ,TMPDIR=str(ROOT/'tmp'));env.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
 def interrupt(sig,frame):raise RuntimeError('Font mutation interrupted: '+str(sig))
 for sig in [signal.SIGINT,signal.SIGTERM]:signal.signal(sig,interrupt)
 def restore():
  for name,data in originals.items():
   (package/name).write_bytes(data)
   assert (PKG/name).read_bytes()==data and (package/name).read_bytes()==data,'Source SHA256 restore mismatch'
 def probe(label,test,red=False):
  command=['bash','-c','. "$1";shift;gt_timeout_require macos-fonts;gt_timeout 300 "$@"','macos-fonts',str(ROOT/'scripts/lib/gt-timeout.sh'),'swift','test','--build-system','native','--package-path',str(package),'--filter',test]
  result=subprocess.run(command,cwd=ROOT,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);output=result.stdout
  (work/(label+'.log')).write_text(output)
  good='Build complete!' in output and 'Executed 1 test' in output and '0 unexpected' in output and 'skipped' not in output and 'unexpected signal' not in output
  good=good and (result.returncode==1 and 'XCTAssert' in output and 'with 1 failure' in output if red else result.returncode==0 and 'with 0 failures' in output)
  if not good:print(output,flush=True);raise RuntimeError('Invalid assertion red/control green: '+label)
 def target(test):return test if '/' in test else 'TerminalFontTests/'+test
 control='ConfigStoreTests/testAtomicWriteRoundTrip';accepted=[]
 try:
  for i,test in enumerate(dict.fromkeys(CASES[i-1][4] for i in chosen)):probe('baseline-'+str(i),target(test))
  probe('baseline-control',control)
  for number in chosen:
   name,file,old,new,test=CASES[number-1];label=f'F{number:02d}-{name}'
   (package/file).write_text(originals[file].decode().replace(old,new,1)+'\n// '+MARK+'\n')
   probe(label+'-target',target(test),True);probe(label+'-control',control)
   restore();probe(label+'-restored',target(test))
   accepted.append({'case':number,'name':name,'compiled_assertion_red':True,'independent_config_green':True,'restored_green':True})
   (work/'accepted-results.json').write_text(json.dumps(accepted,indent=2));print(label+': compiled assertion red / config green / restored green / SHA256',flush=True)
 finally:restore()
 print('MACOS FONT MUTATIONS PASS:',len(accepted),'groups',flush=True)
if __name__=='__main__':main()
