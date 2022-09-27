#undef NDEBUG
#include <cstdlib>
#include <cerrno>
#include <cassert>
#include <climits>

#include <time.h>
#include <poll.h>
#include <unistd.h>

#include "app.h"
#include "utf8.h"
#include "colors.h"

#include "timestamp.h"
#include "teletype.h"
#include "translate.h"
#include "process.h"

static int io_buffer_size = 65536;
static int io_poll_timeout = 1;
static int line_cache_size = 128;
static bool debug_io = false;

static const char* ctrl_code[32] = {
    "NUL",
    "SOH", // ^A - Start Of Heading
    "STX", // ^B - Start Of Text
    "ETX", // ^C - End Of Text
    "EOT", // ^D - End Of Transmission
    "ENQ", // ^E - Enquiry
    "ACK", // ^F - Acknowledge
    "BEL", // ^G - Bell
    "BS",  // ^H - Backspace
    "HT",  // ^I - Horizontal Tab
    "LF",  // ^J - Line Feed
    "VT",  // ^K - Vertical Tab
    "FF",  // ^L - Form Feed
    "CR",  // ^M - Carriage Return
    "SO",  // ^N - Shift Out
    "SI",  // ^O - Shift In
    "DLE", // ^P - Data Link Escape
    "DC1", // ^Q - Device Control 1
    "DC2", // ^R - Device Control 2
    "DC3", // ^S - Device Control 3
    "DC4", // ^T - Device Control 4
    "NAK", // ^U - Negative Acknowledge
    "SYN", // ^V - Synchronize Idle
    "ETB", // ^W - End Of Transmission Block
    "CAN", // ^X - Cancel
    "EM",  // ^Y - End of Medium
    "SUB", // ^Z - Substitute
    "ESC", // ^[ - Escape
    "FS",  // ^\ - File Separator
    "GS",  // ^] - Group Separator
    "RS",  // ^^ - Record Separator
    "US",  // ^_ - Unit Separator
};

static std::string control_string(std::string s)
{
    enum char_class { c_none, c_ctrl, c_ascii, c_hex };

    auto classify = [] (int c) -> char_class {
        if (c < 32) return c_ctrl;
        else if (c < 127) return c_ascii;
        else return c_hex;
    };
    auto code = [] (int c) -> std::string {
        if (c < 32) return std::string(ctrl_code[c]);
        else return std::string("?");
    };
    auto text = [] (int c) -> std::string {
        if (c == '"') return std::string("\\\"");
        else return std::string(1, c);
    };
    auto hex = [](int c) -> std::string {
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02x", c & 0xff);
        return hex;
    };

    std::string out;
    int l = 0, c;
    char_class lc = c_none, cc;

    for (size_t i = 0; i < s.size(); i++) {
        c = (unsigned char)s[i];
        cc = classify(c);
        if (cc == c_ascii && (out.size() == 0 || lc != c_ascii)) {
            out.append("\"");
        }
        if (cc != c_ascii && out.size() > 0) out.append(" ");
        switch (cc) {
        case c_none: break;
        case c_ctrl: out.append(code(c)); break;
        case c_ascii: out.append(text(c)); break;
        case c_hex: out.append(hex(c)); break;
        }
        l = c;
        lc = cc;
    }
    if (lc == c_ascii) out.append("\"");

    return out;
}

static void dump_buffer(const char *buf, size_t len, std::function<void(const char*)> emit)
{
    enum char_class { c_none, c_ctrl, c_ascii, c_hex };

    auto classify = [] (int c) -> char_class {
        if (c < 32) return c_ctrl;
        else if (c < 127) return c_ascii;
        else return c_hex;
    };
    auto code = [] (int c) -> std::string {
        if (c < 32) return std::string(ctrl_code[c]);
        else return std::string("?");
    };
    auto text = [] (int c) -> std::string {
        if (c == '"') return std::string("\\\"");
        else return std::string(1, c);
    };
    auto hex = [](int c) -> std::string {
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02x", c & 0xff);
        return hex;
    };

    std::string out;
    int l = 0, c;
    char_class lc = c_none, cc;

    for (size_t i = 0; i < len; i++) {
        c = (unsigned char)buf[i];
        cc = classify(c);
        if (((cc == c_ascii && cc == lc) && out.size() > 62) ||
            ((cc != c_ascii || cc != lc) && out.size() > 58)) {
            if (lc == c_ascii) out.append("\"");
            emit(out.c_str());
            out.clear();
        }
        if (lc == c_ascii && cc != c_ascii) {
            out.append("\"");
        }
        if (out.size() != 0 && (cc != c_ascii || lc != c_ascii)) {
            out.append(" ");
        }
        if (cc == c_ascii && (out.size() == 0 || lc != c_ascii)) {
            out.append("\"");
        }
        switch (cc) {
        case c_none: break;
        case c_ctrl: out.append(code(c)); break;
        case c_ascii: out.append(text(c)); break;
        case c_hex: out.append(hex(c)); break;
        }
        l = c;
        lc = cc;
    }
    if (out.size() > 0) {
        if (lc == c_ascii) out.append("\"");
        emit(out.c_str());
    }
}

typedef struct { ushort d[3]; } tty_int48;

static inline llong tty_int48_get(tty_int48 p)
{
    llong v = ((llong)p.d[0]) | ((llong)p.d[1] << 16) | ((llong)p.d[2] << 32);
    return v << 16 >> 16;
}

static inline tty_int48 tty_int48_set(llong v)
{
    tty_int48 x = { (ushort)v, (ushort)(v >> 16), (ushort)(v >> 32) };
    return x;
}

struct tty_private_mode_rec
{
    uint code;
    uint flag;
    const char *name;
};

struct tty_packed_line
{
    tty_int48 text_offset;
    tty_int48 cell_offset;
    tty_int48 text_count;
    tty_int48 cell_count;
    tty_timestamp tv;
};

struct tty_cached_line
{
    tty_int48 lline;
    short dirty;
    tty_line ldata;
};

struct tty_packed_log_loc { tty_int48 lline, loff; };
struct tty_packed_vis_loc { tty_int48 vrow, count; };

enum coord_type { coord_type_none, coord_type_rel, coord_type_abs };

struct tty_coord { coord_type type; llong val; };

static tty_coord coord_none() { return tty_coord { coord_type_none, 0 }; }
static tty_coord coord_rel(llong val) { return tty_coord { coord_type_rel, val }; }
static tty_coord coord_abs(llong val) { return tty_coord { coord_type_abs, val }; }

struct tty_line_store
{
    std::vector<tty_cell> cells;
    std::vector<char> text;
    std::vector<tty_packed_line> lines;
    std::vector<tty_cached_line> cache;
    std::vector<tty_packed_log_loc> voffsets;
    std::vector<tty_packed_vis_loc> loffsets;

    tty_packed_line pack(tty_line &uline);
    tty_line unpack(tty_packed_line &pline);
    tty_line& get_line(llong lline, bool edit);
    llong count_cells(tty_packed_line &pline);
    llong count_cells(llong lline);
    void clear_line(llong lline);
    void erase_line(llong lline, llong start, llong end, llong cols, tty_cell tmpl);
    void clear_all();
    void invalidate_cache();
    void dump_stats();

    tty_line_store();
};

struct tty_teletype_impl : tty_teletype
{
    uint state;
    uint flags;
    uint charset;
    uint code;
    uint argc;
    uint argv[5];
    uint fd;
    uchar needs_update;
    std::string osc_data;

    std::vector<uchar> in_buf;
    ssize_t in_start;
    ssize_t in_end;

    std::vector<uchar> out_buf;
    ssize_t out_start;
    ssize_t out_end;

    tty_timestamp tv;
    tty_cell tmpl;
    tty_line_store hist;
    tty_line empty_line;
    tty_cell_span sel;
    tty_winsize ws;
    llong cur_line;
    llong cur_offset;
    bool cur_overflow;
    llong sav_line;
    llong sav_offset;
    llong sav_overflow;
    llong sav_row;
    llong sav_col;
    llong min_line;
    llong max_cols;
    llong top_marg;
    llong bot_marg;
    llong scr_row;
    llong scr_col;

    tty_teletype_impl();
    virtual ~tty_teletype_impl() = default;

    virtual void log(logger::L level, const char *fmt, ...);

