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

struct tty_private_mode_rec
{
    uint code;
    uint flag;
    const char *name;
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

struct tty_packed_voff { tty_int48 lline, offset; };
struct tty_packed_loff { tty_int48 vline, count; };

struct tty_line_store
{
    std::vector<tty_cell> cells;
    std::vector<char> text;
    std::vector<tty_packed_line> lines;
    std::vector<tty_cached_line> cache;
    std::vector<tty_packed_voff> voffsets;
    std::vector<tty_packed_loff> loffsets;

    tty_packed_line pack(tty_line &uline);
    tty_line unpack(tty_packed_line &pline);
    tty_line& get_line(llong lline, bool edit);
    llong count_cells(tty_packed_line &pline);
    llong count_cells(llong lline);
    void clear_line(llong lline);
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
    uchar needs_capture;
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
    llong cur_row;
    llong cur_col;
    llong min_row;
    llong top_marg;
    llong bot_marg;

    tty_teletype_impl();
    virtual ~tty_teletype_impl() = default;

    virtual void close();
    virtual bool get_needs_update();
    virtual void set_needs_update();
    virtual bool get_needs_capture();
    virtual void set_needs_capture();
    virtual void update_offsets();
    virtual tty_line_voff visible_to_logical(llong vline);
    virtual tty_line_loff logical_to_visible(llong lline);
    virtual tty_line& get_line(llong lline);
    virtual void set_selection(tty_cell_span sel);
    virtual tty_cell_span get_selection();
    virtual std::string get_selected_text();
    virtual llong total_rows();
    virtual llong total_lines();
    virtual llong visible_rows();
    virtual llong visible_lines();
    virtual llong get_cur_row();
    virtual llong get_cur_col();
    virtual bool has_flag(uint check);
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
    void xtwinops();
    void set_row(llong row);
    void set_col(llong col);
    void move_abs(llong row, llong col);
    void move_rel(llong row, llong col);
    void scroll_region(llong line0, llong line1);
    void reset_style();
    void erase_screen(uint arg);
    void erase_line(uint arg);
    void insert_lines(uint arg);
    void delete_lines(uint arg);
    void delete_chars(uint arg);
    void handle_bell();
    void handle_backspace();
    void handle_horizontal_tab();
    void handle_line_feed();
    void handle_carriage_return();
    void handle_bare(uint c);
    void handle_control_character(uint c);
    void handle_charset(uint cmd, uint set);
    void osc(uint c);
    void osc_string(uint c);
    void csi_private_mode(uint code, uint set);
    void csi_dec(uint c);
    void csi_dec2(uint c);
    void csi_dec3(uint c);
    void csi_dsr();
    void csi(uint c);
    void absorb(uint c);
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
    needs_capture(0),
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
    cur_row(0),
    cur_col(0),
    min_row(0),
    top_marg(0),
    bot_marg(0)
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

bool tty_teletype_impl::get_needs_capture()
{
    bool flag = needs_capture;
    needs_capture = false;
    return flag;
}

void tty_teletype_impl::set_needs_capture()
{
    needs_capture |= true;
}

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

void tty_line_store::invalidate_cache()
{
    for (llong cl = 0; cl < line_cache_size; cl++) {
        llong olline = tty_int48_get(cache[cl].lline);
        if (olline >= 0 && cache[cl].dirty) {
            lines[olline] = pack(cache[cl].ldata);
            cache[cl].lline = tty_int48_set(-1);
        }
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
        voffsets.size(), sizeof(tty_packed_voff),
        voffsets.size() * sizeof(tty_packed_voff));
    Info("tty_line_store.loffsets    = %9zu x %2zu (%9zu)\n",
        loffsets.size(), sizeof(tty_packed_loff),
        loffsets.size() * sizeof(tty_packed_loff));
    Info("tty_line_store.pack.lines  = %9zu x %2zu (%9zu)\n",
        lines.size(), sizeof(tty_packed_line),
        lines.size() * sizeof(tty_packed_line));
    Info("tty_line_store.pack.cells  = %9zu x %2zu (%9zu)\n",
        cells.size(), sizeof(tty_cell), cells.size() * sizeof(tty_cell));
    Info("tty_line_store.pack.text   = %9zu x %2zu (%9zu)\n",
        text.size(), sizeof(char), text.size() * sizeof(char));
    size_t total = cache.size() * sizeof(tty_cached_line)
                 + cache_cells * sizeof(tty_cell)
                 + voffsets.size() * sizeof(tty_packed_voff)
                 + loffsets.size() * sizeof(tty_packed_loff)
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
        return;
    }

