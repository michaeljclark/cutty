#!/usr/bin/env python3
#
# program to run terminal test cases using the capture tool
#
# - fast-mode - capture program outputs box files directly
#               using the same logic as the cellgrid renderer.
# - ocr-mode  - capture program outputs image files which are
#               converted to text using tesseract OCR.
#

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

def run_test(test, trace, ocr):
    test_name = re.sub(r'tests/(.*)\.sbox$', r'\1', test)
    test_exe = 'build/%s' % test_name

    if not os.path.exists(test_exe):
        return

    capture_png = 'tmp/%s.png' % test_name
    capture_prefix = 'tmp/%s' % test_name
    test_box = 'tmp/%s.box' % test_name
    capture_sbox = 'tmp/%s.sbox' % test_name
    capture_cmd = [ 'build/capture' ]
    if trace:
        capture_cmd.append('-t')
    if ocr:
        capture_cmd.extend([ '-o', capture_png, '-x', test_exe ])
        ret = subprocess.run(capture_cmd, check=True, env=mesa_env)
        tesseract_cmd = [ 'tesseract', '--dpi', '72', '--psm', '6',
            '-l', 'eng', capture_png, capture_prefix, 'makebox' ]
        ret = subprocess.run(tesseract_cmd, check=True)
        data1 = sbox.simplify_box_file(test_box, 1200, 800, 1200/80, 800/24)
        data2 = sbox.read_sbox_file(test)
    else:
        capture_cmd.extend([ '-s', capture_sbox, '-x', test_exe ])
        ret = subprocess.run(capture_cmd, check=True, env=mesa_env)
        data1 = sbox.read_sbox_file(capture_sbox)
        data2 = sbox.read_sbox_file(test)

    global run_count, pass_count
    run_count += 1
    if data1 == data2:
        print("%-72s: PASS" % test_name)
        pass_count += 1
    else:
        print("%-72s: FAIL" % test_name)
        sbox.write_sbox_file(data1, capture_sbox)

parser = argparse.ArgumentParser(description='run tests')
parser.add_argument('-r', '--test-sbox', action='store',
                    help='run one test specified by sbox file')
parser.add_argument('--ocr', default=False, action='store_true',
                    help='enable tesseract ocr testing mode')
parser.add_argument('--trace', default=False, action='store_true',
                    help='enable tracing')
args = parser.parse_args()

run_count = 0
pass_count = 0

if args.test_sbox:
    sbox_files = [ args.test_sbox ]
else:
    sbox_files = sorted(glob.glob('tests/*.sbox'))

print("=== running %d tests ===" % len(sbox_files))
for test_sbox in sbox_files:
    run_test(test_sbox, args.trace, args.ocr)
print("=== %d of %d succeeded ===" % (pass_count, run_count))

if pass_count != run_count:
    exit(9)