    virtual void close();
    virtual bool get_needs_update();
    virtual void set_needs_update();
    virtual void update_offsets();
    virtual tty_log_loc visible_to_logical(llong vrow);
    virtual tty_vis_loc logical_to_visible(llong lline);
    virtual tty_line& get_line(llong lline);
    virtual void set_selection(tty_cell_span sel);
    virtual tty_cell_span get_selection();
    virtual std::string get_selected_text();
    virtual llong total_rows();
    virtual llong total_cols();
    virtual llong visible_rows();
    virtual llong visible_cols();
    virtual llong top_row();
    virtual llong scroll_top();
    virtual llong scroll_bottom();
    virtual bool scroll_top_enabled();
    virtual bool scroll_bottom_enabled();
    virtual llong scroll_row();
    virtual llong scroll_row_limit();
    virtual llong scroll_col();
    virtual llong scroll_col_limit();
    virtual void set_scroll_row(llong row);
    virtual void set_scroll_col(llong col);
    virtual llong cursor_row();
    virtual llong cursor_col();
    virtual llong cursor_line();
    virtual llong cursor_offset();
    virtual bool has_flag(uint flag);
    virtual void set_flag(uint flag, bool value);
    virtual tty_winsize get_winsize();
    virtual void set_winsize(tty_winsize dim);
    virtual void set_fd(int fd);
    virtual void reset();
    virtual ssize_t io();
    virtual ssize_t proc();
    virtual ssize_t emit(const char *buf, size_t len);
    virtual void emit_loop(const char *buf, size_t len);
    virtual bool keyboard(int key, int scancode, int action, int mods);

protected:
    std::string args_str();
    int opt_arg(int arg, int opt);

    void send(uint c);
    void move(tty_coord row, tty_coord col);
    void reset_style();
    void erase_screen(uint arg);
    void erase_line(uint arg);
    void insert_lines(uint arg);
    void delete_lines(uint arg);
    void delete_chars(uint arg);

    void handle_scroll();
    void handle_scroll_region(llong line0, llong line1);
    void handle_save_cursor();
    void handle_restore_cursor();
    void handle_bell();
    void handle_backspace();
    void handle_horizontal_tab();
    void handle_line_feed();
    void handle_carriage_return();
    void handle_bare(uint c);
    void handle_control(uint c);
    void handle_charset(uint cmd, uint set);
    void handle_keypad_mode(bool set);
    void handle_window_manager();
    void handle_osc(uint c);
    void handle_osc_string(uint c);
    void handle_csi_private_mode(uint code, uint set);
    void handle_csi_dec(uint c);
    void handle_csi_dec2(uint c);
    void handle_csi_dec3(uint c);
    void handle_csi_dsr();
    void handle_csi(uint c);

    void absorb(uint c);
};

static tty_private_mode_rec dec_flags[] = {
    {    1, tty_flag_DECCKM,     "app_cursor_keys"        },
    {    7, tty_flag_DECAWM,     "auto_wrap"              },
    {   12, tty_flag_ATTBC,      "blinking_cursor"        },
    {   25, tty_flag_DECTCEM,    "cursor_enable"          },
    { 1034, tty_flag_XT8BM,      "eight_bit_mode"         },
    { 1047, tty_flag_XTAS,       "alt_screen"             },
    { 1048, tty_flag_XTSC,       "save_cursor"            },
    { 1049, tty_flag_XTAS |
            tty_flag_XTSC,       "save_cursor_alt_screen" },
    { 2004, tty_flag_XTBP,       "bracketed_paste"        },
    { 7000, tty_flag_DECBKM,     "backarrow_sends_delete" },
    { 7001, tty_flag_DECAKM,     "alt_keypad_mode"        }
};

tty_teletype_impl::tty_teletype_impl() :
    state(tty_state_normal),
    flags(tty_flag_DECAWM | tty_flag_DECTCEM | tty_flag_DECBKM),
    charset(tty_charset_utf8),
    code(0),
    argc(0),
    argv{},
    fd(-1),
    needs_update(1),
    osc_data(),
    in_buf(),
    in_start(0),
    in_end(0),
    out_buf(),
    out_start(0),
    out_end(0),
    tmpl{},
    hist(),
    empty_line{},
    sel{null_cell_ref, null_cell_ref},
    ws{0,0,0,0},
    cur_line(0),
    cur_offset(0),
    cur_overflow(0),
    sav_row(0),
    sav_col(0),
    min_line(0),
    max_cols(0),
    top_marg(0),
    bot_marg(0),
    scr_row(0),
    scr_col(0)
{
    in_buf.resize(io_buffer_size);
    out_buf.resize(io_buffer_size);
}

tty_teletype* tty_new()
{
    return new tty_teletype_impl();
}

void tty_teletype_impl::close()
{
    ::close(fd);
    fd = -1;
}

bool tty_teletype_impl::get_needs_update()
{
    bool flag = needs_update;
    needs_update = false;
    return flag;
}

void tty_teletype_impl::set_needs_update()
{
    needs_update |= true;
}

static llong trace_counter;
static std::vector<char> trace_charseq;

void tty_teletype_impl::log(logger::L level, const char *fmt, ...)
{
    char prefix[64];
    llong row = cursor_row() - top_row() + 1, col = cursor_col() + 1;
    snprintf(prefix, sizeof(prefix), "%s: [%09llu] (%-2llu,%-2llu) ",
        logger::level_names[level], trace_counter, row, col);

    if (trace_charseq.size() > 0) {
        dump_buffer(&trace_charseq[0], trace_charseq.size(),
            [&](const char* msg) { logger::log(level, "charseq: %s\n", msg); });
        trace_charseq.clear();
    }

    va_list ap;
    va_start(ap, fmt);
    logger::output(prefix, fmt, ap);
    va_end(ap);
}

