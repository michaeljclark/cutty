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

struct tty_line
{
    std::vector<tty_cell> cells;
    tty_timestamp tv;
};

struct tty_cell_ref { llong row; llong col; };
struct tty_cell_span { tty_cell_ref start, end; };

static const tty_cell_ref null_cell_ref = { INT_MIN, INT_MIN };

inline bool operator< (const tty_cell_ref &a, const tty_cell_ref &b)
{ return std::tie(a.row, a.col) < std::tie(b.row, b.col); }
inline bool operator> (const tty_cell_ref &a, const tty_cell_ref &b)
{ return std::tie(a.row, a.col) > std::tie(b.row, b.col); }
inline bool operator<= (const tty_cell_ref &a, const tty_cell_ref &b)
{ return std::tie(a.row, a.col) <= std::tie(b.row, b.col); }
inline bool operator>= (const tty_cell_ref &a, const tty_cell_ref &b)
{ return std::tie(a.row, a.col) >= std::tie(b.row, b.col); }
inline bool operator== (const tty_cell_ref &a, const tty_cell_ref &b)
{ return std::tie(a.row, a.col) == std::tie(b.row, b.col); }
inline bool operator!= (const tty_cell_ref &a, const tty_cell_ref &b)
{ return std::tie(a.row, a.col) != std::tie(b.row, b.col); }

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
    tty_char_DEL = 0x7F, // ^? - Delete
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
    tty_flag_DECCKM   = (1 << 0),   // [X] DEC Cursor Key Mode
    tty_flag_DECAWM   = (1 << 1),   // [X] DEC Auto Wrap Mode
    tty_flag_DECTCEM  = (1 << 2),   // [X] DEC Text Cursor Enable Mode
    tty_flag_DECAKM   = (1 << 3),   // [ ] DEC Alternate Keypad Mode
    tty_flag_DECBKM   = (1 << 4),   // [X] DEC Backarrow Sends Delete Mode
    tty_flag_ATTBC    = (1 << 5),   // [ ] AT&T Blinking Cursor
    tty_flag_XT8BM    = (1 << 6),   // [ ] XTerm 8-Bit Mode
    tty_flag_XTAS     = (1 << 7),   // [ ] XTerm Alt Screen
    tty_flag_XTSC     = (1 << 8),   // [ ] XTerm Save Cursor
    tty_flag_XTBP     = (1 << 9),   // [X] XTerm Bracketed Paste
    tty_flag_CUTSC    = (1 << 10),  // [X] Cutty Screen Capture
};

enum tty_col
{
    tty_col_home = 0xffff0000
};

struct tty_font_metric
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
    llong vis_rows;
    llong vis_cols;
    llong pix_width;
    llong pix_height;

    template <typename... Args> constexpr auto tuple() {
        return std::tie(vis_rows, vis_cols, pix_width, pix_height);
    }
};

inline bool operator==(tty_winsize &a, tty_winsize&b) { return a.tuple() == b.tuple(); }
inline bool operator!=(tty_winsize &a, tty_winsize&b) { return a.tuple() != b.tuple(); }

struct tty_log_loc { llong lline, loff; };
struct tty_vis_loc { llong vrow, count; };

struct tty_teletype
{
    virtual ~tty_teletype() = default;

    virtual void close() = 0;
    virtual bool get_needs_update() = 0;
    virtual void set_needs_update() = 0;
    virtual bool get_needs_capture() = 0;
    virtual void set_needs_capture() = 0;
    virtual void update_offsets() = 0;
    virtual tty_log_loc visible_to_logical(llong vrow) = 0;
    virtual tty_vis_loc logical_to_visible(llong lline) = 0;
    virtual tty_line& get_line(llong lline) = 0;
    virtual void set_selection(tty_cell_span selection) = 0;
    virtual tty_cell_span get_selection() = 0;
    virtual std::string get_selected_text() = 0;
    virtual llong total_rows() = 0;
    virtual llong total_cols() = 0;
    virtual llong visible_rows() = 0;
    virtual llong visible_cols() = 0;
    virtual llong scroll_row() = 0;
    virtual llong scroll_row_limit() = 0;
    virtual llong scroll_col() = 0;
    virtual llong scroll_col_limit() = 0;
    virtual void set_scroll_row(llong row) = 0;
    virtual void set_scroll_col(llong col) = 0;
    virtual llong top_row() = 0;
    virtual llong cursor_row() = 0;
    virtual llong cursor_line() = 0;
    virtual llong cursor_offset() = 0;
    virtual bool has_flag(uint flag) = 0;
    virtual void set_flag(uint flag, bool value) = 0;
    virtual tty_winsize get_winsize() = 0;
    virtual void set_winsize(tty_winsize dim) = 0;
    virtual void set_fd(int fd) = 0;
    virtual void reset() = 0;
    virtual ssize_t io() = 0;
    virtual ssize_t proc() = 0;
    virtual ssize_t emit(const char *buf, size_t len) = 0;
    virtual bool keyboard(int key, int scancode, int action, int mods) = 0;
};

tty_teletype* tty_new();
