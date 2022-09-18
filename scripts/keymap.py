#!/usr/bin/env python3

# shift, control, alt, super

codes = {
    32 : 'space',
    39 : 'apostrophe',
    44 : 'comma',
    45 : 'minus',
    46 : 'period',
    47 : 'slash',
    59 : 'semicolon',
    61 : 'equal',
    91 : 'left_bracket',
    92 : 'backslash',
    93 : 'right_bracket',
    96 : 'grave_accent',
    161 : 'world_1',
    162 : 'world_2',
    256 : 'escape',
    257 : 'enter',
    258 : 'tab',
    259 : 'backspace',
    260 : 'insert',
    261 : 'delete',
    262 : 'right',
    263 : 'left',
    264 : 'down',
    265 : 'up',
    266 : 'page_up',
    267 : 'page_down',
    268 : 'home',
    269 : 'end',
    280 : 'caps_lock',
    281 : 'scroll_lock',
    282 : 'num_lock',
    283 : 'print_screen',
    284 : 'pause',
    290 : 'f1',
    291 : 'f2',
    292 : 'f3',
    293 : 'f4',
    294 : 'f5',
    295 : 'f6',
    296 : 'f7',
    297 : 'f8',
    298 : 'f9',
    299 : 'f10',
    300 : 'f11',
    301 : 'f12',
    302 : 'f13',
    303 : 'f14',
    304 : 'f15',
    305 : 'f16',
    306 : 'f17',
    307 : 'f18',
    308 : 'f19',
    309 : 'f20',
    310 : 'f21',
    311 : 'f22',
    312 : 'f23',
    313 : 'f24',
    314 : 'f25',
    320 : 'pad_0',
    321 : 'pad_1',
    322 : 'pad_2',
    323 : 'pad_3',
    324 : 'pad_4',
    325 : 'pad_5',
    326 : 'pad_6',
    327 : 'pad_7',
    328 : 'pad_8',
    329 : 'pad_9',
    330 : 'pad_decimal',
    331 : 'pad_divide',
    332 : 'pad_multiply',
    333 : 'pad_subtract',
    334 : 'pad_add',
    335 : 'pad_enter',
    336 : 'pad_equal',
    340 : 'left_shift',
    341 : 'left_control',
    342 : 'left_alt',
    343 : 'left_super',
    344 : 'right_shift',
    345 : 'right_control',
    346 : 'right_alt',
    347 : 'right_super',
    384 : 'menu',
}

# add hindu-arabic digits
for i in range(48,58):
    codes[i] = "digit_%s" % chr(i)
# add roman letters (lower case)
for i in range(97,123):
    codes[i] = "roman_%s" % chr(i)

keys = {v: k for k, v in codes.items()}

def quote(s):
	if s == '\\':
		return '"\\\\"'
	elif s == '"':
		return '"\\""'
	else:
		return '\"%s\"' % s

def map_shift(k, v):
	print("%-40s -> emit %s;" % ("shift + %s" % codes[ord(k)], quote(v)))

def map_special(k, v):
	print("%-40s -> emit %s;" % (k, v))

def map_ctrl(k, v):
	print("%-40s -> emit %s;" % ("ctrl + %s" % k, v))

def map_direct(i):
	print("%-40s -> emit %s;" % (codes[i], quote(chr(i))))

def map_capital(i):
	print("%-40s -> emit %s;" % ("shift + %s" % codes[i], quote(chr(i-32))))

def map_custom(k, v):
	print("%-40s -> %s;" % (k, v))

def comment(c):
	print("\n# %s" % c)

print('#')
print('# cutty US english keyboard mapping')
print('#')

comment('regular keys')
# direct map keys excluding roman alphabet
for i in range(32,127):
	if i in codes:
		map_direct(i)

comment('regular keys shifted')
map_shift('\'', '"')
map_shift(',', '<')
map_shift('-', '_')
map_shift('.', '>')
map_shift('/', '?')
map_shift(';', ':')
map_shift('=', '+')
map_shift('1', '!')
map_shift('2', '@')
map_shift('3', '#')
map_shift('4', '$')
map_shift('5', '%')
map_shift('6', '^')
map_shift('7', '&')
map_shift('8', '*')
map_shift('9', '(')
map_shift('0', ')')
map_shift('[', '{')
map_shift('\\', '|')
map_shift(']', '}')
map_shift('`', '~')

# roman alphabet (shift capital)
for i in range(97,123):
	map_capital(i)