#undef Trace
#define Trace(fmt, ...) \
if (logger::L::Ltrace >= logger::level) log(logger::L::Ltrace, fmt, ## __VA_ARGS__);

static std::string char_str(uint c)
{
    char buf[32];
    if (c < 32) {
        snprintf(buf, sizeof(buf), "%s (0x%02x)", ctrl_code[c], c);
    }
    else if (c == 0x7f) {
        snprintf(buf, sizeof(buf), "DEL (0x%02x)", c);
    }
    else if (c < 0x7f) {
        snprintf(buf, sizeof(buf), "'%c' (0x%02x)", c, c);
    }
    else {
        char u[8];
        utf32_to_utf8(u, sizeof(u), c);
        snprintf(buf, sizeof(buf), "\"%s\" (0x%04x)", u, c);
    }
    return buf;
}

std::string tty_teletype_impl::args_str()
{
    std::string s;
    for (size_t i = 0; i < argc; i++) {
        if (i > 0) s.append(";");
        s.append(std::to_string(argv[i]));
    }
    return s;
}

int tty_teletype_impl::opt_arg(int arg, int opt)
{
    return arg < argc ? argv[arg] : opt;
}

void tty_teletype_impl::send(uint c)
{
    Trace("send: %s\n", char_str(c).c_str());
    char b = (char)c;
    emit(&b, 1);
}

/*
 * - unpacked lines: cells vector has one element for every character and
 *   each cell has a utf32 codepoint. style flags, foreground and background
 *   colors. the cell count for the line is in cells.size().
 * - packed lines: cells vector holds style changes. the codepoint element
 *   contains an offset into utf8_data and the cell count is in pcount.
 */

tty_line_store::tty_line_store()
    : cells(), text(), lines(), cache(), voffsets(), loffsets()
{
    for (size_t i = 0; i < line_cache_size; i++) {
        cache.push_back(tty_cached_line{ tty_int48_set(-1), false, tty_line{} });
    }
    lines.push_back(tty_packed_line{});
}

tty_packed_line tty_line_store::pack(tty_line &uline)
{
    llong toff = text.size(), tcount = 0;
    llong coff = cells.size(), ccount = 0;

    tty_cell t = { (uint)-1 };
    for (llong i = 0; i < uline.cells.size(); i++) {
        char u[8];
        tty_cell s = uline.cells[i];
        llong l = utf32_to_utf8(u, sizeof(u), s.codepoint);
        llong o = text.size(), p = o - toff;
        assert(p < (1ull << 32));
        if (s.flags != t.flags || s.fg != t.fg || s.bg != t.bg) {
            t = tty_cell{(uint)p, s.flags, s.fg, s.bg};
            cells.push_back(t);
            ccount++;
        }
        text.resize(o + l);
        memcpy(&text[o], u, l);
        tcount += l;
    }

    assert(toff < (1ull << 48));
    assert(coff < (1ull << 48));
    assert(tcount < (1ull << 48));
    assert(ccount < (1ull << 48));

    return tty_packed_line {
        tty_int48_set(toff),
        tty_int48_set(coff),
        tty_int48_set(tcount),
        tty_int48_set(ccount),
        { uline.tv.vec[0], uline.tv.vec[1], uline.tv.vec[2] }
    };
}

tty_line tty_line_store::unpack(tty_packed_line &pline)
{
    tty_line uline;

    tty_cell t = { 0 };
    llong o = 0, j = tty_int48_get(pline.text_offset), l = tty_int48_get(pline.text_count);
    llong p = 0, k = tty_int48_get(pline.cell_offset), m = tty_int48_get(pline.cell_count);
    while (o < l) {
        if (p < m && cells[k + p].codepoint == o) {
            t = cells[k + p];
            p++;
        }
        utf32_code v = utf8_to_utf32_code(&text[j + o]);
        uline.cells.push_back(tty_cell{(uint)v.code, t.flags, t.fg, t.bg});
        o += v.len;
    }

    uline.tv.vec[0] = pline.tv.vec[0];
    uline.tv.vec[1] = pline.tv.vec[1];
    uline.tv.vec[2] = pline.tv.vec[2];

    return uline;
}

tty_line& tty_line_store::get_line(llong lline, bool edit)
{
    llong cl = lline & (line_cache_size - 1);
    llong olline = tty_int48_get(cache[cl].lline);

    if (olline != lline)
    {
        if (olline >= 0 && cache[cl].dirty) {
            lines[olline] = pack(cache[cl].ldata);
        }
        cache[cl].ldata = unpack(lines[lline]);
        cache[cl].lline = tty_int48_set(lline);
        cache[cl].dirty = false;
    }

    cache[cl].dirty |= edit;

    return cache[cl].ldata;
}

llong tty_line_store::count_cells(tty_packed_line &pline)
{
    llong o = 0;
    llong t = tty_int48_get(pline.text_offset);
    llong c = tty_int48_get(pline.text_count);
    while (o < c) {
        utf32_code v = utf8_to_utf32_code(&text[t + o]);
        o += v.len;
    }
    return o;
}

llong tty_line_store::count_cells(llong lline)
{
    llong cl = lline & (line_cache_size - 1);

    if (tty_int48_get(cache[cl].lline) == lline) {
        return cache[cl].ldata.cells.size();
    } else {
        return count_cells(lines[lline]);
    }
}

void tty_line_store::clear_line(llong lline)
{
    llong cl = lline & (line_cache_size - 1);

    if (tty_int48_get(cache[cl].lline) == lline && cache[cl].ldata.cells.size() > 0) {
        cache[cl].ldata.cells.clear();
        cache[cl].dirty = true;
    }

    lines[lline].text_count = tty_int48_set(0);
    lines[lline].cell_count = tty_int48_set(0);
}

void tty_line_store::erase_line(llong lline, llong start, llong end, llong cols, tty_cell tmpl)
{
    /* erase line from start to end where end is not the right hand column */
    if (end < count_cells(lline) && (end % cols) != 0)
    {
        tty_line &line = get_line(lline, true);
        tty_cell cell = tmpl;
        cell.codepoint = ' ';
        for (size_t col = start; col < end; col++) {
            line.cells[col] = cell;
        }
    }
    /* erase line from start to end where end is the right hand column
     * and text beyond end needs to be split onto another line */
    else if (end < count_cells(lline) && (end % cols) == 0)
    {
        bool blank_line = start != 0 && start % cols == 0;
        invalidate_cache();
        lines.insert(lines.begin() + lline + 1, tty_packed_line{});
        if (blank_line) {
            lines.insert(lines.begin() + lline + 1, tty_packed_line{});
        }
        tty_line &curr_line = get_line(lline, true);
        tty_line &next_line = get_line(lline + 1 + blank_line, true);
        size_t copy_start = size_t(end);
        size_t copy_end = curr_line.cells.size();
        size_t copy_count = copy_end - copy_start;
        next_line.cells.resize(copy_count);
        for (size_t i = 0; i < copy_count; i++) {
            next_line.cells[i] = curr_line.cells[copy_start + i];
        }
        curr_line.cells.resize(start);
    }
    /* erase line from start to end where end is the right hand column */
    else if (start < count_cells(lline) && (end % cols) == 0)
    {
        tty_line &line = get_line(lline, true);
        line.cells.resize(start);
    }
}

void tty_line_store::clear_all()
{
    cache.clear();
    for (size_t i = 0; i < line_cache_size; i++) {
        cache.push_back(tty_cached_line{ tty_int48_set(-1), false, tty_line{} });
    }
    lines.clear();
    lines.push_back(tty_packed_line{});
}

void tty_line_store::invalidate_cache()
{
    for (llong cl = 0; cl < line_cache_size; cl++) {
        llong olline = tty_int48_get(cache[cl].lline);
        if (olline >= 0 && cache[cl].dirty) {
            lines[olline] = pack(cache[cl].ldata);
            cache[cl].dirty = false;
        }
        cache[cl].lline = tty_int48_set(-1);
    }
}

void tty_line_store::dump_stats()
{
    size_t cache_cells = 0;
    for (size_t i = 0; i < line_cache_size; i++) {
        cache_cells += cache[i].ldata.cells.size();
    }
    Info("=] stats [=============================================\n");
    Info("tty_line_store.cache.lines = %9zu x %2zu (%9zu)\n",
        cache.size(), sizeof(tty_cached_line),
        cache.size() * sizeof(tty_cached_line));
    Info("tty_line_store.cache.cells = %9zu x %2zu (%9zu)\n",
        cache_cells, sizeof(tty_cell),
        cache_cells * sizeof(tty_cell));
    Info("tty_line_store.voffsets    = %9zu x %2zu (%9zu)\n",
        voffsets.size(), sizeof(tty_packed_log_loc),
        voffsets.size() * sizeof(tty_packed_log_loc));
    Info("tty_line_store.loffsets    = %9zu x %2zu (%9zu)\n",
        loffsets.size(), sizeof(tty_packed_vis_loc),
        loffsets.size() * sizeof(tty_packed_vis_loc));
    Info("tty_line_store.pack.lines  = %9zu x %2zu (%9zu)\n",
        lines.size(), sizeof(tty_packed_line),
        lines.size() * sizeof(tty_packed_line));
    Info("tty_line_store.pack.cells  = %9zu x %2zu (%9zu)\n",
        cells.size(), sizeof(tty_cell),
        cells.size() * sizeof(tty_cell));
    Info("tty_line_store.pack.text   = %9zu x %2zu (%9zu)\n",
        text.size(), sizeof(char), text.size() * sizeof(char));
    size_t total = cache.size() * sizeof(tty_cached_line)
                 + cache_cells * sizeof(tty_cell)
                 + voffsets.size() * sizeof(tty_packed_log_loc)
                 + loffsets.size() * sizeof(tty_packed_vis_loc)
                 + lines.size() * sizeof(tty_packed_line)
                 + cells.size() * sizeof(tty_cell)
                 + text.size() * sizeof(char);
    Info("-------------------------------------------------------\n");
    Info("tty_line_store.total       = %14s (%9zu)\n", "", total);
}

void tty_teletype_impl::update_offsets()
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;
    size_t cols = ws.vis_cols;
    llong vlstart, vl;

    if (!wrap_enabled) {
        hist.voffsets.clear();
        hist.loffsets.clear();

        max_cols = 0;
        for (llong k = 0; k < hist.lines.size(); k++) {
            max_cols = std::max(max_cols, hist.count_cells(k));
        }
    }

    /* recompute line offsets incrementally from min_line */
    if (min_line == 0) {
        vlstart = 0;
    } else {
        auto &loff = hist.loffsets[min_line - 1];
        vlstart = tty_int48_get(loff.vrow) + tty_int48_get(loff.count);
    }

    /* count lines with wrap incrementally from min row */
    vl = vlstart;
    for (llong k = min_line; k < hist.lines.size(); k++) {
        llong cell_count = hist.count_cells(k);
        llong wrap_count = cell_count == 0 ? 1
            : cols == 0 ? 1 : wrap_enabled ? (cell_count + cols - 1) / cols : 1;
        vl += wrap_count;
    }

    /* write out indices incrementally from min row */
    hist.voffsets.resize(vl);
    hist.loffsets.resize(hist.lines.size());
    vl = vlstart;
    for (llong k = min_line; k < hist.lines.size(); k++) {
        llong cell_count = hist.count_cells(k);
        llong wrap_count = cell_count == 0 ? 1
            : cols == 0 ? 1 : wrap_enabled ? (cell_count + cols - 1) / cols : 1;
        hist.loffsets[k] = { tty_int48_set(vl), tty_int48_set(wrap_count) };
        for (llong j = 0; j < wrap_count; j++, vl++) {
            hist.voffsets[vl] = { tty_int48_set(k), tty_int48_set(j * cols) };
        }
    }

    /* set min_line to cur_line */
    min_line = cur_line;
}

tty_log_loc tty_teletype_impl::visible_to_logical(llong vrow)
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;

    if (wrap_enabled) {
        if (vrow < 0) {
            return tty_log_loc{ -1, 0 };
        }
        else if (vrow < (llong)hist.voffsets.size()) {
            return tty_log_loc{
                tty_int48_get(hist.voffsets[vrow].lline),
                tty_int48_get(hist.voffsets[vrow].loff)
            };
        }
        else {
            llong size = (llong)hist.loffsets.size();
            llong delta = vrow - (llong)hist.voffsets.size();
            return tty_log_loc{ size + delta, 0 };
        }
    } else {
        return tty_log_loc{ vrow, 0 };
    }
}

