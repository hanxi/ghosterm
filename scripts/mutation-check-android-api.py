#!/usr/bin/env python3
"""Prove the CI API gate catches an API-30 call in a min-29 dependency module."""
import hashlib
import importlib.util
import json
from pathlib import Path
import signal
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_API_MUTATION'
spec=importlib.util.spec_from_file_location('native_helper',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)


def sources():
    names=set(subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines())
    return {name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in sorted(names)
        if not name.endswith('.md') and not name.startswith('vendor/') and (ROOT/name).is_file()}


def main():
    path=ROOT/'android/data/src/main/java/com/hanxi/ghosterm/data/LocalFileBrowser.kt'
    original=path.read_bytes()
    if MARK.encode() in original:raise RuntimeError('Residual Android API mutation')
    work=ROOT/'tmp/android-api-mutations'/str(time.time_ns());work.mkdir(parents=True)
    before=sources();(work/'sources-before.json').write_text(json.dumps(before,indent=2)+'\n')
    def run(label,arguments,green=True):
        result=helper.run(arguments,900);output=result.stdout+result.stderr
        (work/(label+'.log')).write_text(output)
        if green:helper.require(result)
        else:
            if result.returncode==0 or not all(text in output for text in (
                    'LocalFileBrowser.kt','NewApi','API level 30','android.system.Os#fcntlInt')):
                raise RuntimeError('Expected the API-level assertion to fail: '+output[-8000:])
        return output
    gate=['bash',ROOT/'scripts/check-android-api.sh']
    try:
        run('baseline',gate)
        old='val flags=FileDescriptorFlags.status(fd)'
        new='val flags=Os.fcntlInt(fd,OsConstants.F_GETFL,0)'
        source=original.decode()
        if source.count(old)!=1:raise RuntimeError('API mutation anchor is missing/ambiguous')
        path.write_text(source.replace(old,new+' // '+MARK))
        # This call exists in compileSdk 36: compilation must succeed first.
        run('mutant-compiled',['bash',ROOT/'scripts/build-android.sh',':app:assembleDebug'])
        run('mutant-target',gate,False)
        run('mutant-independent-terminal',['bash',ROOT/'scripts/build-android.sh',':terminal:lintRelease'])
        path.write_bytes(original)
        run('restored',gate)
        run('restored-compiled',['bash',ROOT/'scripts/build-android.sh',':app:assembleDebug'])
    finally:
        if MARK.encode() in path.read_bytes():path.write_bytes(original)
        after=sources();(work/'sources-after.json').write_text(json.dumps(after,indent=2)+'\n')
        if before!=after:raise RuntimeError('API mutation source set/SHA256 restoration failed')
    print(f'ANDROID API MUTATION PASS: compiled API-30 data call red / same mutation terminal lint green / restored green / SHA256; receipts {work}',flush=True)


if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    main()
