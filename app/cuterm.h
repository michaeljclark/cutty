#pragma once

#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <termios.h>

#if defined(__linux__)
#include <pty.h>
#endif
#if defined(__APPLE__)
#include <util.h>
#endif
#if defined(__FreeBSD__)
#include <libutil.h>
#endif

#include <string>
#include <vector>

typedef long long llong;
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long long ullong;

struct circular_buffer
{
    llong sum;
    llong count;
    llong offset;
    llong samples[31];
};

enum cu_cell_flag
{
	cu_cell_bold            = 1 << 0,
	cu_cell_faint           = 1 << 1,
	cu_cell_italic          = 1 << 2,
	cu_cell_underline       = 1 << 3,
	cu_cell_doubleunderline = 1 << 4,
	cu_cell_blink           = 1 << 5,
	cu_cell_inverse         = 1 << 6,
	cu_cell_hidden          = 1 << 7,
	cu_cell_strikethrough   = 1 << 8,
	cu_cell_fraktur         = 1 << 9,
};

enum cu_cell_color
{
	cu_cell_color_black   = 0xff000000,
	cu_cell_color_red     = 0xff000099,
	cu_cell_color_green   = 0xff009900,
	cu_cell_color_yellow  = 0xffdddd00,
	cu_cell_color_blue    = 0xff990000,
	cu_cell_color_magenta = 0xffdd00dd,
	cu_cell_color_cyan    = 0xff00dddd,
	cu_cell_color_white   = 0xffdddddd,
};

enum cu_charset
{
	cu_charset_utf8         = 0,
	cu_charset_iso8859_1    = 1,
};

struct cu_cell
{
	uint codepoint;
	uint flags;
	uint fg_col;
	uint bg_col;
};

enum cu_line_flag
{
	cu_line_packed = (1 << 0)
};

struct cu_line
{
	uint pcount;
	std::vector<cu_cell> cells;
	std::string utf8;

	bool ispacked();
	void pack();
	void unpack();
	void clear();
};

enum cu_char
{
	cu_char_NUL = 0x00, // Null
	cu_char_SOH = 0x01, // ^A - Start Of Heading
	cu_char_STX = 0x02, // ^B - Start Of Text
	cu_char_ETX = 0x03, // ^C - End Of Text
	cu_char_EOT = 0x04, // ^D - End Of Transmission
	cu_char_ENQ = 0x05, // ^E - Enquiry
	cu_char_ACK = 0x06, // ^F - Acknowledge
	cu_char_BEL = 0x07, // ^G - Bell
	cu_char_BS  = 0x08, // ^H - Backspace
	cu_char_HT  = 0x09, // ^I - Horizontal Tab
	cu_char_LF  = 0x0A, // ^J - Line Feed
	cu_char_VT  = 0x0B, // ^K - Vertical Tab
	cu_char_FF  = 0x0C, // ^L - Form Feed
	cu_char_CR  = 0x0D, // ^M - Carriage Return
	cu_char_SO  = 0x0E, // ^N - Shift Out
	cu_char_SI  = 0x0F, // ^O - Shift In
	cu_char_DLE = 0x10, // ^P - Data Link Escape
	cu_char_DC1 = 0x11, // ^Q - Device Control 1
	cu_char_DC2 = 0x12, // ^R - Device Control 2
	cu_char_DC3 = 0x13, // ^S - Device Control 3
	cu_char_DC4 = 0x14, // ^T - Device Control 4
	cu_char_NAK = 0x15, // ^U - Negative Acknowledge
	cu_char_SYN = 0x16, // ^V - Synchronize Idle
	cu_char_ETB = 0x17, // ^W - End Of Transmission Block
	cu_char_CAN = 0x18, // ^X - Cancel
	cu_char_EM  = 0x19, // ^Y - End of Medium
	cu_char_SUB = 0x1A, // ^Z - Substitute
	cu_char_ESC = 0x1B, // ^[ - Escape
	cu_char_FS  = 0x1C, // ^\ - File Separator
	cu_char_GS  = 0x1D, // ^] - Group Separator
	cu_char_RS  = 0x1E, // ^^ - Record Separator
	cu_char_US  = 0x1F, // ^_ - Unit Separator
};

enum cu_clear
{
	cuterm_clear_end,
	cuterm_clear_start,
	cuterm_clear_all
};

enum cu_state
{
	cu_state_normal,
	cu_state_escape,
	cu_state_utf4,
	cu_state_utf3,
	cu_state_utf2,
	cu_state_csi_init,
	cu_state_csi,
	cu_state_csi_dec,
	cu_state_csi_dec2,
	cu_state_csi_dec3,
	cu_state_osc_init,
	cu_state_osc,
	cu_state_osc_string,
	cu_state_charset,
};

enum cu_term_flag
{
	cu_term_wrap            = 1 << 1,
};

struct cu_font_metric
{
	float size;
	float advance;
	float leading;
	float height;
	float ascender;
	float descender;
	float underline_position;
	float underline_thickness;
};

struct cu_dim
{
	int vis_lines;
	int vis_rows;
	int vis_cols;
};

struct cu_term
{
	uint state;
	uint code;
	uint flags;
	uint charset;
	uint argc;
	uint argv[5];
	uint fd;
	uint needs_update;
	std::string osc_string;

    struct winsize ws;
    struct termios tio;
	char slave[PATH_MAX];

	std::vector<uchar> in_buf;
	ssize_t in_start;
	ssize_t in_end;

	std::vector<uchar> out_buf;
	ssize_t out_start;
	ssize_t out_end;

	cu_cell tmpl;
	std::vector<cu_line> lines;
	uint cur_row;
	uint cur_col;
	uint vis_rows;
	uint vis_cols;
	uint vis_lines;
};

void cuterm_init(cu_term *term);
void cuterm_close(cu_term *term);
int cuterm_fork(cu_term *term, uint cols, uint rows);
void cuterm_move_abs(cu_term *term, int row, int col);
void cuterm_move_rel(cu_term *term, int row, int col);
ssize_t cuterm_io(cu_term *term);
ssize_t cuterm_process(cu_term *term);
ssize_t cuterm_write(cu_term *term, const char *buf, size_t len);
void cuterm_keyboard(cu_term *term, int key, int scancode, int action, int mods);