tty_vis_loc tty_teletype_impl::logical_to_visible(llong lline)
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;

    if (wrap_enabled) {
        if (lline < 0) {
            return tty_vis_loc{ -1, 0 };
        }
        if (lline < hist.loffsets.size()) {
            return tty_vis_loc{
                tty_int48_get(hist.loffsets[lline].vrow),
                tty_int48_get(hist.loffsets[lline].count)
            };
        } else {
            llong size = (llong)hist.voffsets.size();
            llong delta = lline - (llong)hist.loffsets.size();
            return tty_vis_loc{ size + delta, 0 };
        }
    } else {
        return tty_vis_loc{ lline, 0 };
    }
}

tty_line& tty_teletype_impl::get_line(llong lline)
{
    if (lline >= 0 && lline < hist.lines.size()) {
        return hist.get_line(lline, false);
    } else {
        return empty_line;
    }
}

void tty_teletype_impl::set_selection(tty_cell_span selection)
{
    sel = selection;
}

tty_cell_span tty_teletype_impl::get_selection()
{
    return sel;
}

std::string tty_teletype_impl::get_selected_text()
{
    tty_cell_span span = sel;
    std::string text;

    if (span.start == null_cell_ref && span.end == null_cell_ref) {
        goto out;
    }

    if (span.start > span.end) {
        std::swap(span.start, span.end);
    }

    for (llong lline = span.start.row; lline <= span.end.row; lline++) {
        tty_line line = get_line(lline);
        llong count = (llong)line.cells.size();
        llong s = std::max(0ll, lline == span.start.row ? span.start.col : 0ll);
        llong e = std::min(count-1ll, lline == span.end.row ? span.end.col : count-1ll);
        for (; s <= e; s++) {
            char u[8];
            llong o = text.size();
            llong l = utf32_to_utf8(u, sizeof(u), line.cells[s].codepoint);
            text.append(u);
        }
        if (s == count && lline != span.end.row) text.append("\n");
    }

out:
    return text;
}

llong tty_teletype_impl::total_rows()
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;

    return wrap_enabled ? hist.voffsets.size() : hist.lines.size();
}

llong tty_teletype_impl::total_cols()
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;

    return wrap_enabled ? ws.vis_cols : std::max(ws.vis_cols, max_cols);
}

llong tty_teletype_impl::visible_rows()
{
    return ws.vis_rows;
}

llong tty_teletype_impl::visible_cols()
{
    return ws.vis_cols;
}

llong tty_teletype_impl::scroll_top()
{
    return top_marg == 0 ? 1
        : std::max(1ll, std::min(top_marg, ws.vis_rows));
}

llong tty_teletype_impl::scroll_bottom()
{
    return bot_marg == 0 ? ws.vis_rows
        : std::max(1ll, std::min(bot_marg, ws.vis_rows));
}

bool tty_teletype_impl::scroll_top_enabled()
{
    return scroll_top() != 1;
}

bool tty_teletype_impl::scroll_bottom_enabled()
{
    return scroll_bottom() != ws.vis_rows;
}

llong tty_teletype_impl::scroll_row()
{
    return scr_row;
}

llong tty_teletype_impl::scroll_row_limit()
{
    return std::max(0ll, total_rows() - visible_rows());
}

llong tty_teletype_impl::scroll_col()
{
    return scr_col;
}

llong tty_teletype_impl::scroll_col_limit()
{
    return std::max(0ll, total_cols() - visible_cols());
}

void tty_teletype_impl::set_scroll_row(llong row)
{
    llong new_scroll_row = std::max(std::min(row, scroll_row_limit()), 0ll);
    if (scr_row != new_scroll_row) {
        scr_row = new_scroll_row;
        needs_update = 1;
    }
}

void tty_teletype_impl::set_scroll_col(llong col)
{
    llong new_scroll_col = std::max(std::min(col, scroll_col_limit()), 0ll);
    if (scr_col != new_scroll_col) {
        scr_col = new_scroll_col;
        needs_update = 1;
    }
}

llong tty_teletype_impl::top_row()
{
    return (llong)std::max(size_t(ws.vis_rows), hist.voffsets.size()) - ws.vis_rows;
}

llong tty_teletype_impl::cursor_row()
{
    tty_vis_loc vloc = logical_to_visible(cur_line);
    return vloc.vrow + std::min(vloc.count,
        cur_offset / ws.vis_cols - cur_overflow);
}

llong tty_teletype_impl::cursor_col()
{
    return cur_offset % ws.vis_cols;
}

llong tty_teletype_impl::cursor_line()
{
    return cur_line;
}

llong tty_teletype_impl::cursor_offset()
{
    return cur_offset;
}

bool tty_teletype_impl::has_flag(uint flag)
{
    return (flags & flag) == flag;
}

void tty_teletype_impl::set_flag(uint flag, bool value)
{
    if (value) {
        flags |= flag;
    } else {
        flags &= ~flag;
    }
}

tty_winsize tty_teletype_impl::get_winsize()
{
    return ws;
}

void tty_teletype_impl::set_winsize(tty_winsize d)
{
    if (ws != d) {
        ws = d;
        min_line = 0;
    }
}

static const char* coord_type(tty_coord c)
{
    switch (c.type) {
    case coord_type_abs: return "abs";
    case coord_type_rel: return "rel";
    case coord_type_none: default: break;
    }
    return "none";
}

void tty_teletype_impl::move(tty_coord row, tty_coord col)
{
    //Trace("move: %s(%lld) %s(%lld)\n", coord_type(row), row.val, coord_type(col), col.val);

    llong old_line = cur_line;
    llong old_offset = cur_offset;
    llong old_overflow = cur_overflow;

    llong new_line = old_line;
    llong new_offset = old_offset;
    llong new_overflow = old_overflow;

    tty_vis_loc vloc;
    tty_log_loc lloc;

    llong trow;
    llong tcol;

    update_offsets();

    /*
     * handle scrolling at the bottom of the scroll region
     */
    if (scroll_bottom_enabled() &&
        scroll_bottom() == cursor_row() - top_row() + 1 &&
        row.type == coord_type_rel && row.val == 1)
    {
        /* save cursor position */
        trow = cursor_row();
        tcol = cursor_col();

        /* as scrolling invalidates the cursor position */
        hist.invalidate_cache();
        if (scroll_top_enabled()) {
            tty_log_loc tloc = visible_to_logical(top_row() + scroll_top() - 1);
            tty_log_loc bloc = visible_to_logical(top_row() + scroll_bottom() - 1);
            hist.lines.insert(hist.lines.begin() + bloc.lline + 1, tty_packed_line{});
            hist.lines.erase(hist.lines.begin() + tloc.lline);
            min_line = std::min(min_line, tloc.lline);
        } else {
            tty_log_loc bloc = visible_to_logical(top_row() + scroll_bottom() - 1);
            hist.lines.insert(hist.lines.begin() + bloc.lline + 1, tty_packed_line{});
        }
        update_offsets();

        /* restore cursor position */
        tty_log_loc lloc = visible_to_logical(trow);
        cur_line = lloc.lline;
        cur_offset = lloc.loff + cursor_col();
        min_line = std::min(min_line, cur_line);
    }

    switch (row.type) {
    case coord_type_none:
        break;
    case coord_type_rel:
        new_overflow = false;
        vloc = logical_to_visible(new_line);
        trow = std::max(0ll, vloc.vrow + new_offset / ws.vis_cols + row.val);
        tcol = std::max(0ll, new_offset % ws.vis_cols);
        lloc = visible_to_logical(trow);
        new_line = lloc.lline;
        new_offset = lloc.loff + tcol;
        break;
    case coord_type_abs:
        new_overflow = false;
        trow = std::min(ws.vis_rows, std::max(1ll, row.val)) - 1ll;
        tcol = std::max(0ll, new_offset % ws.vis_cols);
        lloc = visible_to_logical(top_row() + trow);
        new_line = lloc.lline;
        new_offset = lloc.loff + tcol;
        break;
    }

    switch (col.type) {
    case coord_type_none:
        break;
    case coord_type_rel:
        vloc = logical_to_visible(new_line);
        tcol = std::max(0ll, (new_offset + col.val) % ws.vis_cols);
        trow = std::max(0ll, vloc.vrow + new_offset / ws.vis_cols);
        lloc = visible_to_logical(trow);
        new_line = lloc.lline;
        new_offset = lloc.loff + tcol;
        break;
    case coord_type_abs:
        vloc = logical_to_visible(new_line);
        tcol = std::min(ws.vis_cols, std::max(1ll, col.val)) - 1ll;
        trow = std::max(0ll, vloc.vrow + new_offset / ws.vis_cols - new_overflow);
        lloc = visible_to_logical(trow);
        new_line = lloc.lline;
        new_offset = lloc.loff + tcol;
        break;
    }

    new_overflow = false;

    cur_line = new_line;
    cur_offset = new_offset;
    cur_overflow = new_overflow;

    if (cur_line >= hist.lines.size()) {
        hist.lines.resize(cur_line + 1);
    }
    min_line = std::min(min_line, cur_line);

    Trace("move: %s(%lld) %s(%lld) "
        "# cursor (%lld,%lld%s) -> (%lld,%lld%s) "
        "# winsize (%lld,%lld)\n",
        coord_type(row), row.val, coord_type(col), col.val,
        old_line, old_offset, old_overflow ? "+" : "",
        new_line, new_offset, new_overflow ? "+" : "",
        ws.vis_rows, ws.vis_cols);
}

