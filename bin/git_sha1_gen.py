#!/usr/bin/env python

import os.path
import subprocess
import sys

git_dir = os.path.join(os.path.dirname(sys.argv[0]), '..', '.git')
try:
    git_sha1 = subprocess.check_output([
        'git',
        '--git-dir=' + git_dir,
        'rev-parse',
        '--short=10',
        'HEAD',
    ], stderr=open(os.devnull, 'w'))
except subprocess.CalledProcessError as e:
    # don't print anything if git fails
    pass
else:
    sys.stdout.write('#define MESA_GIT_SHA1 "git-%s"\n' % git_sha1.rstrip())