    /* recompute line offsets incrementally from min_row */
    if (min_row == 0) {
        vlstart = 0;
    } else {
        auto &loff = hist.loffsets[min_row - 1];
        vlstart = tty_int48_get(loff.vline) + tty_int48_get(loff.count);
    }

    /* count lines with wrap incrementally from min row */
    vl = vlstart;
    for (llong k = min_row; k < hist.lines.size(); k++) {
        llong cell_count = hist.count_cells(k);
        llong wrap_count = cell_count == 0 ? 1
            : cols == 0 ? 1 : wrap_enabled ? (cell_count + cols - 1) / cols : 1;
        vl += wrap_count;
    }

    /* write out indices incrementally from min row */
    hist.voffsets.resize(vl);
    hist.loffsets.resize(hist.lines.size());
    vl = vlstart;
    for (llong k = min_row; k < hist.lines.size(); k++) {
        llong cell_count = hist.count_cells(k);
        llong wrap_count = cell_count == 0 ? 1
            : cols == 0 ? 1 : wrap_enabled ? (cell_count + cols - 1) / cols : 1;
        hist.loffsets[k] = { tty_int48_set(vl), tty_int48_set(wrap_count) };
        for (llong j = 0; j < wrap_count; j++, vl++) {
            hist.voffsets[vl] = { tty_int48_set(k), tty_int48_set(j * cols) };
        }
    }

    /* set min_row to cur_row */
    min_row = cur_row;
}

tty_line_voff tty_teletype_impl::visible_to_logical(llong vline)
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;

    if (wrap_enabled) {
        if (vline < 0) {
            return tty_line_voff{ -1, 0 };
        }
        else if (vline < hist.voffsets.size()) {
            return tty_line_voff{
                tty_int48_get(hist.voffsets[vline].lline),
                tty_int48_get(hist.voffsets[vline].offset)
            };
        }
        else {
            return tty_line_voff{ (llong)hist.loffsets.size(), 0 };
        }
    } else {
        return tty_line_voff{ vline, 0 };
    }
}

tty_line_loff tty_teletype_impl::logical_to_visible(llong lline)
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;

    if (wrap_enabled) {
        if (lline < 0) {
            return tty_line_loff{ -1, 0 };
        }
        if (lline < hist.loffsets.size()) {
            return tty_line_loff{
                tty_int48_get(hist.loffsets[lline].vline),
                tty_int48_get(hist.loffsets[lline].count)
            };
        } else {
            size_t last = hist.loffsets.size() - 1;
            return tty_line_loff{ (llong)hist.voffsets.size(), 0 };
        }
    } else {
        return tty_line_loff{ lline, 0 };
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

llong tty_teletype_impl::total_lines()
{
    return hist.lines.size();
}

llong tty_teletype_impl::visible_rows()
{
    return ws.vis_rows;
}

llong tty_teletype_impl::visible_lines()
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;
    llong rows = ws.vis_rows;

    if (wrap_enabled) {
        /*
         * calculate the number of visible lines so that we can calculate
         * absolute position while considering dynamically wrapped lines.
         */
        llong row_count = total_rows(), wrapped_rows = 0;
        for (llong j = row_count-1, l = 0; l < rows && j >= 0; j--, l++)
        {
            if (j >= row_count) continue;
            if (visible_to_logical(j).offset > 0) wrapped_rows++;
        }
        return rows - wrapped_rows;
    } else {
        return rows;
    }
}

llong tty_teletype_impl::get_cur_row()
{
    return cur_row;
}

llong tty_teletype_impl::get_cur_col()
{
    return cur_col;
}

bool tty_teletype_impl::has_flag(uint check)
{
    return (flags & check) == check;
}

