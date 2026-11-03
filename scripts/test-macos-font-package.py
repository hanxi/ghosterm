#!/usr/bin/env python3
"""Run the packaged font loader; missing/corrupt resources must fail without hiding configuration controls."""
import argparse,hashlib,json,os,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--app',type=Path,required=True);args=parser.parse_args()
 app=args.app.resolve();assert (app/'Contents/MacOS/GhostermApp').is_file()
 work=Path(tempfile.mkdtemp(prefix='macos-font-package-',dir=ROOT/'tmp'))
 env=dict(os.environ,TMPDIR=str(ROOT/'tmp'))
 config=work/'control.conf';config.write_text('font-size = 14\nbackground = #123456\n')
 def run(label,target,mode,green):
  command=[str(target/'Contents/MacOS/GhostermApp'),mode]+([str(config)] if mode=='--config-check' else [])
  result=subprocess.run(['bash','-c','. "$1";shift;gt_timeout_require font-package;gt_timeout 60 "$@"','font-package',str(ROOT/'scripts/lib/gt-timeout.sh'),*command],env=env,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  output=result.stdout;(work/(label+'.log')).write_text(output)
  if green:
   good=result.returncode==0 and ('CONFIG_OK diagnostics=0' in output if mode=='--config-check' else '"scope":"process"' in output)
  else:good=result.returncode==1 and '字体校验失败：' in output
  if not good:print(output,flush=True);raise RuntimeError('Packaged font check failed: '+label)
  return output
 report=json.loads(run('baseline-font',app,'--font-check',True))
 resources=Path(report['resources']);assert resources.is_relative_to(app/'Contents/Resources')
 files={str(p.relative_to(app)):hashlib.sha256(p.read_bytes()).hexdigest() for p in resources.iterdir() if p.is_file()}
 assert len([p for p in resources.iterdir() if p.suffix=='.ttf'])==4
 run('baseline-control',app,'--config-check',True)
 results=[]
 for case in ['bundle-missing','face-missing','face-corrupt','license-corrupt']:
  target=work/(case+'.app');shutil.copytree(app,target,symlinks=True)
  relative=resources.relative_to(app);fonts=target/relative
  if case=='bundle-missing':shutil.rmtree(target/'Contents/Resources/GhostermApp_GhostermAppModel.bundle')
  elif case=='face-missing':(fonts/'SourceCodePro-Bold.ttf').unlink()
  elif case=='face-corrupt':(fonts/'SourceCodePro-It.ttf').write_bytes(b'corrupt font')
  else:(fonts/'SourceCodePro-OFL.txt').write_bytes(b'corrupt license')
  run(case+'-target',target,'--font-check',False);run(case+'-control',target,'--config-check',True)
  shutil.rmtree(target);shutil.copytree(app,target,symlinks=True);run(case+'-restored',target,'--font-check',True)
  assert {str((target/relative/p.name).relative_to(target)):hashlib.sha256((target/relative/p.name).read_bytes()).hexdigest() for p in resources.iterdir() if p.is_file()}==files
  shutil.rmtree(target)
  results.append({'case':case,'explicit_error':True,'same_bundle_config_green':True,'restored_green_sha256':True});print('PASS:',case,flush=True)
 (work/'receipt.json').write_text(json.dumps({'resources':report,'files':files,'cases':results},indent=2))
 print('MACOS FONT PACKAGE PASS:',len(results),'groups; receipts',work)
if __name__=='__main__':main()
