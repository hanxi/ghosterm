#!/usr/bin/env python3
"""Private test fixture over adb loopback. Secrets only travel in memory to the test process."""
import hmac,json,os,secrets,socketserver,subprocess,sys,threading,importlib.util,signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def run(args,seconds=600):
    command=['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require android-ssh; shift; gt_timeout "$@"','android-ssh',str(ROOT),str(seconds),*map(str,args)]
    result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    if result.returncode: print(result.stdout+result.stderr,flush=True);raise RuntimeError('Android SSH command failed')
    return result.stdout+result.stderr

def main():
    if len(sys.argv)!=6: raise SystemExit('Use test-android-ssh.sh with the isolated sshd fixture')
    host,port,user,key,work=sys.argv[1:]
    serial=os.environ['ANDROID_SERIAL'];adb=['adb','-s',serial]
    # Device shell tests must not run the developer's interactive shell rc/plugins.
    # Restrict only this disposable authorized key, leaving the host account untouched.
    locales=run(['locale','-a']).splitlines()
    locale=next((x for x in ('en_US.UTF-8','C.UTF-8','C.utf8','en_US.utf8') if x in locales),None)
    if locale is None:raise RuntimeError('SSH test shell requires an installed UTF-8 locale')
    Path(work,'authorized_keys').write_text('command="env LC_ALL='+locale+' INPUTRC=/dev/null /bin/sh -i" '+Path(key+'.pub').read_text())
    spec=importlib.util.spec_from_file_location('credential_fixture',ROOT/'scripts/lib/android-ssh-credentials.py')
    credentials=importlib.util.module_from_spec(spec);spec.loader.exec_module(credentials)
    credential_tests=(os.environ.get('ANDROID_SSH_ALL')=='1' or os.environ.get('ANDROID_SSH_CREDENTIAL_MUTATE')=='1' or os.environ.get('ANDROID_SSH_IDENTITIES_MUTATE')=='1' or
                      any(name in os.environ.get('ANDROID_SSH_TEST_CLASS','') for name in ('SSHCredentialFlowTest','IdentitySettingsTest')))
    identity_tests=(os.environ.get('ANDROID_SSH_ALL')=='1' or os.environ.get('ANDROID_SSH_IDENTITIES_MUTATE')=='1' or
                    'IdentitySettingsTest' in os.environ.get('ANDROID_SSH_TEST_CLASS',''))
    with credentials.fixture(ROOT,work,key,credential_tests,identity_tests) as credential_data:
        token=secrets.token_hex(32)
        # No key text in instrumentation arguments, output logs, repository, or device files.
        payload=json.dumps(dict(user=user,port=int(port),pem=Path(key).read_text(),
            clientFingerprint=run(['ssh-keygen','-lf',str(key)+'.pub','-E','sha256']).split()[1],
            fingerprint=os.environ['GT_TEST_HOSTKEY_SHA256'],public=Path(work,'hostkey.pub').read_text(),
            wrongPem=Path(work,'hostkey').read_text(),otherPublic=Path(work,'clientkey.pub').read_text(),**credential_data)).encode()
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                self.connection.settimeout(5)
                supplied=self.rfile.readline(128).strip().decode('ascii',errors='ignore')
                if hmac.compare_digest(supplied,token): self.wfile.write(payload+b'\n')
        with socketserver.ThreadingTCPServer(('127.0.0.1',0),Handler) as server:
            server.daemon_threads=True
            fixture_port=server.server_address[1]
            worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
            mappings=[]
            try:
                for value in (port,fixture_port,*(credential_data[label]['port'] for label in ('password','mixed') if label in credential_data)):
                    run([*adb,'reverse','tcp:'+str(value),'tcp:'+str(value)]);mappings.append(value)
                logs=ROOT/'tmp/android-ssh-tests';logs.mkdir(exist_ok=True)
                def build(label):
                    (logs/(label+'-build.log')).write_text(run(['bash',ROOT/'scripts/build-android.sh'],900))
                    for path in ('apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk'):
                        run([*adb,'install','-r','-t',ROOT/'android/app/build/outputs'/path])
                def test(label,selection,green):
                    args=[*adb,'shell','am','instrument','-w','-r','-e','fixturePort',str(fixture_port),'-e','fixtureToken',token]
                    if selection: args+=['-e','class',selection]
                    output=run([*args,'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
                    (logs/(label+'.log')).write_text(output)
                    if green:
                        good='OK (' in output and 'FAILURES' not in output and 'INSTRUMENTATION_FAILED' not in output
                    else:
                        good='FAILURES' in output and ('AssertionError' in output or 'ComparisonFailure' in output)
                    if not good:print(output,flush=True);raise RuntimeError('Device assertion expected '+('green' if green else 'red'))
                    return output
                build('baseline')
                selection=None if os.environ.get('ANDROID_SSH_ALL')=='1' else os.environ.get('ANDROID_SSH_TEST_CLASS','com.hanxi.ghosterm.RemoteTerminalTest')
                output=test('baseline',selection,True)
                (ROOT/'tmp/android-a3-ssh-device.log').write_text(output)
                if os.environ.get('ANDROID_SSH_MUTATE')=='1':
                    spec=importlib.util.spec_from_file_location('mutations',ROOT/'scripts/mutation-check-android-ssh.py')
                    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
                    module.execute(ROOT,build,test)
                if os.environ.get('ANDROID_SSH_PRODUCT_MUTATE')=='1':
                    spec=importlib.util.spec_from_file_location('product_mutations',ROOT/'scripts/mutation-check-android-ssh-product.py')
                    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
                    module.execute(ROOT,build,test)
                if os.environ.get('ANDROID_SSH_IMPORT_MUTATE')=='1':
                    spec=importlib.util.spec_from_file_location('import_mutations',ROOT/'scripts/mutation-check-android-identity-import.py')
                    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
                    module.execute(ROOT,build,test)
                if os.environ.get('ANDROID_SSH_CREDENTIAL_MUTATE')=='1':
                    spec=importlib.util.spec_from_file_location('credential_mutations',ROOT/'scripts/mutation-check-android-credentials.py')
                    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
                    module.execute(ROOT,build,test)
                if os.environ.get('ANDROID_SSH_IDENTITIES_MUTATE')=='1':
                    spec=importlib.util.spec_from_file_location('identity_mutations',ROOT/'scripts/mutation-check-android-identities.py')
                    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
                    module.execute(ROOT,build,test)
                if os.environ.get('ANDROID_SSH_HOST_IDENTITIES_MUTATE')=='1':
                    spec=importlib.util.spec_from_file_location('host_identity_mutations',ROOT/'scripts/mutation-check-android-host-identities.py')
                    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
                    module.execute(ROOT,build,test)
                if os.environ.get('ANDROID_SSH_KNOWN_HOSTS_SETTINGS_MUTATE')=='1':
                    spec=importlib.util.spec_from_file_location('known_host_settings_mutations',ROOT/'scripts/mutation-check-android-known-hosts-settings.py')
                    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
                    module.execute(ROOT,build,test)
                print(output,flush=True);print('ANDROID SSH PASS',flush=True)
            finally:
                for value in reversed(mappings):run([*adb,'reverse','--remove','tcp:'+str(value)])
                server.shutdown();worker.join()
if __name__=='__main__':
    def interrupt(signum,frame):raise KeyboardInterrupt(signum)
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupt)
    main()