comment('control characters')
map_ctrl('space', 'NUL')
map_ctrl('roman_a', 'SOH')
map_ctrl('roman_b', 'STX')
map_ctrl('roman_c', 'ETX')
map_ctrl('roman_d', 'EOT')
map_ctrl('roman_e', 'ENQ')
map_ctrl('roman_f', 'ACK')
map_ctrl('roman_g', 'BEL')
map_ctrl('roman_h', 'BS')
map_ctrl('roman_i', 'HT')
map_ctrl('roman_j', 'LF')
map_ctrl('roman_k', 'VT')
map_ctrl('roman_l', 'FF')
map_ctrl('roman_m', 'CR')
map_ctrl('roman_n', 'SO')
map_ctrl('roman_o', 'SI')
map_ctrl('roman_p', 'DLE')
map_ctrl('roman_q', 'DC1')
map_ctrl('roman_r', 'DC2')
map_ctrl('roman_s', 'DC3')
map_ctrl('roman_t', 'DC4')
map_ctrl('roman_u', 'NAK')
map_ctrl('roman_v', 'SYN')
map_ctrl('roman_w', 'ETB')
map_ctrl('roman_x', 'CAN')
map_ctrl('roman_y', 'EM')
map_ctrl('roman_z', 'SUB')
map_ctrl('left_bracket', 'ESC')
map_ctrl('backslash', 'FS')
map_ctrl('right_bracket', 'GS')
#map_ctrl('^^', 'RS') # ?
#map_ctrl('^_', 'US') # ?
map_ctrl('backspace', 'BS')

comment('special keys')
map_special('escape', "ESC")
map_special('enter', "LF")
map_special('tab', 'HT')
map_special('backarrow_sends_delete=0 backspace', 'BS')
map_special('backarrow_sends_delete=1 backspace', 'DEL')
map_special('insert', 'CSI "2~"')
map_special('delete', 'CSI "3~"')
map_special('page_up', 'CSI "5~"')
map_special('page_down', 'CSI "6~"')
map_special('app_cursor_keys=0 up' , 'CSI "A"')
map_special('app_cursor_keys=0 down' , 'CSI "B"')
map_special('app_cursor_keys=0 right' , 'CSI "C"')
map_special('app_cursor_keys=0 left' , 'CSI "D"')
map_special('app_cursor_keys=0 home' , 'CSI "H"')
map_special('app_cursor_keys=0 end' , 'CSI "F"')
map_special('app_cursor_keys=0 f1' , 'CSI "P"')
map_special('app_cursor_keys=0 f2' , 'CSI "Q"')
map_special('app_cursor_keys=0 f3' , 'CSI "R"')
map_special('app_cursor_keys=0 f4' , 'CSI "S"')
map_special('app_cursor_keys=1 up' , 'SS3 "A"')
map_special('app_cursor_keys=1 down' , 'SS3 "B"')
map_special('app_cursor_keys=1 right' , 'SS3 "C"')
map_special('app_cursor_keys=1 left' , 'SS3 "D"')
map_special('app_cursor_keys=1 home' , 'SS3 "H"')
map_special('app_cursor_keys=1 end' , 'SS3 "F"')
map_special('app_cursor_keys=1 f1' , 'SS3 "P"')
map_special('app_cursor_keys=1 f2' , 'SS3 "Q"')
map_special('app_cursor_keys=1 f3' , 'SS3 "R"')
map_special('app_cursor_keys=1 f4' , 'SS3 "S"')
map_special('f5' , 'CSI "15~"')
map_special('f6' , 'CSI "17~"')
map_special('f7' , 'CSI "18~"')
map_special('f8' , 'CSI "19~"')
map_special('f9' , 'CSI "20~"')
map_special('f10' , 'CSI "21~"')
map_special('f11' , 'CSI "23~"')
map_special('f12' , 'CSI "24~"')

comment('special keys shifted')
map_special('shift + insert', 'CSI "2;2~"')
map_special('shift + delete', 'CSI "3;2~"')
map_special('shift + page_up', 'CSI "5;2~"')
map_special('shift + page_down', 'CSI "6;2~"')
map_special('shift + up' , 'CSI "1;2A"')
map_special('shift + down' , 'CSI "1;2B"')
map_special('shift + right' , 'CSI "1;2C"')
map_special('shift + left' , 'CSI "1;2D"')
map_special('shift + home' , 'CSI "1;2H"')
map_special('shift + end' , 'CSI "1;2F"')
map_special('shift + f1' , 'CSI "1;2P"')
map_special('shift + f2' , 'CSI "1;2Q"')
map_special('shift + f3' , 'CSI "1;2R"')
map_special('shift + f4' , 'CSI "1;2S"')
map_special('shift + f5' , 'CSI "15;2~"')
map_special('shift + f6' , 'CSI "17;2~"')
map_special('shift + f7' , 'CSI "18;2~"')
map_special('shift + f8' , 'CSI "19;2~"')
map_special('shift + f9' , 'CSI "20;2~"')
map_special('shift + f10' , 'CSI "21;2~"')
map_special('shift + f11' , 'CSI "23;2~"')
map_special('shift + f12' , 'CSI "24;2~"')

comment('internal')
map_custom('ctrl_cmd + roman_c', 'copy')
map_custom('ctrl_cmd + roman_v', 'paste')
