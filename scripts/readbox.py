#!/usr/bin/env python3

import re
import math
import argparse

# simplify box file by clamping boxes to grid and accumulating
# adjacent characters outputing in the format: row,col "string"
def simplify_box_file(input_file, width, height, cellw, cellh):
    data = []
    with open(input_file) as f:
        lines = f.readlines()
        lcol = -1
        lrow = -1
        text = ''
        for line in lines:
            d = list(map(str.strip, line.split(' ')))
            c, x1, y1, x2, y2 = d[0], float(d[1]), float(d[2]), float(d[3]), float(d[4])
            col = 1 + int(math.floor(((x1 / cellw) + (x2 / cellw)) / 2.0))
            row = 1 + int(math.floor((((height-y1) / cellh) + ((height-y2) / cellh)) / 2.0))
            if lrow == row and lcol == col-1:
                text += c
            elif lrow == row and lcol == col-2:
                text += ' ' + c
            else:
                if len(text) > 0:
                    data.append({ "text" : text, "row" : srow, "col" : scol})
                text = c
                scol = col
                srow = row
            lcol = col
            lrow = row
        if len(text) > 0:
            data.append({ "text" : text, "row" : srow, "col" : scol})
    return data

# read simplified box file
def read_sbox_file(input_file):
    data = []
    reg = re.compile("(\d+),(\d+)\s\"(.*)\"")
    with open(input_file) as f:
        lines = f.readlines()
        for line in lines:
            m = reg.match(line)
            data.append({ "text" : m.group(3),
                "row" : int(m.group(1)), "col" : int(m.group(2))})
    return data

# write simplified box file
def write_sbox_file(data, output_file):
    with open(output_file, 'w') as f:
        for d in data:
            print("%d,%d \"%s\"" % (d['row'], d['col'], d['text']), file=f)

#
# main program
#

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='simplify box file')
    parser.add_argument('-n', '--test-name', action='store',
                        help='test name')
    parser.add_argument('-i', '--input-box-file', action='store',
                        help='input box file', required=True)
    parser.add_argument('-o', '--output-sbox-file', action='store',
                        help='output sbox file')
    parser.add_argument('-e', '--exemplar-sbox-file', action='store',
                        help='exemplar sbox file')
    parser.add_argument('--rows', action='store', type=int,
                        help='cell grid rows', default=24)
    parser.add_argument('--cols', action='store', type=int,
                        help='cell grid cols', default=80)
    parser.add_argument('--width', action='store', type=int,
                        help='width in pixels', default=1200)
    parser.add_argument('--height', action='store', type=int,
                        help='height in pixels', default=800)

    args = parser.parse_args()
    cols = args.cols
    rows = args.rows
    width = args.width
    height = args.height
    cellw = width/cols
    cellh = height/rows

    data1 = simplify_box_file(args.input_box_file, width, height, cellw, cellh)
    if args.output_sbox_file:
        write_sbox_file(data1, args.output_sbox_file)
    if args.exemplar_sbox_file:
        data2 = read_sbox_file(args.exemplar_sbox_file)
        if data1 == data2:
            print("%s: PASS" % args.test_name)
        else:
            print("%s: FAIL" % args.test_name)
