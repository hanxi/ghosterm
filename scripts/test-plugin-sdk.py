#!/usr/bin/env python3
"""Portable SDK: strict TS, schema parity, actual public C Session + QuickJS-NG.
Default offline; --fetch only permits integrity-pinned developer dependencies.
Mutations edit isolated copies, force TS/C links and require normal assertions.
"""
import argparse, hashlib, importlib.util, json, os
from pathlib import Path
import shutil, signal, subprocess, sys, tempfile
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_PLUGIN_SDK_MUTATION'
CASES = ['codec', 'roundtrip', 'input', 'incoming-schema', 'output-schema', 'result-schema',
         'queued-cancel', 'queued-timeout', 'sent-cancel', 'sent-timeout', 'dispose', 'stale',
         'admission', 'capacity', 'outcomes', 'confirmation']
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true'); parser.add_argument('--mutate', action='store_true')
    args = parser.parse_args()
    sdk = ROOT / 'packages/plugin-sdk'
    inputs = [p for p in sdk.rglob('*') if p.is_file()]
    inputs += [p for folder in ['core/include', 'core/src', 'core/tests'] for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ['.c', '.h', '.inc']]
    inputs += [ROOT/p for p in ['core/CMakeLists.txt', 'scripts/test-plugin-sdk.py', 'scripts/probes/plugin-sdk.c', 'scripts/test-plugin-js.py', 'scripts/generate-plugin-sdk.py', 'scripts/fixtures/plugin-sdk-tools/package.json', 'scripts/fixtures/plugin-sdk-tools/package-lock.json']]
    for p in [*sdk.glob('portable/*.ts'), ROOT/'scripts/probes/plugin-sdk.c']:
        if MARK in p.read_text():
            print('residual mutation: '+str(p)); raise SystemExit(2)
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    work = Path(tempfile.mkdtemp(prefix='plugin-sdk-', dir=ROOT/'tmp')); logs = work/'logs'; logs.mkdir()
    source = work/'source'; commands = []
    env = dict(os.environ, TMPDIR=str(work), ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    def run(label, command, budget=180, assertion=False):
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-sdk; shift; gt_timeout "$@"'
        p = subprocess.run(['bash', '-c', shell, 'plugin-sdk', str(ROOT), str(budget), *map(str, command)],
                           cwd=ROOT, env=env, capture_output=True, text=True, timeout=budget+10)
        out = p.stdout+p.stderr; (logs/(label+'.log')).write_text(out)
        commands.append(dict(label=label, returncode=p.returncode, assertion=assertion))
        if assertion:
            if p.returncode != 1 or not p.stderr.startswith('ASSERT: Error: ASSERT ') or any(s in out for s in ['AddressSanitizer', 'runtime error:', 'FAIL ', 'JS:']):
                raise RuntimeError(label+': expected normal SDK assertion\n'+out)
        elif p.returncode: raise RuntimeError(label+'\n'+out)
        return out
    try:
        fixture = ROOT/'scripts/fixtures/plugin-sdk-tools'; tools = work/'tools'; tools.mkdir()
        for name in ['package.json', 'package-lock.json']: shutil.copyfile(fixture/name, tools/name)
        lock = json.loads((tools/'package-lock.json').read_text())
        for name, entry in lock['packages'].items():
            if name and (not entry['resolved'].startswith('https://registry.npmjs.org/') or not entry['integrity'].startswith('sha512-')): raise RuntimeError('unpinned tool')
        npmrc = work/'empty.npmrc'; npmrc.touch(); env['NPM_CONFIG_USERCONFIG'] = str(npmrc)
        command = ['npm', 'ci', '--prefix', tools, '--ignore-scripts', '--no-audit', '--no-fund', '--cache', ROOT/'tmp/plugin-sdk-npm-cache', '--registry', 'https://registry.npmjs.org']
        if not args.fetch: command.append('--offline')
        run('tools', command)
        shutil.copytree(sdk, source/'packages/plugin-sdk')
        shutil.copytree(ROOT/'core', source/'core', ignore=shutil.ignore_patterns('build', 'build-*'))
        (source/'scripts/probes').mkdir(parents=True); shutil.copyfile(ROOT/'scripts/probes/plugin-sdk.c', source/'scripts/probes/plugin-sdk.c')
        run('projection', ['python3', ROOT/'scripts/generate-plugin-sdk.py', '--check'])
        spec = importlib.util.spec_from_file_location('sdk_engine_probe', ROOT/'scripts/test-plugin-js.py')
        engine = importlib.util.module_from_spec(spec); spec.loader.exec_module(engine)
        engine_root = engine.unpack(engine.ENGINES['quickjs-ng'], work, args.fetch)
        cc, flags, objects, target = engine.build_engine('quickjs-ng', engine_root, work, False)
        core = work/'core-build'
        run('core-configure', ['cmake', '-S', source/'core', '-B', core, '-DGHOSTERM_WITH_LIBSSH2=OFF', '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        run('core-build', ['cmake', '--build', core, '--target', 'ghosterm_core', '-j', '2'])
        tsc = tools/'node_modules/typescript/bin/tsc'; emitted = work/'emitted'; modules = work/'modules'
        def compile_sdk(label):
            if emitted.exists(): shutil.rmtree(emitted)
            run(label+'-ts', ['node', tsc, '-p', source/'packages/plugin-sdk/testkit/tsconfig.json', '--outDir', emitted])
            modules.mkdir(exist_ok=True)
            for folder in ['portable', 'testkit']:
                for p in (emitted/folder).glob('*.js'): shutil.copyfile(p, modules/p.name)
            probe = source/'scripts/probes/plugin-sdk.c'; obj = target/'sdk.o'; binary = target/'sdk'
            obj.unlink(missing_ok=True); binary.unlink(missing_ok=True)
            run(label+'-cc', [cc, *flags, '-Wall', '-Wextra', '-Werror', '-I', source/'core/include', '-c', probe, '-o', obj])
            run(label+'-link', [cc, *flags, obj, *objects, core/'libghosterm_core.a', '-lm', '-pthread', '-o', binary])
            assert obj.is_file() and binary.is_file()
            return binary
        binary = compile_sdk('baseline')
        for case in CASES: run('baseline-'+case, [binary, modules, case], budget=20)
        # Compare emitted codec schemas against independent, pinned Ajv. This is
        # developer tooling only; portable modules do not import Ajv or Node.
        parity = work/'schema-parity.mjs'
        parity.write_text('''import Ajv from './tools/node_modules/ajv/dist/ajv.js';
import {object,text,integer,snapshot} from './modules/codec.js';
const codec=object({text:text(2),count:integer(0,100)}), validate=new Ajv({strict:true}).compile(codec.schema);
let count=0;for(const text of ['', '中', '😀😀', '😀😀😀', 1, null])for(const countValue of [-1,0,100,101,1.5,'1']){
 const value={text,count:countValue};let accepted=true;try{codec.parse(snapshot(value))}catch{accepted=false}
 if(accepted!==validate(value))throw Error('schema parity');count++;
}console.log('PASS schema parity '+count);
''')
        run('schema-parity', ['node', parity])
        cases = [
            ('input-schema', 'session.ts', 'encode(method.input.parse(snapshot(input)))', 'encode(snapshot(input))', 'input'),
            ('result-schema', 'session.ts', 'value: codec.parse(o.value)', 'value: (void codec, o.value as T)', 'result-schema'),
            ('late-check', 'session.ts', 'if (!this.#live || item.cancelled || this.#port.check(ticket) !== "ok")', 'if (false && (!this.#live || item.cancelled || this.#port.check(ticket) !== "ok"))', 'sent-cancel'),
            ('dispose-ledger', 'session.ts', 'this.#incoming.clear();', '// retained mutant', 'dispose'),
            ('receipt', 'session.ts', 'result.state !== "succeeded" || !result.confirmed', 'result.state !== "succeeded"', 'confirmation'),
            ('capacity', 'session.ts', 'if (this.#pending.size >= 64)', 'if (false && this.#pending.size >= 64)', 'capacity'),
            ('uint64-max', 'codec.ts', '(value.length === 20 && value > "18446744073709551615")', '(value.length === 20 && value > "99999999999999999999")', 'codec'),
            ('utf16', 'codec.ts', 'if (!(low >= 0xdc00 && low <= 0xdfff))', 'if (false && !(low >= 0xdc00 && low <= 0xdfff))', 'codec'),
            ('snapshot-freeze', 'codec.ts', 'return Object.freeze(array ? items : out);', 'return array ? items : out;', 'codec'),
        ]
        if args.mutate:
            for label, name, old, new, case in cases:
                path = source/'packages/plugin-sdk/portable'/name; original = path.read_bytes(); assert old in original.decode(), label
                path.write_text(original.decode().replace(old,new)+'\n// '+MARK+'\n')
                try:
                    binary = compile_sdk(label); run(label+'-target', [binary, modules, case], budget=20, assertion=True)
                    control = 'roundtrip' if case == 'codec' else 'codec'
                    run(label+'-control', [binary, modules, control], budget=20)
                finally: path.write_bytes(original)
                binary = compile_sdk(label+'-restored'); run(label+'-restored', [binary, modules, case], budget=20)
                assert sha(path) == hashlib.sha256(original).hexdigest(); print(label+': TS/C linked assertion red / independent control green / rebuilt restore / SHA256', flush=True)
        for case in CASES: run('final-'+case, [binary, modules, case], budget=20)
        for name, digest in hashes.items(): assert sha(ROOT/name) == digest, name
        receipt = dict(accepted=True, cases=CASES, schema_parity=36, mutation_groups=[c[0] for c in cases] if args.mutate else [],
                       inputs_sha256=hashes, commands=commands, engine=engine.ENGINES['quickjs-ng'],
                       emitted_sha256={p.name:sha(p) for p in modules.glob('*.js')}, binary_sha256=sha(binary),
                       core_sha256=sha(core/'libghosterm_core.a'), sanitizers=['address','undefined'], leak_sanitizer=False,
                       simulated_adapter_cases=['confirmation'], pure_value_cases=['codec'], pre_admission_cases=['input'],
                       scope='portable SDK and macOS C/QuickJS conformance; no production runtime/native JS adapter, App UI, device, SSH, gateway or release claim')
        (logs/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
        shutil.rmtree(source); shutil.rmtree(core); shutil.rmtree(engine_root); shutil.rmtree(target)
        print('PLUGIN SDK PASS; logs: '+str(logs), flush=True)
    except BaseException as error: (logs/'failure.txt').write_text(repr(error)+'\n'); raise
def stop(number, _frame): raise KeyboardInterrupt('signal '+str(number))
if __name__ == '__main__':
    signal.signal(signal.SIGINT,stop); signal.signal(signal.SIGTERM,stop); main()