void tty_teletype_impl::reset_style()
{
    tmpl.flags = 0;
    tmpl.fg = tty_cell_color_fg_dfl;
    tmpl.bg = tty_cell_color_bg_dfl;
}

void tty_teletype_impl::set_fd(int fd)
{
    this->fd = fd;
}

void tty_teletype_impl::reset()
{
    Trace("reset\n");
    move(coord_abs(1), coord_abs(1));
    reset_style();
}

void tty_teletype_impl::erase_screen(uint arg)
{
    Trace("erase_screen: %d\n", arg);

    llong start, end;

    switch (arg) {
    case tty_clear_end:
        start = cursor_row();
        end = total_rows();
        break;
    case tty_clear_start:
        start = top_row();
        end = cursor_row();
        break;
    case tty_clear_all:
        start = top_row();
        end = total_rows();
        break;
    }

    for (ssize_t row = start; row < end; row++)
    {
        tty_log_loc lloc = visible_to_logical(row);
        hist.erase_line(lloc.lline, lloc.loff, lloc.loff + ws.vis_cols,
            ws.vis_cols, tmpl);
    }
}

void tty_teletype_impl::erase_line(uint arg)
{
    Trace("erase_line: %d\n", arg);

    llong row = cursor_row(), col = cursor_col();

    if (cur_overflow) return;

    auto round_offset = [&](llong offset, llong addend) -> llong {
        return ((offset + addend) / ws.vis_cols) * ws.vis_cols;
    };

    switch (arg) {
    case tty_clear_end:
        hist.erase_line(cur_line, cur_offset,
            round_offset(cur_offset, ws.vis_cols), ws.vis_cols, tmpl);
        break;
    case tty_clear_start:
        hist.erase_line(cur_line, round_offset(cur_offset, 0),
            cur_offset, ws.vis_cols, tmpl);
        break;
    case tty_clear_all:
        hist.erase_line(cur_line, round_offset(cur_offset, 0),
            round_offset(cur_offset, ws.vis_cols), ws.vis_cols, tmpl);
        break;
    }

    update_offsets();

    tty_log_loc lloc = visible_to_logical(row);
    cur_line = lloc.lline;
    cur_offset = lloc.loff + col;
}

void tty_teletype_impl::insert_lines(uint arg)
{
    Trace("insert_lines: %d\n", arg);
    if (arg == 0) return;
    // todo: consider line editing mode: *following*, or preceding
    // todo: handle case where lines are wrapping (parial insert and erase)
    tty_log_loc tloc = visible_to_logical(top_row() + scroll_top() - 1);
    tty_log_loc bloc = visible_to_logical(top_row() + scroll_bottom() - 1);
    if (cur_line < tloc.lline || cur_line > bloc.lline) return;
    hist.invalidate_cache();
    for (uint i = 0; i < arg; i++) {
        hist.lines.erase(hist.lines.begin() + bloc.lline);
        hist.lines.insert(hist.lines.begin() + cur_line, tty_packed_line{});
    }
    cur_offset = 0;
}

void tty_teletype_impl::delete_lines(uint arg)
{
    Trace("delete_lines: %d\n", arg);
    if (arg == 0) return;
    // todo: consider line editing mode: *following*, or preceding
    // todo: handle case where lines are wrapping (parial insert and erase)
    tty_log_loc tloc = visible_to_logical(top_row() + scroll_top() - 1);
    tty_log_loc bloc = visible_to_logical(top_row() + scroll_bottom() - 1);
    if (cur_line < tloc.lline || cur_line > bloc.lline) return;
    hist.invalidate_cache();
    for (uint i = 0; i < arg; i++) {
        if (cur_line < hist.lines.size()) {
            hist.lines.erase(hist.lines.begin() + cur_line);
            hist.lines.insert(hist.lines.begin() + bloc.lline, tty_packed_line{});
        }
    }
    cur_offset = 0;
}

void tty_teletype_impl::delete_chars(uint arg)
{
    Trace("delete_chars: %d\n", arg);
    for (size_t i = 0; i < arg; i++) {
        if (cur_offset < hist.count_cells(cur_line)) {
            tty_line &line = hist.get_line(cur_line, true);
            line.cells.erase(
                line.cells.begin() + cur_offset
            );
        }
    }
}

void tty_teletype_impl::handle_scroll()
{
    Trace("handle_scroll\n");
    if (cursor_row() == top_row())
    {
        llong row = cursor_row(), col = cursor_col();
        tty_log_loc bloc = visible_to_logical(top_row() + scroll_bottom() - 1);
        hist.invalidate_cache();
        hist.lines.erase(hist.lines.begin() + bloc.lline);
        update_offsets();
        tty_log_loc lloc = visible_to_logical(row);
        cur_line = lloc.lline;
        cur_offset = lloc.loff + col;
    }
    move(coord_rel(-1), coord_none());
}

void tty_teletype_impl::handle_scroll_region(llong line0, llong line1)
{
    Trace("handle_scroll_region: %lld %lld\n", line0, line1);
    top_marg = line0;
    bot_marg = line1;
}

void tty_teletype_impl::handle_save_cursor()
{
    Trace("handle_save_cursor\n");
    sav_row = cursor_row();
    sav_col = cursor_col();
}

void tty_teletype_impl::handle_restore_cursor()
{
    Trace("handle_restore_cursor\n");
    tty_log_loc lloc = visible_to_logical(sav_row);
    cur_line = lloc.lline;
    cur_offset = lloc.loff + sav_col;
    min_line = std::min(min_line, cur_line);
}

void tty_teletype_impl::handle_bell()
{
    Trace("handle_bell: unimplemented\n");
}

void tty_teletype_impl::handle_backspace()
{
    Trace("handle_backspace\n");
    move(coord_none(), coord_rel(-1));
}

void tty_teletype_impl::handle_horizontal_tab()
{
    Trace("handle_horizontal_tab\n");
    move(coord_none(), coord_rel(8 - (cur_offset % 8)));
}

void tty_teletype_impl::handle_line_feed()
{
    Trace("handle_line_feed\n");
    move(coord_rel(1), coord_none());
}

void tty_teletype_impl::handle_carriage_return()
{
    Trace("handle_carriage_return\n");
    move(coord_none(), coord_abs(1));
}

void tty_teletype_impl::handle_bare(uint c)
{
    /* join with next line if we wrap */
    if (cur_offset >= ws.vis_cols &&
        cur_offset % ws.vis_cols == 0 &&
        hist.count_cells(cur_line) % ws.vis_cols == 0 &&
        cur_line < hist.lines.size() - 1)
    {
        tty_line &curr_line = hist.get_line(cur_line, true);
        tty_line &next_line = hist.get_line(cur_line + 1, true);
        curr_line.cells.resize(cur_offset + next_line.cells.size());
        for (size_t i = 0; i < next_line.cells.size(); i++) {
            curr_line.cells[cur_offset + i] = next_line.cells[i];
        }
        hist.invalidate_cache();
        hist.lines.erase(hist.lines.begin() + cur_line + 1);
        update_offsets();
    }

    tty_line &line = hist.get_line(cur_line, true);

    if (cur_offset >= line.cells.size()) {
        line.cells.resize(cur_offset + 1);
    }
    line.cells[cur_offset++] =
        tty_cell{c, tmpl.flags, tmpl.fg, tmpl.bg};

    cur_overflow = cur_offset % ws.vis_cols == 0;
}

void tty_teletype_impl::handle_control(uint c)
{
    switch (c) {
    case tty_char_BEL: handle_bell(); break;
    case tty_char_BS: handle_backspace(); break;
    case tty_char_HT: handle_horizontal_tab(); break;
    case tty_char_LF: handle_line_feed(); break;
    case tty_char_CR: handle_carriage_return(); break;
    default:
        Trace("handle_control: unhandled control character %s\n",
            char_str(c).c_str());
    }
}

void tty_teletype_impl::handle_charset(uint cmd, uint set)
{
    Trace("handle_charset: %c %c unimplemented\n", cmd, set);
}

void tty_teletype_impl::handle_keypad_mode(bool set)
{
    Trace("handle_keypad_mode: unimplemented");
}

void tty_teletype_impl::handle_window_manager()
{
    Trace("handle_window_manager: %s unimplemented\n", args_str().c_str());
}

