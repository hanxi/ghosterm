#!/usr/bin/env python3
"""Observe this owned SSH child's real wait status; do not alter its signals."""
import json
import os
from pathlib import Path
import subprocess
import sys

gterm, directory, home = sys.argv[1:4]
observation = Path(directory)
environment = {'PATH': '/usr/bin:/bin', 'HOME': home, 'TMPDIR': os.environ.get('TMPDIR', ''),
               'GT_EXECUTION_APP_FIXTURE': directory, 'GT_EXECUTION_AMBIENT': 'unselected-fixture'}
with (observation / 'stderr').open('wb') as error:
    child = subprocess.Popen([gterm, *sys.argv[4:]], stdin=sys.stdin.buffer, stdout=sys.stdout.buffer,
                             stderr=error, env=environment)
    (observation / 'gterm-pid').write_text(str(child.pid))
    status = child.wait()
    (observation / 'exit').write_text(json.dumps({'status': status}))
sys.exit(status if status >= 0 else 128 - status)
