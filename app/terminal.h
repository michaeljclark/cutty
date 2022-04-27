#pragma once

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
	cu_cell_bold             = 1 << 0,
	cu_cell_faint            = 1 << 1,
	cu_cell_italic           = 1 << 2,
	cu_cell_underline        = 1 << 3,
	cu_cell_dunderline       = 1 << 4,
	cu_cell_blink            = 1 << 5,
	cu_cell_rblink           = 1 << 6,
	cu_cell_inverse          = 1 << 7,
	cu_cell_hidden           = 1 << 8,
	cu_cell_strikeout        = 1 << 9,
	cu_cell_fraktur          = 1 << 10,
};

enum cu_cell_color
{
	cu_cell_color_fg_dfl     = 0xff000000,
	cu_cell_color_bg_dfl     = 0xffffffff,

	cu_cell_color_nr_black   = 0xff333333,
	cu_cell_color_nr_red     = 0xff000099,
	cu_cell_color_nr_green   = 0xff009900,
	cu_cell_color_nr_yellow  = 0xff00cccc,
	cu_cell_color_nr_blue    = 0xff990000,
	cu_cell_color_nr_magenta = 0xffcc00cc,
	cu_cell_color_nr_cyan    = 0xffcccc00,
	cu_cell_color_nr_white   = 0xffcccccc,

	cu_cell_color_br_black   = 0xff555555,
	cu_cell_color_br_red     = 0xff0000bb,
	cu_cell_color_br_green   = 0xff00bb00,
	cu_cell_color_br_yellow  = 0xff00eeee,
	cu_cell_color_br_blue    = 0xffbb0000,
	cu_cell_color_br_magenta = 0xffee00ee,
	cu_cell_color_br_cyan    = 0xffeeee00,
	cu_cell_color_br_white   = 0xffeeeeee,
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
	uint fg;
	uint bg;
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
	cu_term_clear_end,
	cu_term_clear_start,
	cu_term_clear_all
};

enum cu_state
{
	cu_state_normal,
	cu_state_escape,
	cu_state_utf4,
	cu_state_utf3,
	cu_state_utf2,
	cu_state_csi0,
	cu_state_csi,
	cu_state_csi_dec,
	cu_state_csi_dec2,
	cu_state_csi_dec3,
	cu_state_osc0,
	cu_state_osc,
	cu_state_osc_string,
	cu_state_charset,
};

enum cu_flag
{
	cu_flag_DECCKM          = 1 << 0,
	cu_flag_DECAWM          = 1 << 1,
	cu_flag_DECTCEM         = 1 << 2
};

enum cu_term_col
{
	cu_term_col_home = 0xffff0000
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

struct cu_winsize
{
	int vis_lines;
	int vis_rows;
	int vis_cols;
	int pix_width;
	int pix_height;

	template <typename... Args> constexpr auto tuple() {
		return std::tie(vis_lines, vis_rows, vis_cols, pix_width, pix_height);
	}
};

inline bool operator==(cu_winsize &a, cu_winsize&b) { return a.tuple() == b.tuple(); }
inline bool operator!=(cu_winsize &a, cu_winsize&b) { return a.tuple() != b.tuple(); }

struct cu_term
{
	uint state;
	uint code;
	uint flags;
	uint charset;
	uint argc;
	uint argv[5];
	uint fd;
	uchar needs_update;
	uchar needs_capture;
	std::string osc_string;

	std::vector<uchar> in_buf;
	ssize_t in_start;
	ssize_t in_end;

	std::vector<uchar> out_buf;
	ssize_t out_start;
	ssize_t out_end;

	cu_cell tmpl;
	std::vector<cu_line> lines;
	llong cur_row;
	llong cur_col;
	llong vis_rows;
	llong vis_cols;
	llong vis_lines;
	llong top_marg;
	llong bot_marg;
};

cu_term* cu_term_new();
void cu_term_close(cu_term *term);
void cu_term_set_fd(cu_term *term, int fd);
void cu_term_set_dim(cu_term *term, cu_winsize dim);
void cu_term_reset(cu_term *term);
ssize_t cu_term_io(cu_term *term);
ssize_t cu_term_process(cu_term *term);
ssize_t cu_term_write(cu_term *term, const char *buf, size_t len);
void cu_term_keyboard(cu_term *term, int key, int scancode, int action, int mods);