void tty_teletype_impl::handle_osc(uint c)
{
    Trace("handle_osc: %s %s\n",
        args_str().c_str(), char_str(c).c_str());
    for (size_t i = 0; i < argc; i++) {
        switch (opt_arg(i, 0)) {
        case 555:
            Trace("handle_osc: screen_capture\n");
            set_flag(tty_flag_CUTSC, true);
            break;
        case 556:
            Trace("handle_osc: dump_stats\n");
            hist.dump_stats();
            break;
        default:
            Trace("handle_osc: %d unimplemented\n", opt_arg(i, 0));
        }
    }
}

void tty_teletype_impl::handle_osc_string(uint c)
{
    Trace("handle_osc_string: %s %s \"%s\" unimplemented\n",
        args_str().c_str(), char_str(c).c_str(), osc_data.c_str());
}

static tty_private_mode_rec* tty_lookup_private_mode_rec(uint code)
{
    for (size_t i = 0; i < array_size(dec_flags); i++) {
        if (dec_flags[i].code == code) return dec_flags + i;
    }
    return NULL;
}

void tty_teletype_impl::handle_csi_private_mode(uint code, uint set)
{
    tty_private_mode_rec *rec = tty_lookup_private_mode_rec(code);
    if (rec == NULL) {
        Trace("handle_csi_private_mode: flag %d: unknown = %s\n",
            code, set ? "enabled" : "disabled");
    } else {
        Trace("handle_csi_private_mode: flag %d: %s = %s\n",
            code, rec->name, set ? "enabled" : "disabled");
        if (set) {
            flags |= rec->flag;
        } else {
            flags &= ~rec->flag;
        }
    }
}

void tty_teletype_impl::handle_csi_dec(uint c)
{
    switch (c) {
    case 'l':
        for (size_t i = 0; i < argc; i++) {
            handle_csi_private_mode(opt_arg(i, 0), 0);
        }
        break;
    case 'h':
        for (size_t i = 0; i < argc; i++) {
            handle_csi_private_mode(opt_arg(i, 0), 1);
        }
        break;
    default:
        Trace("handle_csi_dec: %s %s unimplemented\n",
            char_str(c).c_str(), args_str().c_str());
        break;
    }
}

void tty_teletype_impl::handle_csi_dec2(uint c)
{
    Trace("handle_csi_dec2: %s %s unimplemented\n",
        char_str(c).c_str(), args_str().c_str());
}

void tty_teletype_impl::handle_csi_dec3(uint c)
{
    Trace("handle_csi_dec3: %s %s unimplemented\n",
        char_str(c).c_str(), args_str().c_str());
}

void tty_teletype_impl::handle_csi_dsr()
{
    Trace("handle_csi_dsr: %s\n", args_str().c_str());
    switch (opt_arg(0, 0)) {
    case 6: { /* report cursor position */
        char buf[32];
        update_offsets();
        llong col = (cur_offset % ws.vis_cols) + 1;
        llong row = (cursor_row() - top_row()) + 1;
        row = std::max(1ll, std::min(row, (llong)ws.vis_rows));
        col = std::max(1ll, std::min(col, (llong)ws.vis_cols));
        int len = snprintf(buf, sizeof(buf), "\x1b[%llu;%lluR", row, col);
        emit(buf, len);
        break;
    }
    default:
        Trace("handle_csi_dsr: %s\n", args_str().c_str());
        break;
    }
}

void tty_teletype_impl::handle_csi(uint c)
{
    Trace("handle_csi: %s %s\n",
        args_str().c_str(), char_str(c).c_str());

    switch (c) {
    case '@': /* insert blanks */
    {
        tty_line &line = hist.get_line(cur_line, true);
        int n = opt_arg(0, 0);
        if (cur_offset < line.cells.size()) {
            tty_cell cell = tmpl;
            cell.codepoint = ' ';
            for (size_t i = 0; i < n; i++) {
                line.cells.insert(line.cells.begin() + cur_offset, cell);
            }
        }
        break;
    }
    case 'A': /* move up */
        move(coord_rel(-opt_arg(0, 1)), coord_none());
        break;
    case 'B': /* move down */
        move(coord_rel(opt_arg(0, 1)),  coord_none());
        break;
    case 'C': /* move right */
        move(coord_none(),  coord_rel(opt_arg(0, 1)));
        break;
    case 'D': /* move left */
        move(coord_none(), coord_rel(-opt_arg(0, 1)));
        break;
    case 'E': /* move next line */
        move(coord_rel(opt_arg(0, 1)), coord_abs(1));
        break;
    case 'F': /* move prev line */
        move(coord_rel(-opt_arg(0, 1)), coord_abs(1));
        break;
    case 'G': /* move to {col} */
        move(coord_none(), coord_abs(opt_arg(0, 1)));
        break;
    case 'H': /* move to {line};{col} */
        move(coord_abs(opt_arg(0, 1)), coord_abs(opt_arg(1, 1)));
        break;
    case 'J': /* erase lines {0=to-end,1=from-start,2=all} */
        switch (opt_arg(0, 0)) {
        case 0: erase_screen(tty_clear_end); break;
        case 1: erase_screen(tty_clear_start); break;
        case 2: erase_screen(tty_clear_all); break;
        default:
            Trace("handle_csi: CSI J: invalid arg: %d\n", opt_arg(0, 0));
            break;
        }
        break;
    case 'K': /* erase chars {0=to-end,1=from-start,2=all} */
        switch (opt_arg(0, 0)) {
        case 0: erase_line(tty_clear_end); break;
        case 1: erase_line(tty_clear_start); break;
        case 2: erase_line(tty_clear_all); break;
        default:
            Trace("handle_csi: CSI K: invalid arg: %d\n", opt_arg(0, 0));
            break;
        }
        break;
    case 'L': /* insert lines */
        insert_lines(opt_arg(0, 1));
        break;
    case 'M': /* delete lines */
        delete_lines(opt_arg(0, 1));
        break;
    case 'P': /* delete characters */
        delete_chars(opt_arg(0, 1));
        break;
    case 'd': /* move to {line};1 absolute */
        move(coord_abs(opt_arg(0, 1)), coord_none());
        break;
    case 'e': /* move to {line};1 relative */
        move(coord_rel(opt_arg(0, 1)), coord_none());
        break;
    case 'f': /* move to {line};{col} absolute */
        move(coord_abs(opt_arg(0, 1)), coord_abs(opt_arg(1, 1)));
        break;
    case 'm': /* color and formatting */
        if (argc == 0) {
            reset_style();
            break;
        }
        for (size_t i = 0; i < argc; i++) {
            uint code = argv[i];
            switch (code) {
            case 0: reset_style(); break;
            case 1: tmpl.flags |= tty_cell_bold; break;
            case 2: tmpl.flags |= tty_cell_faint; break;
            case 3: tmpl.flags |= tty_cell_italic; break;
            case 4: tmpl.flags |= tty_cell_underline; break;
            case 5: tmpl.flags |= tty_cell_blink; break;
            case 6: tmpl.flags |= tty_cell_rblink; break;
            case 7: tmpl.flags |= tty_cell_inverse; break;
            case 8: tmpl.flags |= tty_cell_hidden; break;
            case 9: tmpl.flags |= tty_cell_strikeout; break;
            /* case 10 through 19 custom fonts */
            case 20: tmpl.flags |= tty_cell_fraktur; break;
            case 21: tmpl.flags |= tty_cell_dunderline; break;
            case 22: tmpl.flags &= ~(tty_cell_bold | tty_cell_faint); break;
            case 23: tmpl.flags &= ~(tty_cell_italic | tty_cell_fraktur); break;
            case 24: tmpl.flags &= ~(tty_cell_underline | tty_cell_dunderline); break;
            case 25: tmpl.flags &= ~tty_cell_blink; break;
            case 26: tmpl.flags &= ~tty_cell_rblink; break;
            case 27: tmpl.flags &= ~tty_cell_inverse; break;
            case 28: tmpl.flags &= ~tty_cell_hidden; break;
            case 29: tmpl.flags &= ~tty_cell_strikeout; break;
            case 30: tmpl.fg = tty_cell_color_nr_black; break;
            case 31: tmpl.fg = tty_cell_color_nr_red; break;
            case 32: tmpl.fg = tty_cell_color_nr_green; break;
            case 33: tmpl.fg = tty_cell_color_nr_yellow; break;
            case 34: tmpl.fg = tty_cell_color_nr_blue; break;
            case 35: tmpl.fg = tty_cell_color_nr_magenta; break;
            case 36: tmpl.fg = tty_cell_color_nr_cyan; break;
            case 37: tmpl.fg = tty_cell_color_nr_white; break;
            case 38:
                if (i + 2 < argc && argv[i+1] == 5) {
                    tmpl.fg = tty_colors_256[argv[i+2]];
                    i += 2;
                }
                else if (i + 4 < argc && argv[i+1] == 2) {
                    uint r = tty_colors_256[argv[i+2]];
                    uint g = tty_colors_256[argv[i+3]];
                    uint b = tty_colors_256[argv[i+4]];
                    tmpl.fg = (((uint)r << 0) | ((uint)g << 8) |
                                  ((uint)b << 16) | ((uint)0xff << 24));
                    i += 4;
                }
                break;
            case 39: tmpl.fg = tty_cell_color_fg_dfl; break;
            case 40: tmpl.bg = tty_cell_color_nr_black; break;
            case 41: tmpl.bg = tty_cell_color_nr_red; break;
            case 42: tmpl.bg = tty_cell_color_nr_green; break;
            case 43: tmpl.bg = tty_cell_color_nr_yellow; break;
            case 44: tmpl.bg = tty_cell_color_nr_blue; break;
            case 45: tmpl.bg = tty_cell_color_nr_magenta; break;
            case 46: tmpl.bg = tty_cell_color_nr_cyan; break;
            case 47: tmpl.bg = tty_cell_color_nr_white; break;
            case 48:
                if (i + 2 < argc && argv[i+1] == 5) {
                    tmpl.bg = tty_colors_256[argv[i+2]];
                    i += 2;
                }
                else if (i + 4 < argc && argv[i+1] == 2) {
                    uint r = tty_colors_256[argv[i+2]];
                    uint g = tty_colors_256[argv[i+3]];
                    uint b = tty_colors_256[argv[i+4]];
                    tmpl.bg = (((uint)r << 0) | ((uint)g << 8) |
                                  ((uint)b << 16) | ((uint)0xff << 24));
                    i += 4;
                }
                break;
            case 49: tmpl.bg = tty_cell_color_bg_dfl; break;
            case 90: tmpl.fg = tty_cell_color_br_black; break;
            case 91: tmpl.fg = tty_cell_color_br_red; break;
            case 92: tmpl.fg = tty_cell_color_br_green; break;
            case 93: tmpl.fg = tty_cell_color_br_yellow; break;
            case 94: tmpl.fg = tty_cell_color_br_blue; break;
            case 95: tmpl.fg = tty_cell_color_br_magenta; break;
            case 96: tmpl.fg = tty_cell_color_br_cyan; break;
            case 97: tmpl.fg = tty_cell_color_br_white; break;
            case 100: tmpl.bg = tty_cell_color_br_black; break;
            case 101: tmpl.bg = tty_cell_color_br_red; break;
            case 102: tmpl.bg = tty_cell_color_br_green; break;
            case 103: tmpl.bg = tty_cell_color_br_yellow; break;
            case 104: tmpl.bg = tty_cell_color_br_blue; break;
            case 105: tmpl.bg = tty_cell_color_br_magenta; break;
            case 106: tmpl.bg = tty_cell_color_br_cyan; break;
            case 107: tmpl.bg = tty_cell_color_br_white; break;
                break;
            default:
                break;
            }
        }
        break;
    case 'n': /* device status report */
        handle_csi_dsr();
        break;
    case 'r': /* set scrolling region {line-start};{line-end}*/
        handle_scroll_region(opt_arg(0, 1), opt_arg(1, 1));
        break;
    case 't': /* window manager hints */
        handle_window_manager();
        break;
    }
}