tty_winsize tty_teletype_impl::get_winsize()
{
    return ws;
}

void tty_teletype_impl::set_winsize(tty_winsize d)
{
    if (ws != d) {
        ws = d;
        min_row = 0;
    }
}

void tty_teletype_impl::set_row(llong row)
{
    if (row < 0) row = 0;
    if (row != cur_row) {
        if (row >= hist.lines.size()) {
            hist.lines.resize(row + 1);
        }
        min_row = std::min(min_row, (cur_row = row));
        update_offsets();
    }
}

void tty_teletype_impl::set_col(llong col)
{
    if (col < 0) col = 0;
    if (col != cur_col) {
        cur_col = col;
    }
}

void tty_teletype_impl::move_abs(llong row, llong col)
{
    Trace("move_abs: %lld %lld\n", row, col);
    if (row != -1) {
        update_offsets();
        llong vis_lines = visible_lines();
        size_t new_row = std::max(0ll, std::min((llong)hist.lines.size() - 1,
                        (llong)hist.lines.size() - vis_lines + row - 1));
        set_row(new_row);
    }
    if (col != -1) {
        set_col(std::max(0ll, col - 1));
    }
}

void tty_teletype_impl::move_rel(llong row, llong col)
{
    Trace("move_rel: %lld %lld\n", row, col);
    llong new_row = cur_row + row;
    llong new_col = col == tty_col_home ? 0 : cur_col + col;
    if (new_row < 0) new_row = 0;
    if (new_col < 0) new_col = 0;
    set_row(new_row);
    set_col(new_col);
}

void tty_teletype_impl::scroll_region(llong line0, llong line1)
{
    Trace("scroll_region: %lld %lld\n", line0, line1);
    top_marg = line0;
    bot_marg = line1;
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
    move_abs(1, 1);
    reset_style();
}

void tty_teletype_impl::erase_screen(uint arg)
{
    Trace("erase_screen: %d\n", arg);
    switch (arg) {
    case tty_clear_end:
        for (size_t row = cur_row; row < hist.lines.size(); row++) {
            hist.clear_line(row);
        }
        break;
    case tty_clear_start:
        for (ssize_t row = cur_row; row < hist.lines.size(); row--) {
            hist.clear_line(row);
        }
        break;
    case tty_clear_all:
        for (size_t row = 0; row < hist.lines.size(); row++) {
            hist.clear_line(row);
        }
        move_abs(1, 1);
        break;
    }
}

void tty_teletype_impl::erase_line(uint arg)
{
    Trace("erase_line: %d\n", arg);
    switch (arg) {
    case tty_clear_end:
        if (cur_col < hist.count_cells(cur_row)) {
            tty_line &line = hist.get_line(cur_row, true);
            line.cells.resize(cur_col);
        }
        break;
    case tty_clear_start:
        if (cur_col < hist.count_cells(cur_row)) {
            tty_line &line = hist.get_line(cur_row, true);
            tty_cell cell = tmpl;
            cell.codepoint = ' ';
            for (size_t col = 0; col < cur_col; col++) {
                line.cells[col] = cell;
            }
        }
        break;
    case tty_clear_all: {
        tty_line &line = hist.get_line(cur_row, true);
        line.cells.resize(0);
        break;
    }
    }
}

void tty_teletype_impl::insert_lines(uint arg)
{
    Trace("insert_lines: %d\n", arg);
    if (arg == 0) return;
    // consider line editing mode: *following*, or preceding
    // consider bottom margin for following mode. add bounds checks
    llong top = top_marg == 0 ? 1           : top_marg;
    llong bot = bot_marg == 0 ? ws.vis_rows : bot_marg;
    llong scrolloff = bot < ws.vis_rows ? ws.vis_rows - bot : 0;
    hist.invalidate_cache();
    for (uint i = 0; i < arg; i++) {
        hist.lines.insert(hist.lines.begin() + cur_row, tty_packed_line{});
        hist.lines.erase(hist.lines.end() - 1 - scrolloff);
    }
    cur_col = 0;
}

