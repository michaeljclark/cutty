#pragma once

typedef long long llong;
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long long ullong;

enum tty_cell_flag
{
	tty_cell_bold             = 1 << 0,
	tty_cell_faint            = 1 << 1,
	tty_cell_italic           = 1 << 2,
	tty_cell_underline        = 1 << 3,
	tty_cell_dunderline       = 1 << 4,
	tty_cell_blink            = 1 << 5,
	tty_cell_rblink           = 1 << 6,
	tty_cell_inverse          = 1 << 7,
	tty_cell_hidden           = 1 << 8,
	tty_cell_strikeout        = 1 << 9,
	tty_cell_fraktur          = 1 << 10,
};

enum tty_cell_color
{
	tty_cell_color_fg_dfl     = 0xff000000,
	tty_cell_color_bg_dfl     = 0xffffffff,

	tty_cell_color_nr_black   = 0xff333333,
	tty_cell_color_nr_red     = 0xff000099,
	tty_cell_color_nr_green   = 0xff009900,
	tty_cell_color_nr_yellow  = 0xff00cccc,
	tty_cell_color_nr_blue    = 0xff990000,
	tty_cell_color_nr_magenta = 0xffcc00cc,
	tty_cell_color_nr_cyan    = 0xffcccc00,
	tty_cell_color_nr_white   = 0xffcccccc,

	tty_cell_color_br_black   = 0xff555555,
	tty_cell_color_br_red     = 0xff0000bb,
	tty_cell_color_br_green   = 0xff00bb00,
	tty_cell_color_br_yellow  = 0xff00eeee,
	tty_cell_color_br_blue    = 0xffbb0000,
	tty_cell_color_br_magenta = 0xffee00ee,
	tty_cell_color_br_cyan    = 0xffeeee00,
	tty_cell_color_br_white   = 0xffeeeeee,
};

enum tty_charset
{
	tty_charset_utf8         = 0,
	tty_charset_iso8859_1    = 1,
};

struct tty_cell
{
	uint codepoint;
	uint flags;
	uint fg;
	uint bg;
};

enum tty_line_flag
{
	tty_line_packed = (1 << 0)
};

struct tty_line
{
	size_t pcount;
	std::vector<tty_cell> cells;
	std::string utf8;

	size_t count();
	bool ispacked();
	void pack();
	void unpack();
	void clear();
};

enum tty_char
{
	tty_char_NUL = 0x00, // Null
	tty_char_SOH = 0x01, // ^A - Start Of Heading
	tty_char_STX = 0x02, // ^B - Start Of Text
	tty_char_ETX = 0x03, // ^C - End Of Text
	tty_char_EOT = 0x04, // ^D - End Of Transmission
	tty_char_ENQ = 0x05, // ^E - Enquiry
	tty_char_ACK = 0x06, // ^F - Acknowledge
	tty_char_BEL = 0x07, // ^G - Bell
	tty_char_BS  = 0x08, // ^H - Backspace
	tty_char_HT  = 0x09, // ^I - Horizontal Tab
	tty_char_LF  = 0x0A, // ^J - Line Feed
	tty_char_VT  = 0x0B, // ^K - Vertical Tab
	tty_char_FF  = 0x0C, // ^L - Form Feed
	tty_char_CR  = 0x0D, // ^M - Carriage Return
	tty_char_SO  = 0x0E, // ^N - Shift Out
	tty_char_SI  = 0x0F, // ^O - Shift In
	tty_char_DLE = 0x10, // ^P - Data Link Escape
	tty_char_DC1 = 0x11, // ^Q - Device Control 1
	tty_char_DC2 = 0x12, // ^R - Device Control 2
	tty_char_DC3 = 0x13, // ^S - Device Control 3
	tty_char_DC4 = 0x14, // ^T - Device Control 4
	tty_char_NAK = 0x15, // ^U - Negative Acknowledge
	tty_char_SYN = 0x16, // ^V - Synchronize Idle
	tty_char_ETB = 0x17, // ^W - End Of Transmission Block
	tty_char_CAN = 0x18, // ^X - Cancel
	tty_char_EM  = 0x19, // ^Y - End of Medium
	tty_char_SUB = 0x1A, // ^Z - Substitute
	tty_char_ESC = 0x1B, // ^[ - Escape
	tty_char_FS  = 0x1C, // ^\ - File Separator
	tty_char_GS  = 0x1D, // ^] - Group Separator
	tty_char_RS  = 0x1E, // ^^ - Record Separator
	tty_char_US  = 0x1F, // ^_ - Unit Separator
};

enum tty_clear
{
	tty_clear_end,
	tty_clear_start,
	tty_clear_all
};

enum tty_state
{
	tty_state_normal,
	tty_state_escape,
	tty_state_utf4,
	tty_state_utf3,
	tty_state_utf2,
	tty_state_csi0,
	tty_state_csi,
	tty_state_csi_dec,
	tty_state_csi_dec2,
	tty_state_csi_dec3,
	tty_state_osc0,
	tty_state_osc,
	tty_state_osc_string,
	tty_state_charset,
};

enum tty_flag
{
	tty_flag_DECCKM          = 1 << 0,
	tty_flag_DECAWM          = 1 << 1,
	tty_flag_DECTCEM         = 1 << 2
};

enum tty_col
{
	tty_col_home = 0xffff0000
};

struct tty_fontmetric
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

struct tty_winsize
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

inline bool operator==(tty_winsize &a, tty_winsize&b) { return a.tuple() == b.tuple(); }
inline bool operator!=(tty_winsize &a, tty_winsize&b) { return a.tuple() != b.tuple(); }

struct tty_line_voff { size_t lline, offset; };
struct tty_line_loff { size_t vline, count; };

struct tty_teletype
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

	tty_cell tmpl;
	std::vector<tty_line> lines;
	std::vector<tty_line_voff> voffsets;
	std::vector<tty_line_loff> loffsets;
	llong min_row;
	llong cur_row;
	llong cur_col;
	llong vis_rows;
	llong vis_cols;
	llong vis_lines;
	llong top_marg;
	llong bot_marg;
};

tty_teletype* tty_new();
void tty_close(tty_teletype *tty);
void tty_set_fd(tty_teletype *tty, int fd);
void tty_set_dim(tty_teletype *tty, tty_winsize dim);
void tty_update_offsets(tty_teletype *t);
void tty_reset(tty_teletype *tty);
ssize_t tty_io(tty_teletype *tty);
ssize_t tty_proc(tty_teletype *tty);
ssize_t tty_write(tty_teletype *tty, const char *buf, size_t len);
void tty_keyboard(tty_teletype *tty, int key, int scancode, int action, int mods);
