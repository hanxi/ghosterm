#!/usr/bin/env python3
"""Isolated connection admission mutations: compiled assertions, independent API, SHA restoration."""
import hashlib, importlib.util, os, shutil, signal, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_CONNECTION_CONFIG_MUTATION'
spec=importlib.util.spec_from_file_location('helper',ROOT/'scripts/mutation-check-config-rename.py')
helper=importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)

def main():
    source=ROOT/'core/src/config.c'; original=source.read_bytes(); sha=hashlib.sha256(original).digest()
    if MARK.encode() in original: raise RuntimeError('residual mutation')
    logs=ROOT/'tmp/connection-config-mutations'; logs.mkdir(parents=True,exist_ok=True)
    cases=[
      ('jump','        return !strcasecmp(value, "none");\n','        return true;\n','jump rejected'),
      ('scope','        if (!conditional && n->block >= 0 && !block_matches(&cfg->blocks[n->block], host)) {\n','        if (false && !conditional && n->block >= 0 &&\n            !block_matches(&cfg->blocks[n->block], host)) {\n','other host excluded'),
      ('precedence','            shadowed = seen[key];\n','            shadowed = false && seen[key];\n','shadowed value ignored'),
      ('list','        if (key >= 0 && k_keys[key].kind != K_LIST && n->nargs > 0) {\n','        if (key >= 0 && true && n->nargs > 0) {\n','list is cumulative'),
      ('conditional','        bool conditional = !strcmp(n->keyword, "match");\n','        bool conditional = false;\n','conditional scope refused'),
      ('unknown','  return false;\n}\n\nint gt_config_check_connection','  return true;\n}\n\nint gt_config_check_connection','unknown directive refused'),
      ('expansion','        return strchr(value, \'%\') == NULL && strstr(value, "${") == NULL;\n','        return true && strstr(value, "${") == NULL;\n','unimplemented expansion rejected'),
      ('port-high','        return port > 0 && port <= 65535;\n','        return port > 0 && true;\n','port upper bound'),
      ('port-low','        return port > 0 && port <= 65535;\n','        return true && port <= 65535;\n','port lower bound'),
      ('source','        out->source_path = n->source_path;\n','        out->source_path = "";\n','included source preserved'),
      ('included','        out->included = n->included;\n','        out->included = false;\n','included source preserved'),
      ('line','        out->source_line = n->source_line;\n','        out->source_line = 0;\n','first winning source'),
      ('enumeration','if (index-- != 0)','if (false && index-- != 0)','list is cumulative'),
      ('invalid','    if (!cfg || !host || !*host || !out) {\n        return GT_ERR_INVALID_ARG;\n','    if (!cfg || !host || !*host || !out) {\n        return GT_OK;\n','invalid arguments'),
    ]
    with tempfile.TemporaryDirectory(prefix='connection-config-',dir=ROOT/'tmp') as directory:
        work=Path(directory); (work/'tmp').mkdir()
        shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
        target=work/'core/src/config.c'; build=work/'core/build'
        helper.require(helper.run(['cmake','-S',work/'core','-B',build,'-DCMAKE_BUILD_TYPE=Debug'],logs/'configure.log'))
        def compile(label): helper.require(helper.run(['cmake','--build',build,'--clean-first','--target','test_connection_config','test_api'],logs/(label+'-build.log')))
        def check(label,reason=None):
            result=helper.run([build/'test_connection_config',work/'tmp'],logs/(label+'.log'))
            if reason is None: helper.require(result)
            elif result.returncode!=1 or 'FAIL connection config: '+reason not in result.stderr:
                raise RuntimeError('wrong target failure: '+result.stdout+result.stderr)
        try:
            compile('baseline'); check('baseline')
            for label,old,new,reason in cases:
                text=original.decode()
                if text.count(old)!=1: raise RuntimeError('ambiguous anchor: '+label)
                target.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n')
                compile(label); check(label,reason)
                helper.require(helper.run([build/'test_api'],logs/(label+'-control.log')))
                target.write_bytes(original)
                assert hashlib.sha256(target.read_bytes()).digest()==sha
                compile(label+'-restored'); check(label+'-restored')
                print('PASS '+label+': compiled target red / API green / restored green / SHA256',flush=True)
        finally:
            target.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest()!=sha: raise RuntimeError('production source changed')
    print(f'CONNECTION CONFIG MUTATIONS PASS: {len(cases)} groups')

if __name__=='__main__':
    def interrupt(signum,frame): raise KeyboardInterrupt(signum)
    for sig in (signal.SIGINT,signal.SIGTERM): signal.signal(sig,interrupt)
    os.environ.setdefault('TMPDIR',str(ROOT/'tmp'))
    main()
