#!/usr/bin/env python3

import os
import re
import glob
import sbox
import argparse
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

def run_test(test, trace):
    global pass_count
    test_name = re.sub(r'tests/(.*)\.sbox$', r'\1', test)
    test_exe = 'build/%s' % test_name
    capture_png = 'tmp/%s.png' % test_name
    capture_prefix = 'tmp/%s' % test_name
    test_box = 'tmp/%s.box' % test_name
    test_sbox_out = 'tmp/%s.sbox' % test_name
    if os.path.exists(test_exe):
        if trace:
            capture_cmd = [ 'build/capture', '-t', '-o', capture_png, '-x', test_exe ]
        else:
            capture_cmd = [ 'build/capture', '-o', capture_png, '-x', test_exe ]
        ret = subprocess.run(capture_cmd, check=True, env=mesa_env)
        tesseract_cmd = [ 'tesseract', '--dpi', '72', '--psm', '6', '-l', 'eng', capture_png, capture_prefix, 'makebox' ]
        ret = subprocess.run(tesseract_cmd, check=True)
        data1 = sbox.simplify_box_file(test_box, 1200, 800, 1200/80, 800/24)
        data2 = sbox.read_sbox_file(test)
        if data1 == data2:
            print("%-72s: PASS" % test_name)
            pass_count += 1
        else:
            print("%-72s: FAIL" % test_name)
            sbox.write_sbox_file(data1, test_sbox_out)

parser = argparse.ArgumentParser(description='run tests')
parser.add_argument('-r', '--test-name', action='store',
                    help='test name')
parser.add_argument('-t', '--trace', default=False, action='store_true',
                    help='enable tracing')
args = parser.parse_args()

pass_count = 0

if args.test_name:
    run_test(args.test_name, args.trace)
else:
    for test_sbox in sbox_files:
        run_test(test_sbox, args.trace)

print("=== %d out of %d pass ===" % (pass_count, len(sbox_files)))

if pass_count != len(sbox_files):
    exit(9)
