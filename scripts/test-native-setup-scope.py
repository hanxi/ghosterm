#!/usr/bin/env python3
"""Owned fixture scopes are lexical boundaries, never a request to run SSH."""
import runpy,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
fixture_paths=runpy.run_path(str(ROOT/'scripts/probe-native-linux-setup.py'))['fixture_paths']
class ScopeTests(unittest.TestCase):
    def test_legacy_siblings(self):
        env={'GHOSTERM_REPLICA_RUNTIME':'/owned/ghosterm-test-run','GHOSTERM_SESSION_SETUP_ROOT':'/owned/ghosterm-test-install'}
        self.assertEqual(fixture_paths(env),(Path(env['GHOSTERM_REPLICA_RUNTIME']),Path(env['GHOSTERM_SESSION_SETUP_ROOT']),Path(env['GHOSTERM_REPLICA_RUNTIME'])))
    def test_owned_nested_runtime(self):
        env={'GHOSTERM_REPLICA_RUNTIME':'/owned/ghosterm-test-run/s/contract','GHOSTERM_TEST_RUNTIME_SCOPE':'/owned/ghosterm-test-run','GHOSTERM_SESSION_SETUP_ROOT':'/owned/ghosterm-test-install'}
        self.assertEqual(fixture_paths(env)[2],Path('/owned/ghosterm-test-run'))
    def test_foreign_paths_rejected(self):
        base={'GHOSTERM_REPLICA_RUNTIME':'/owned/ghosterm-test-run/s/contract','GHOSTERM_TEST_RUNTIME_SCOPE':'/owned/ghosterm-test-run','GHOSTERM_SESSION_SETUP_ROOT':'/owned/ghosterm-test-install'}
        for edits in (
            {'GHOSTERM_REPLICA_RUNTIME':'/owned/other/s/contract'},
            {'GHOSTERM_REPLICA_RUNTIME':'/owned/ghosterm-test-run/../other'},
            {'GHOSTERM_TEST_RUNTIME_SCOPE':'/owned/user-data'},
            {'GHOSTERM_TEST_RUNTIME_SCOPE':'relative'},
            {'GHOSTERM_SESSION_SETUP_ROOT':'/other/ghosterm-test-install'},
            {'GHOSTERM_SESSION_SETUP_ROOT':'/owned/user-data'},
            {'GHOSTERM_REPLICA_RUNTIME':'/owned/ghosterm-test-run/a/b/c/d/e'},
        ):
            with self.subTest(edits=edits),self.assertRaises(RuntimeError):fixture_paths(dict(base,**edits))
if __name__=='__main__':unittest.main()
