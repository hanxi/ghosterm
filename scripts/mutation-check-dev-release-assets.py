#!/usr/bin/env python3
"""Prove shared Release checks reject replacement/lost assets and byte mismatches."""
import copy
import hashlib
import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('assets', ROOT / 'scripts/check-dev-release-assets.py')
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


def main():
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dev-release-assets-', dir=ROOT / 'tmp') as temporary:
        folder = Path(temporary)
        names = sorted(set.union(*check.FILES.values()))
        for name in names:
            (folder / name).write_bytes(('independent ' + name).encode())
        baseline = dict(id=123, assets=[dict(id=i, name=name, size=(folder/name).stat().st_size,
            digest='sha256:' + hashlib.sha256((folder/name).read_bytes()).hexdigest()) for i, name in enumerate(names)])
        for platform in check.FILES:
            check.verify(platform, folder, baseline, baseline)
            own = check.FILES[platform]
            other = set(names) - own
            variants = []
            for label, operation in [
                ('missing-own', lambda v: v['assets'].remove(next(a for a in v['assets'] if a['name'] in own))),
                ('missing-other', lambda v: v['assets'].remove(next(a for a in v['assets'] if a['name'] in other))),
                ('replaced-release', lambda v: v.update(id=124)),
                ('changed-other-id', lambda v: next(a for a in v['assets'] if a['name'] in other).update(id=999)),
                ('changed-own-bytes', lambda v: next(a for a in v['assets'] if a['name'] in own).update(digest='sha256:'+'0'*64)),
                ('changed-own-size', lambda v: next(a for a in v['assets'] if a['name'] in own).update(size=999)),
                ('stale-archive', lambda v: v['assets'].append(dict(id=998,name='Ghosterm-0.0.0-arm64.zip',size=0,digest='sha256:'+'0'*64))),
                ('duplicate-name', lambda v: v['assets'].append(copy.deepcopy(v['assets'][0]))),
                ('unknown-release-id', lambda v: v.update(id=None)),
                ('unknown-other-id', lambda v: next(a for a in v['assets'] if a['name'] in other).update(id=None)),
                ('unknown-digest', lambda v: next(a for a in v['assets'] if a['name'] in own).update(digest=None)),
            ]:
                value = copy.deepcopy(baseline);operation(value);variants.append((label,value))
            for label, value in variants:
                try:
                    check.verify(platform, folder, value, baseline)
                except ValueError:
                    pass
                else:
                    raise AssertionError(platform + '/' + label + ': expected integrity rejection')
                # Same mutation: the opposite platform's local files remain intact.
                assert all('sha256:'+hashlib.sha256((folder/a['name']).read_bytes()).hexdigest()==a['digest']
                           for a in baseline['assets'] if a['name'] in other)
                check.verify(platform, folder, baseline, baseline)
                print(platform+'/'+label+': target red / independent files green / restored green')
        legacy = ['Ghosterm-0.1.0-dev-arm64.zip', 'Ghosterm-macos-arm64-old.dmg']
        assert check.obsolete_macos(names+legacy+['Ghosterm-linux-arm64-dev.zip','readme.zip'])==sorted(legacy)
        assert check.obsolete_macos(names)==[]
    print('SHARED DEV RELEASE MUTATIONS PASS: 22 groups; platform cleanup namespace controls green')


if __name__ == '__main__':
    main()
