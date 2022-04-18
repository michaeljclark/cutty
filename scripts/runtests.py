#!/usr/bin/env python3

import os
import re
import glob
import readbox
import subprocess

mesa_env_overrides = {
    'MESA_GL_VERSION_OVERRIDE': '3.2',
    'MESA_GLSL_VERSION_OVERRIDE': '150',
    'DYLD_LIBRARY_PATH': '/opt/llvm/lib'
}
mesa_env = {**os.environ, **mesa_env_overrides}

# find and run test cases with exemplars
sbox_files = sorted(glob.glob('tests/*.sbox'))

print("=== running %d tests ===" % len(sbox_files))

pass_count = 0
for test_sbox in sbox_files:
    test_name = re.sub(r'tests/(.*)\.sbox$', r'\1', test_sbox)
    test_exe = 'build/%s' % test_name
    capture_png = 'tmp/%s.png' % test_name
    capture_prefix = 'tmp/%s' % test_name
    test_box = 'tmp/%s.box' % test_name
    if os.path.exists(test_exe):
        capture_cmd = [ 'build/capture', '-o', capture_png, '-x', test_exe ]
        ret = subprocess.run(capture_cmd, check=True, env=mesa_env)
        tesseract_cmd = [ 'tesseract', '--psm', '6', '-l', 'eng', capture_png, capture_prefix, 'makebox' ]
        ret = subprocess.run(tesseract_cmd, check=True)
        data1 = readbox.simplify_box_file(test_box, 1200, 800, 1200/80, 800/24)
        data2 = readbox.read_sbox_file(test_sbox)
        if data1 == data2:
            print("%s: PASS" % test_name)
            pass_count += 1
        else:
            print("%s: FAIL" % test_name)

print("=== %d out of %d pass ===" % (pass_count, len(sbox_files)))

if pass_count != len(sbox_files):
    exit(9)