void tty_teletype_impl::delete_lines(uint arg)
{
    Trace("delete_lines: %d\n", arg);
    if (arg == 0) return;
    // consider line editing mode: *following*, or preceding
    // consider bottom margin for following mode. add bounds checks
    llong top = top_marg == 0 ? 1           : top_marg;
    llong bot = bot_marg == 0 ? ws.vis_rows : bot_marg;
    llong scrolloff = bot < ws.vis_rows ? ws.vis_rows - bot : 0;
    hist.invalidate_cache();
    for (uint i = 0; i < arg; i++) {
        if (cur_row < hist.lines.size()) {
            hist.lines.erase(hist.lines.begin() + cur_row);
            hist.lines.insert(hist.lines.end() - scrolloff, tty_packed_line{});
        }
    }
    cur_col = 0;
}

void tty_teletype_impl::delete_chars(uint arg)
{
    Trace("delete_chars: %d\n", arg);
    for (size_t i = 0; i < arg; i++) {
        if (cur_col < hist.count_cells(cur_row)) {
            tty_line &line = hist.get_line(cur_row, true);
            line.cells.erase(
                line.cells.begin() + cur_col
            );
        }
    }
}

void tty_teletype_impl::handle_bell()
{
    Trace("handle_bell: unimplemented\n");
}

void tty_teletype_impl::handle_backspace()
{
    Trace("handle_backspace\n");
    if (cur_col > 0) cur_col--;
}

void tty_teletype_impl::handle_horizontal_tab()
{
    Trace("handle_horizontal_tab\n");
    cur_col = (cur_col + 8) & ~7;
}

void tty_teletype_impl::handle_line_feed()
{
    Trace("handle_line_feed\n");
    set_row(cur_row + 1);
}

void tty_teletype_impl::handle_carriage_return()
{
    Trace("handle_carriage_return\n");
    cur_col = 0;
}

void tty_teletype_impl::handle_bare(uint c)
{
    Trace("handle_bare: %s\n", char_str(c).c_str());
    tty_line &line = hist.get_line(cur_row, true);
    if (cur_col >= line.cells.size()) {
        line.cells.resize(cur_col + 1);
    }
    line.cells[cur_col++] =
        tty_cell{c, tmpl.flags, tmpl.fg, tmpl.bg};
}

void tty_teletype_impl::handle_control_character(uint c)
{
    Trace("handle_control_character: %s\n", char_str(c).c_str());
    switch (c) {
    case tty_char_BEL: handle_bell(); break;
    case tty_char_BS: handle_backspace(); break;
    case tty_char_HT: handle_horizontal_tab(); break;
    case tty_char_LF: handle_line_feed(); break;
    case tty_char_CR: handle_carriage_return(); break;
    default:
        Debug("handle_control_character: unhandled control character %s\n",
            char_str(c).c_str());
    }
}

void tty_teletype_impl::xtwinops()
{
    Debug("xtwinops: %s unimplemented\n", args_str().c_str());
}

void tty_teletype_impl::handle_charset(uint cmd, uint set)
{
    Debug("handle_charset: %c %c unimplemented\n", cmd, set);
}

void tty_teletype_impl::osc(uint c)
{
    Debug("osc: %s %s unimplemented\n",
        args_str().c_str(), char_str(c).c_str());
    if (argc == 1 && argv[0] == 555) {
        Debug("osc: screen-capture\n");
        set_needs_capture();
    } else if (argc == 1 && argv[0] == 556) {
        hist.dump_stats();
    }
}

void tty_teletype_impl::osc_string(uint c)
{
    Debug("osc_string: %s %s \"%s\" unimplemented\n",
        args_str().c_str(), char_str(c).c_str(), osc_data.c_str());
}

static tty_private_mode_rec* tty_lookup_private_mode_rec(uint code)
{
    for (size_t i = 0; i < array_size(dec_flags); i++) {
        if (dec_flags[i].code == code) return dec_flags + i;
    }
    return NULL;
}