void tty_teletype_impl::absorb(uint c)
{
    if (logger::L::Ltrace >= logger::level) {
        trace_charseq.push_back(c);
        trace_counter++;
    }

restart:
    switch (state) {
    case tty_state_normal:
        if ((c & 0xf8) == 0xf8) {
        } else if ((c & 0xf0) == 0xf0) {
            state = tty_state_utf4;
            code = c & 0x07;
        } else if ((c & 0xe0) == 0xe0) {
            state = tty_state_utf3;
            code = c & 0x0f;
        } else if ((c & 0xc0) == 0xc0) {
            state = tty_state_utf2;
            code = c & 0x1f;
        } else {
            if (c == tty_char_ESC) {
                state = tty_state_escape;
                argc = code = 0;
            } else if (c < 0x20) {
                handle_control(c);
            } else {
                handle_bare(c);
            }
            tty_line &line = hist.get_line(cur_line, true);
            memcpy(&line.tv, &tv, sizeof(tv));
        }
        break;
    case tty_state_utf4:
        code = (code << 6) | (c & 0x3f);
        state = tty_state_utf3;
        break;
    case tty_state_utf3:
        code = (code << 6) | (c & 0x3f);
        state = tty_state_utf2;
        break;
    case tty_state_utf2:
        code = (code << 6) | (c & 0x3f);
        handle_bare(code);
        state = tty_state_normal;
        break;
    case tty_state_escape:
        switch (c) {
        case '[':
            state = tty_state_csi0;
            return;
        case ']':
            state = tty_state_osc0;
            return;
        case 'M':
            handle_scroll();
            state = tty_state_normal;
            break;
        case '7':
            handle_save_cursor();
            state = tty_state_normal;
            break;
        case '8':
            handle_restore_cursor();
            state = tty_state_normal;
            break;
        case '(':
        case '*':
        case '+':
        case '-':
        case '.':
        case '/':
            code = c;
            state = tty_state_charset;
            return;
        case '=':
            handle_keypad_mode(true);
            state = tty_state_normal;
            break;
        case '>':
            handle_keypad_mode(false);
            state = tty_state_normal;
            break;
        default:
            Trace("absorb: invalid ESC char '%c' (0x%02x)\n", c, c);
            state = tty_state_normal;
            break;
        }
        break;
    case tty_state_charset:
        handle_charset(code, c);
        state = tty_state_normal;
        break;
    case tty_state_csi0:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            state = tty_state_csi;
            goto restart;
        case '@': case 'A': case 'B': case 'C': case 'D':
        case 'E': case 'F': case 'G': case 'H': case 'I':
        case 'J': case 'K': case 'L': case 'M': case 'P':
        case 'd': case 'e': case 'f': case 'm': case 'n':
        case 'r': case 't':
            handle_csi(c);
            state = tty_state_normal;
            break;
        case '?': /* DEC */
            state = tty_state_csi_dec;
            break;
        case '>': /* DEC2 */
            state = tty_state_csi_dec2;
            break;
        case '=': /* DEC3 */
            state = tty_state_csi_dec3;
            break;
        default:
            Trace("absorb: invalid CSI char '%c' (0x%02x)\n", c, c);
            state = tty_state_normal;
            goto restart;
        }
        break;
    case tty_state_csi:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            code = code * 10 + (c - '0');
            break;
        case ';':
            if (argc < array_size(argv)) {
                argv[argc++] = code; code = 0;
            } else {
                Trace("absorb: CSI too many args, ignoring %d\n",
                    code);
            }
            break;
        case '@': case 'A': case 'B': case 'C': case 'D':
        case 'E': case 'F': case 'G': case 'H': case 'I':
        case 'J': case 'K': case 'L': case 'M': case 'P':
        case 'd': case 'e': case 'f': case 'm': case 'n':
        case 'r': case 't':
            if (argc < array_size(argv)) {
                argv[argc++] = code; code = 0;
            } else {
                Trace("absorb: CSI too many args, ignoring %d\n",
                    code);
            }
            handle_csi(c);
            state = tty_state_normal;
            break;
        default:
            Trace("absorb: invalid CSI char '%c' (0x%02x)\n", c, c);
            state = tty_state_normal;
            break;
        }
        break;
    case tty_state_csi_dec:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            code = code * 10 + (c - '0');
            break;
        case ';':
            if (argc < array_size(argv)) {
                argv[argc++] = code; code = 0;
            } else {
                Trace("absorb: CSI ? too many args, ignoring %d\n",
                    code);
            }
            break;
        case 'c': case 'h': case 'i': case 'l': case 'n':
        case 'r': case 's': case 'S': case 'J': case 'K':
            if (argc < array_size(argv)) {
                argv[argc++] = code; code = 0;
            } else {
                Trace("absorb: CSI ? too many args, ignoring %d\n",
                    code);
            }
            handle_csi_dec(c);
            state = tty_state_normal;
            break;
        default:
            Trace("absorb: invalid CSI ? char '%c' (0x%02x)\n", c, c);
            state = tty_state_normal;
            break;
        }
        break;
    case tty_state_csi_dec2:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            code = code * 10 + (c - '0');
            break;
        case ';':
            if (argc < array_size(argv)) {
                argv[argc++] = code; code = 0;
            } else {
                Trace("absorb: CSI > too many args, ignoring %d\n",
                    code);
            }
            handle_csi_dec2(c);
            state = tty_state_normal;
            break;
        case 'c': /* device report */
            Trace("absorb: CSI > device report\n");
            state = tty_state_normal;
            break;
        default:
            Trace("absorb: invalid CSI > char '%c' (0x%02x)\n", c, c);
            state = tty_state_normal;
            break;
        }
        break;
    case tty_state_csi_dec3:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            code = code * 10 + (c - '0');
            break;
        case ';':
            if (argc < array_size(argv)) {
                argv[argc++] = code; code = 0;
            } else {
                Trace("absorb: CSI = too many args, ignoring %d\n",
                    code);
            }
            break;
        case 'c': /* device report */
            if (argc < array_size(argv)) {
                argv[argc++] = code; code = 0;
            } else {
                Trace("absorb: CSI = too many args, ignoring %d\n",
                    code);
            }
            handle_csi_dec3(c);
            state = tty_state_normal;
            break;
        default:
            Trace("absorb: invalid CSI = char '%c' (0x%02x)\n", c, c);
            state = tty_state_normal;
            break;
        }
        break;
    case tty_state_osc0:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            state = tty_state_osc;
            goto restart;
        case tty_char_BEL:
            handle_osc(c);
            state = tty_state_normal;
            break;
        default:
            Trace("absorb: invalid OSC char '%c' (0x%02x)\n", c, c);
            break;
        }
        break;
    case tty_state_osc:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            code = code * 10 + (c - '0');
            break;
        case ';':
            if (argc < array_size(argv)) {
                argv[argc++] = code; code = 0;
            } else {
                Trace("absorb: OSC too many args, ignoring %d\n",
                    code);
            }
            if (argc == 1 && argv[0] == 7) {
                state = tty_state_osc_string;
                osc_data.clear();
            }
            break;
        case tty_char_BEL:
            if (argc < array_size(argv)) {
                argv[argc++] = code; code = 0;
            } else {
                Trace("absorb: OSC too many args, ignoring %d\n",
                    code);
            }
            handle_osc(c);
            state = tty_state_normal;
            break;
        default:
            Debug("absorb: invalid OSC char '%c' (0x%02x)\n", c, c);
            break;
        }
        break;
    case tty_state_osc_string:
        if (c == tty_char_BEL) {
            handle_osc_string(c);
            state = tty_state_normal;
        } else {
            osc_data.append(std::string(1, c));
        }
        break;
    }
    needs_update = 1;
}

