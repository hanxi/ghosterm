#!/usr/bin/env python3
"""Isolated compiled CLI faults; actual Node/Exec children, no accounts/models."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'cli/gterm/stdio.c'
ARCH = 'aarch64' if platform.machine() in ['arm64','aarch64'] else 'x86_64'
CASES = [
    ('unsupported-result', 'info.external_process && info.execution_error == GT_ERR_UNSUPPORTED', 'false', 'exec-unsupported'),
    ('runtime-facts', '                             runtime_name,', '                             "portable-js",', 'node-roundtrip'),
    ('execution-approval', 'if (external && !execution_authorized)', 'if (false && external && !execution_authorized)', 'node-no-approval'),
    ('runtime-config', '.execution = external ? &execution : NULL', '.execution = NULL', 'node-roundtrip'),
    ('sdk-identity', '.node_sdk_catalog_sha256 = sdk_sha', '.node_sdk_catalog_sha256 = NULL', 'node-roundtrip'),
    ('selected-environment', 'execution.environment_count = process_environment_count;', 'execution.environment_count = 0;', 'node-roundtrip'),
    ('embedded-budget', '.cpu_ms = external ? 0 : 100,', '.cpu_ms = 100,', 'node-roundtrip'),
    ('source-trust', 'if (strcmp(metadata.package_digest, trusted))', 'if (false && strcmp(metadata.package_digest, trusted))', 'node-source-digest'),
    ('exec-platform', 'const char *host_arch = "'+ARCH+'";', 'const char *host_arch = "incompatible";', 'exec-roundtrip'),
]


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--node', type=Path, required=True)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--case', action='append')
    a = p.parse_args()
    assert not a.case or not set(a.case)-{c[0] for c in CASES}, 'unknown case'
    cases = [c for c in CASES if not a.case or c[0] in a.case]
    for label, old, _, _ in cases:
        assert (ROOT/SOURCE).read_text().count(old)==1,label
    node, sdk = a.node.resolve(strict=True), a.sdk.resolve(strict=True)
    node_sha=sha(node)
    sdk_hashes={str(f.relative_to(sdk)):sha(f) for f in sdk.rglob('*') if f.is_file()}
    work=Path(tempfile.mkdtemp(prefix='gterm-process-mutations-',dir=ROOT/'tmp'))
    source,build,logs=work/'source',work/'build',work/'logs'
    logs.mkdir()
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1',
         'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    env.pop('NODE_OPTIONS',None)
    commands=[]

    def run(label, command, red=False):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require gterm-process; shift; gt_timeout 180 "$@"'
        r=subprocess.run(['bash','-c',shell,'gterm-process',str(ROOT),*map(str,command)],
            cwd=ROOT,env=env,capture_output=True,text=True,timeout=190)
        output=r.stdout+r.stderr
        (logs/(label+'.log')).write_text(output)
        commands.append(dict(label=label,returncode=r.returncode,assertion=red))
        if red:
            assert r.returncode==1 and re.search(r'^FAIL ',r.stderr,re.M) and not any(
                s in output for s in ['ABNORMAL','AddressSanitizer','UndefinedBehaviorSanitizer','runtime error:']),label+'\n'+output
        else:
            assert r.returncode==0,label+'\n'+output
        return output

    try:
        def ignore(directory,names):
            return [n for n in names if n=='__pycache__' or ((Path(directory)/n).is_dir() and (n=='build' or n.startswith('build-')))]
        folders=['core','cli/gterm','scripts','packages/plugin-sdk','patches/quickjs-ng']
        for folder in folders:
            shutil.copytree(ROOT/folder,source/folder,ignore=ignore)
        shutil.copyfile(ROOT/'LICENSE',source/'LICENSE')
        (source/'tmp').mkdir()
        (source/'vendor').mkdir()
        (source/'vendor/quickjs-ng').symlink_to(ROOT/'vendor/quickjs-ng',target_is_directory=True)
        inputs={str(f.relative_to(source)):sha(f) for folder in folders for f in (source/folder).rglob('*') if f.is_file()}
        run('configure',['cmake','-S',source/'cli/gterm','-B',build,
            '-DGHOSTERM_BUILD_TESTS=ON','-DGHOSTERM_GTERM_TESTS=ON','-DGHOSTERM_WITH_LIBSSH2=OFF',
            '-DGHOSTERM_WITH_PLUGIN_JS=ON','-DGHOSTERM_PLUGIN_SDK_DIR='+str(sdk/'portable'),
            '-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g',
            '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        binaries=['gterm','test_gterm_stdio','test_gterm_exec_peer','core/test_config']

        def rebuild(label, changed=False):
            if changed:
                (build/'CMakeFiles/gterm.dir/stdio.c.o').unlink(missing_ok=True)
            for name in [*binaries,'core/libghosterm_core.a']:
                (build/name).unlink(missing_ok=True)
            output=run(label+'-build',['cmake','--build',build,'--target','gterm','test_gterm_stdio',
                'test_gterm_exec_peer','test_config','-j','4'])
            for name in binaries:
                assert 'Linking C executable '+Path(name).name in output,label+': missing actual link '+name
            assert 'Linking C static library' in output
            if changed: assert 'stdio.c.o' in output

        def test(label, case=None, red=False):
            command=['python3',source/'scripts/test-gterm-process.py','--build',build,'--node',node,'--sdk',sdk]
            if case: command+=['--case',case]
            return run(label,command,red)

        rebuild('initial')
        test('baseline')
        for label,old,new,case in cases:
            target=source/SOURCE
            original=target.read_bytes()
            try:
                target.write_text(original.decode().replace(old,new))
                rebuild(label,True)
                test(label+'-red',case,True)
                run(label+'-control',[build/'core/test_config',source/'core/tests/fixtures'])
            finally:
                target.write_bytes(original)
                assert sha(target)==hashlib.sha256(original).hexdigest()
            rebuild(label+'-restored',True)
            test(label+'-restored',case)
            print(label+': compiled assertion red / same archive control green / restored green / SHA256',flush=True)
        test('final')
        for name,digest in inputs.items(): assert sha(source/name)==digest and sha(ROOT/name)==digest,name
        assert sha(node)==node_sha
        for name,digest in sdk_hashes.items(): assert sha(sdk/name)==digest,name
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=[c[0] for c in cases],
            inputs_sha256=inputs,commands=commands,node_sha256=node_sha,sdk_sha256=sdk_hashes,
            sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n')
        print('GTERM PROCESS MUTATIONS PASS: '+str(logs),flush=True)
    except BaseException as error:
        (logs/'failure.txt').write_text(repr(error))
        raise


if __name__=='__main__': main()