void tty_teletype_impl::csi_private_mode(uint code, uint set)
{
    tty_private_mode_rec *rec = tty_lookup_private_mode_rec(code);
    if (rec == NULL) {
        Debug("csi_private_mode: %s flag %d: unimplemented\n",
            set ? "set" : "clear", code);
    } else {
        Debug("csi_private_mode: %s flag %d: %s\n",
            set ? "set" : "clear", code, rec->name);
        if (set) {
            flags |= rec->flag;
        } else {
            flags &= ~rec->flag;
        }
    }
}

void tty_teletype_impl::csi_dec(uint c)
{
    switch (c) {
    case 'l': csi_private_mode(opt_arg(0, 0), 0); break;
    case 'h': csi_private_mode(opt_arg(0, 0), 1); break;
    default:
        Debug("csi_dec: %s %s unimplemented\n",
            char_str(c).c_str(), args_str().c_str());
        break;
    }
}

void tty_teletype_impl::csi_dec2(uint c)
{
    Debug("csi_dec2: %s %s unimplemented\n",
        char_str(c).c_str(), args_str().c_str());
}

void tty_teletype_impl::csi_dec3(uint c)
{
    Debug("csi_dec3: %s %s unimplemented\n",
        char_str(c).c_str(), args_str().c_str());
}

void tty_teletype_impl::csi_dsr()
{
    Trace("csi_dsr: %s\n", args_str().c_str());
    switch (opt_arg(0, 0)) {
    case 6: { /* report cursor position */
        char buf[32];
        update_offsets();
        llong vis_lines = visible_lines();
        llong col = cur_col + 1;
        llong row = cur_row - (hist.lines.size() - vis_lines) + 1;
        row = std::max(1ll, std::min(row, vis_lines));
        int len = snprintf(buf, sizeof(buf), "\x1b[%llu;%lluR", row, col);
        emit(buf, len);
        break;
    }
    default:
        Debug("csi_dsr: %s\n", args_str().c_str());
        break;
    }
}