ssize_t tty_teletype_impl::io()
{
    struct pollfd pfds[1];
    ssize_t len;
    int ret;

    int do_poll_in  = -(in_buf.size() - in_end > 0);
    int do_poll_out = -(out_end - out_start > 0);

    pfds[0].fd = fd;
    pfds[0].events = (do_poll_in & POLLIN) | (do_poll_out & POLLOUT);
    ret = poll(pfds, array_size(pfds), io_poll_timeout);

    timestamp_gettime(tty_clock_realtime, &tv);

    if (pfds[0].revents & POLLOUT) {
        ssize_t count;
        if (out_start > out_end) {
            /* zero xxxxxxxx end ________ start <xxxxxx> limit */
            count = std::min(ssize_t(128), ssize_t(out_buf.size()) - out_start);
        } else {
            /* zero ________ start <xxxxxx> end ________ limit */
            count = std::min(ssize_t(128), out_end - out_start);
        }
        if (count > 0) {
            if ((len = write(fd, &out_buf[out_start], count)) < 0) {
                Panic("write failed: %s\n", strerror(errno));
            }
            if (debug_io) {
                logger::log(logger::L::Ltrace, "io: wrote %zu bytes -> pty\n", len);
                if (logger::L::Ltrace >= logger::level) {
                    dump_buffer((char*)&out_buf[out_start], len, [](const char* msg) {
                        logger::log(logger::L::Ltrace, "io: wrote: %s\n", msg);
                    });
                }
            }
            out_start += len;
        }
        if (out_start == out_buf.size()) {
            /* zero xxxxxxxx end ________________ start limit */
            /* zero start xxxxxxxx end ________________ limit */
            out_start = 0;
        }
    }

    if (pfds[0].revents & POLLIN) {
        ssize_t count;
        if (in_start > in_end) {
            /* zero xxxxxxxx end <xxx---> start xxxxxxxx limit */
            count = in_start - in_end;
        } else {
            /* zero ________ start xxxxxxxx end <xxx---> limit */
            count = in_buf.size() - in_end;
        }
        if (count > 0) {
            if ((len = ::read(fd, &in_buf[in_end], count)) < 0) {
                Panic("read failed: %s\n", strerror(errno));
            }
            if (debug_io) {
                logger::log(logger::L::Ltrace, "io: read %zu bytes -> pty\n", len);
                if (logger::L::Ltrace >= logger::level) {
                    dump_buffer((char*)&in_buf[in_end], len, [](const char* msg) {
                        logger::log(logger::L::Ltrace, "io: read: %s\n", msg);
                    });
                }
            }
            in_end += len;
            if (len == 0) return -1; /* EOF */
        }
        if (in_start < in_end && in_end == in_buf.size()) {
            /* zero ________ start xxxxxxxxxxxxxxxx end limit */
            /* zero end ________ start xxxxxxxxxxxxxxxx limit */
            in_end = 0;
        }
    }

    return 0;
}

ssize_t tty_teletype_impl::proc()
{
    size_t count;

    if (in_start > in_end) {
        /* zero xxxxxxxx end ________ start xxxxxxxx limit */
        count = in_buf.size() - in_start;
    } else {
        /* zero ________ start xxxxxxxx end ________ limit */
        count = in_end - in_start;
    }
    for (size_t i = 0; i < count; i++) {
        absorb(in_buf[in_start]);
        in_start++;
    }
    if (in_end < in_start && in_start == in_buf.size()) {
        /* zero xxxxxxxx end _________________ start limit */
        /* zero start xxxxxxxx end _________________ limit */
        in_start = 0;
    }
    if (count > 0) {
        if (debug_io) {
            Trace("proc: absorbed %zu bytes of input\n", count);
        }
    }
    return count;
}

ssize_t tty_teletype_impl::emit(const char *buf, size_t len)
{
    ssize_t count, ncopy = 0;
    if (out_start > out_end) {
        /* zero xxxxxxxx end <xxx---> start xxxxxxxx limit */
        count = out_start - out_end;
    } else {
        /* zero ________ start xxxxxxxx end <xxx---> limit */
        count = out_buf.size() - out_end;
    }
    if (count > 0) {
        ncopy = len < count ? len : count;
        memcpy(&out_buf[out_end], buf, ncopy);
        if (debug_io) {
            Trace("write: buffered %zu bytes of output\n", len);
        }
        out_end += ncopy;
    }
    if (out_start < out_end && out_end == out_buf.size()) {
        /* zero ________ start xxxxxxxxxxxxxxxx end limit */
        /* zero end ________ start xxxxxxxxxxxxxxxx limit */
        out_end = 0;
    }
    return ncopy;
}

void tty_teletype_impl::emit_loop(const char *buf, size_t len)
{
    size_t nbytes = 0, off = 0;
    do {
        if ((nbytes = emit(buf + off, len - off)) < 0) {
            Error("tty_teletype_impl::emit: %s\n", strerror(errno));
            return;
        }
    } while (nbytes > 0 && (off += nbytes) < len);
}

static std::string keypress_string(tty_keypress kp)
{
    std::string s;

    int mod, mods = kp.mods;
    do {
        if ((mod = mods & -mods)) {
            s.append(tty_keymap_mod_name(mod));
            s.append(" + ");
        }
    } while ((mods &= ~mod));
    s.append(tty_keymap_key_name(kp.key));

    return s;
}

static std::string translate_string(tty_translate_result r)
{
    std::string out;

    switch(r.oper) {
    case tty_oper_none:
        return std::string("none");
    case tty_oper_emit:
        return std::string("emit ") + control_string(r.data);
    case tty_oper_copy:
        return std::string("copy");
    case tty_oper_paste:
        return std::string("paste");
    }

    return "invalid";
}

bool tty_teletype_impl::keyboard(int key, int scancode, int action, int mods)
{
    switch (action) {
    case GLFW_PRESS:
    case GLFW_REPEAT:
        std::vector<tty_keypress> seq = { tty_keypress{ key, mods } };
        tty_translate_result r = tty_keymap_translate(seq, flags);
        if (r.oper == tty_oper_none) break;

        Trace("keyboard: translate %s -> %s\n",
            keypress_string(seq[0]).c_str(), translate_string(r).c_str());

        const char *str;
        switch (r.oper) {
        case tty_oper_emit:
            emit(r.data.c_str(), r.data.size());
            return true;
        case tty_oper_copy:
            app_set_clipboard(get_selected_text().c_str());
            return false;
        case tty_oper_paste:
            str = app_get_clipboard();
            if (has_flag(tty_flag_XTBP)) emit("\x1b[201~", 6);
            emit_loop(str, strlen(str));
            if (has_flag(tty_flag_XTBP)) emit("\x1b[200~", 6);
            return true;
        }
        break;
    }
    return false;
}