void tty_teletype_impl::csi(uint c)
{
    Trace("csi: %s %s\n",
        args_str().c_str(), char_str(c).c_str());

    switch (c) {
    case '@': /* insert blanks */
    {
        tty_line &line = hist.get_line(cur_row, true);
        int n = opt_arg(0, 0);
        if (cur_col < line.cells.size()) {
            tty_cell cell = tmpl;
            cell.codepoint = ' ';
            for (size_t i = 0; i < n; i++) {
                line.cells.insert(line.cells.begin() + cur_col, cell);
            }
        }
        break;
    }
    case 'A': /* move up */
        move_rel(-opt_arg(0, 1), 0);
        break;
    case 'B': /* move down */
        move_rel(opt_arg(0, 1),  0);
        break;
    case 'C': /* move right */
        move_rel(0,  opt_arg(0, 1));
        break;
    case 'D': /* move left */
        move_rel(0, -opt_arg(0, 1));
        break;
    case 'E': /* move next line */
        move_rel(opt_arg(0, 1), tty_col_home);
        break;
    case 'F': /* move prev line */
        move_rel(-opt_arg(0, 1), tty_col_home);
        break;
    case 'G': /* move to {col} */
        move_abs(-1, opt_arg(0, 1));
        break;
    case 'H': /* move to {line};{col} */
        move_abs(opt_arg(0, 1), opt_arg(1, 1));
        break;
    case 'J': /* erase lines {0=to-end,1=from-start,2=all} */
        switch (opt_arg(0, 0)) {
        case 0: erase_screen(tty_clear_end); break;
        case 1: erase_screen(tty_clear_start); break;
        case 2: erase_screen(tty_clear_all); break;
        default:
            Debug("csi: CSI J: invalid arg: %d\n", opt_arg(0, 0));
            break;
        }
        break;
    case 'K': /* erase chars {0=to-end,1=from-start,2=all} */
        switch (opt_arg(0, 0)) {
        case 0: erase_line(tty_clear_end); break;
        case 1: erase_line(tty_clear_start); break;
        case 2: erase_line(tty_clear_all); break;
        default:
            Debug("csi: CSI K: invalid arg: %d\n", opt_arg(0, 0));
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
        move_abs(opt_arg(0, 1), -1);
        break;
    case 'e': /* move to {line};1 relative */
        move_rel(opt_arg(0, 1), 0);
        break;
    case 'f': /* move to {line};{col} absolute */
        move_abs(opt_arg(0, 1), opt_arg(1, 1));
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
        csi_dsr();
        break;
    case 'r': /* set scrolling region {line-start};{line-end}*/
        scroll_region(opt_arg(0, 1), opt_arg(1, 1));
        break;
    case 't': /* window manager hints */
        xtwinops();
        break;
    }
}

void tty_teletype_impl::absorb(uint c)
{
    Trace("absorb: %s\n", char_str(c).c_str());
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
                handle_control_character(c);
            } else {
                handle_bare(c);
            }
            tty_line &line = hist.get_line(cur_row, true);
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
            Debug("absorb: enter alternate keypad mode: unimplemented\n");
            state = tty_state_normal;
            return;
        case '>':
            Debug("absorb: exit alternate keypad mode: unimplemented\n");
            state = tty_state_normal;
            return;
        default:
            Debug("absorb: invalid ESC char '%c' (0x%02x)\n", c, c);
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
            csi(c);
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
            Debug("absorb: invalid CSI char '%c' (0x%02x)\n", c, c);
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
                Debug("absorb: CSI too many args, ignoring %d\n",
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
                Debug("absorb: CSI too many args, ignoring %d\n",
                    code);
            }
            csi(c);
            state = tty_state_normal;
            break;
        default:
            Debug("absorb: invalid CSI char '%c' (0x%02x)\n", c, c);
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
                Debug("absorb: CSI ? too many args, ignoring %d\n",
                    code);
            }
            break;
        case 'c': case 'h': case 'i': case 'l': case 'n':
        case 'r': case 's': case 'S': case 'J': case 'K':
            if (argc < array_size(argv)) {
                argv[argc++] = code; code = 0;
            } else {
                Debug("absorb: CSI ? too many args, ignoring %d\n",
                    code);
            }
            csi_dec(c);
            state = tty_state_normal;
            break;
        default:
            Debug("absorb: invalid CSI ? char '%c' (0x%02x)\n", c, c);
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
                Debug("absorb: CSI > too many args, ignoring %d\n",
                    code);
            }
            csi_dec2(c);
            state = tty_state_normal;
            break;
        case 'c': /* device report */
            Debug("absorb: CSI > device report\n");
            state = tty_state_normal;
            break;
        default:
            Debug("absorb: invalid CSI > char '%c' (0x%02x)\n", c, c);
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
                Debug("absorb: CSI = too many args, ignoring %d\n",
                    code);
            }
            break;
        case 'c': /* device report */
            if (argc < array_size(argv)) {
                argv[argc++] = code; code = 0;
            } else {
                Debug("absorb: CSI = too many args, ignoring %d\n",
                    code);
            }
            csi_dec3(c);
            state = tty_state_normal;
            break;
        default:
            Debug("absorb: invalid CSI = char '%c' (0x%02x)\n", c, c);
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
            osc(c);
            state = tty_state_normal;
            break;
        default:
            Debug("absorb: invalid OSC char '%c' (0x%02x)\n", c, c);
            //state = tty_state_normal;
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
                Debug("absorb: OSC too many args, ignoring %d\n",
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
                Debug("absorb: OSC too many args, ignoring %d\n",
                    code);
            }
            osc(c);
            state = tty_state_normal;
            break;
        default:
            Debug("absorb: invalid OSC char '%c' (0x%02x)\n", c, c);
            //state = tty_state_normal;
            break;
        }
        break;
    case tty_state_osc_string:
        if (c == tty_char_BEL) {
            osc_string(c);
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
            count = out_buf.size() - out_start;
        } else {
            /* zero ________ start <xxxxxx> end ________ limit */
            count = out_end - out_start;
        }
        if (count > 0) {
            if ((len = write(fd, &out_buf[out_start], count)) < 0) {
                Panic("write failed: %s\n", strerror(errno));
            }
            Debug("io: wrote %zu bytes -> pty\n", len);
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
            Debug("io: read %zu bytes <- pty\n", len);
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
        Debug("proc: processed %zu bytes of input\n", count);
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
        Debug("write: buffered %zu bytes of output\n", len);
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
