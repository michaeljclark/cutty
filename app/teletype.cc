#include <cstdlib>
#include <cerrno>
#include <cassert>

#include <time.h>
#include <poll.h>
#include <unistd.h>

#include "app.h"
#include "utf8.h"
#include "colors.h"
#include "teletype.h"
#include "process.h"

static int io_buffer_size = 65536;
static int io_poll_timeout = 1;

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

tty_teletype::tty_teletype() :
    state(tty_state_normal),
    flags(tty_flag_DECAWM | tty_flag_DECTCEM),
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
    lines(),
    voffsets(),
    loffsets(),
    ws{0,0,0,0},
    cur_row(0),
    cur_col(0),
    min_row(0),
    top_marg(0),
    bot_marg(0)
{
    in_buf.resize(io_buffer_size);
    out_buf.resize(io_buffer_size);
    lines.push_back(tty_line{});
}

tty_teletype* tty_new()
{
    return new tty_teletype();
}

void tty_teletype::close()
{
    ::close(fd);
    fd = -1;
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

std::string tty_teletype::args_str()
{
    std::string s;
    for (size_t i = 0; i < argc; i++) {
        if (i > 0) s.append(";");
        s.append(std::to_string(argv[i]));
    }
    return s;
}

int tty_teletype::opt_arg(int arg, int opt)
{
    return arg < argc ? argv[arg] : opt;
}

void tty_teletype::send(uint c)
{
    Trace("send: %s\n", char_str(c).c_str());
    char b = (char)c;
    write(&b, 1);
}

/*
 * - unpacked lines: cells vector has one element for every character and
 *   each cell has a utf32 codepoint. style flags, foreground and background
 *   colors. the cell count for the line is in cells.size().
 * - packed lines: cells vector holds style changes. the codepoint element
 *   contains an offset into utf8_data and the cell count is in pcount.
 */

size_t tty_line::count() { return pcount > 0 ? pcount : cells.size(); }

bool tty_line::ispacked() { return pcount > 0; }

void tty_line::pack()
{
    if (pcount > 0) return;

    std::vector<tty_cell> &ucells = cells;
    std::vector<tty_cell> pcells;

    utf8.clear();

    tty_cell t = { (uint)-1 };
    for (size_t i = 0; i < ucells.size(); i++) {
        tty_cell s = ucells[i];
        if (s.flags != t.flags || s.fg != t.fg || s.bg != t.bg) {
            t = tty_cell{(uint)utf8.size(), s.flags, s.fg, s.bg};
            pcells.push_back(t);
        }
        char u[8];
        size_t l = utf32_to_utf8(u, sizeof(u), s.codepoint);
        utf8.append(std::string(u, l));
    }

    pcount = cells.size();
    cells = pcells;
    cells.shrink_to_fit();
    utf8.shrink_to_fit();
}

void tty_line::unpack()
{
    if (pcount == 0) return;

    std::vector<tty_cell> &pcells = cells;
    std::vector<tty_cell> ucells;

    tty_cell t = { 0 };
    size_t o = 0, q = 0, l = utf8.size();
    while (o < l) {
        if (q < pcells.size() && pcells[q].codepoint == o) {
            t = pcells[q++];
        }
        utf32_code v = utf8_to_utf32_code(&utf8[o]);
        ucells.push_back(tty_cell{(uint)v.code, t.flags, t.fg, t.bg});
        o += v.len;
    }

    pcount = 0;
    cells = ucells;
    utf8.clear();
}

void tty_line::clear()
{
    pcount = 0;
    cells.clear();
    utf8.clear();
    cells.shrink_to_fit();
    utf8.shrink_to_fit();
}

void tty_teletype::update_offsets()
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;
    size_t cols = ws.vis_cols;
    size_t vlstart, vl;

    if (!wrap_enabled) {
        voffsets.clear();
        loffsets.clear();
        return;
    }

    /* recompute line offsets incrementally from min_row */
    if (min_row == 0) {
        vlstart = 0;
    } else {
        auto &loff = loffsets[min_row - 1];
        vlstart = loff.vline + loff.count;
    }

    /* count lines with wrap incrementally from min row */
    vl = vlstart;
    for (size_t k = min_row; k < lines.size(); k++) {
        size_t cell_count = lines[k].count();
        size_t wrap_count = cell_count == 0 ? 1
            : wrap_enabled ? (cell_count + cols - 1) / cols : 1;
        vl += wrap_count;
    }

    /* write out indices incrementally from min row */
    voffsets.resize(vl);
    loffsets.resize(lines.size());
    vl = vlstart;
    for (size_t k = min_row; k < lines.size(); k++) {
        size_t cell_count = lines[k].count();
        size_t wrap_count = cell_count == 0 ? 1
            : wrap_enabled ? (cell_count + cols - 1) / cols : 1;
        loffsets[k] = { vl, wrap_count };
        for (size_t j = 0; j < wrap_count; j++, vl++) {
            voffsets[vl] = { k, j * cols };
        }
    }

    /* set min_row to cur_row */
    min_row = cur_row;
}

tty_line_voff tty_teletype::visible_to_logical(llong vline)
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;

    if (wrap_enabled) {
        return voffsets[vline];
    } else {
        return tty_line_voff{ (size_t)vline, 0 };
    }
}

tty_line_loff tty_teletype::logical_to_visible(llong lline)
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;

    if (wrap_enabled) {
        return loffsets[lline];
    } else {
        return tty_line_loff{ (size_t)lline, 0 };
    }
}

llong tty_teletype::total_rows()
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;

    return wrap_enabled ? voffsets.size() : lines.size();
}

llong tty_teletype::total_lines()
{
    return lines.size();
}

llong tty_teletype::visible_rows()
{
    return ws.vis_rows;
}

llong tty_teletype::visible_lines()
{
    bool wrap_enabled = (flags & tty_flag_DECAWM) > 0;
    llong rows = ws.vis_rows;

    if (wrap_enabled) {
        /*
         * calculate the number of visible lines so that we can calculate
         * absolute position while considering dynamically wrapped lines.
         */
        llong total_rows = wrap_enabled ? voffsets.size() : lines.size();
        llong wrapped_rows = 0;
        for (llong j = total_rows - 1, l = 0; l < rows && j >= 0; j--, l++)
        {
            if (j >= total_rows) continue;
            if (visible_to_logical(j).offset > 0) wrapped_rows++;
        }
        return rows - wrapped_rows;
    } else {
        return rows;
    }
}

tty_winsize tty_teletype::get_winsize()
{
    return ws;
}

void tty_teletype::set_winsize(tty_winsize d)
{
    if (ws != d) {
        ws = d;
        min_row = 0;
    }
}

void tty_teletype::set_row(llong row)
{
    if (row < 0) row = 0;
    if (row != cur_row) {
        lines[cur_row].pack();
        if (row >= lines.size()) {
            lines.resize(row + 1);
        } else {
            lines[row].unpack();
        }
        cur_row = row;
        min_row = std::min(min_row, row);
    }
}

void tty_teletype::set_col(llong col)
{
    if (col < 0) col = 0;
    if (col != cur_col) {
        cur_col = col;
    }
}

void tty_teletype::move_abs(llong row, llong col)
{
    Trace("move_abs: %lld %lld\n", row, col);
    if (row != -1) {
        update_offsets();
        llong vis_lines = visible_lines();
        size_t new_row = std::max(0ll, std::min((llong)lines.size() - 1,
                        (llong)lines.size() - vis_lines + row - 1));
        set_row(new_row);
    }
    if (col != -1) {
        set_col(std::max(0ll, col - 1));
    }
}

void tty_teletype::move_rel(llong row, llong col)
{
    Trace("move_rel: %lld %lld\n", row, col);
    llong new_row = cur_row + row;
    llong new_col = col == tty_col_home ? 0 : cur_col + col;
    if (new_row < 0) new_row = 0;
    if (new_col < 0) new_col = 0;
    set_row(new_row);
    set_col(new_col);
}

void tty_teletype::scroll_region(llong line0, llong line1)
{
    Trace("scroll_region: %lld %lld\n", line0, line1);
    top_marg = line0;
    bot_marg = line1;
}

void tty_teletype::reset_style()
{
    tmpl.flags = 0;
    tmpl.fg = tty_cell_color_fg_dfl;
    tmpl.bg = tty_cell_color_bg_dfl;
}

void tty_teletype::set_fd(int fd)
{
    this->fd = fd;
}

void tty_teletype::reset()
{
    Trace("reset\n");
    move_abs(1, 1);
    reset_style();
}

void tty_teletype::erase_screen(uint arg)
{
    Trace("erase_screen: %d\n", arg);
    switch (arg) {
    case tty_clear_end:
        for (size_t row = cur_row; row < lines.size(); row++) {
            lines[row].clear();
        }
        break;
    case tty_clear_start:
        for (ssize_t row = cur_row; row < lines.size(); row--) {
            lines[row].clear();
        }
        break;
    case tty_clear_all:
        for (size_t row = 0; row < lines.size(); row++) {
            lines[row].clear();
        }
        move_abs(1, 1);
        break;
    }
}

void tty_teletype::erase_line(uint arg)
{
    Trace("erase_line: %d\n", arg);
    switch (arg) {
    case tty_clear_end:
        if (cur_col < lines[cur_row].cells.size()) {
            lines[cur_row].cells.resize(cur_col);
        }
        break;
    case tty_clear_start:
        if (cur_col < lines[cur_row].cells.size()) {
            tty_cell cell = tmpl;
            cell.codepoint = ' ';
            for (size_t col = 0; col < cur_col; col++) {
                lines[cur_row].cells[col] = cell;
            }
        }
        break;
    case tty_clear_all:
        lines[cur_row].cells.resize(0);
        break;
    }
}

void tty_teletype::insert_lines(uint arg)
{
    Trace("insert_lines: %d\n", arg);
    if (arg == 0) return;
    // consider line editing mode: *following*, or preceding
    // consider bottom margin for following mode. add bounds checks
    llong top = top_marg == 0 ? 1           : top_marg;
    llong bot = bot_marg == 0 ? ws.vis_rows : bot_marg;
    llong scrolloff = bot < ws.vis_rows ? ws.vis_rows - bot : 0;
    lines[cur_row].pack();
    for (uint i = 0; i < arg; i++) {
        lines.insert(lines.begin() + cur_row, tty_line{});
        lines.erase(lines.end() - 1 - scrolloff);
    }
    lines[cur_row].unpack();
    cur_col = 0;
}

void tty_teletype::delete_lines(uint arg)
{
    Trace("delete_lines: %d\n", arg);
    if (arg == 0) return;
    // consider line editing mode: *following*, or preceding
    // consider bottom margin for following mode. add bounds checks
    llong top = top_marg == 0 ? 1           : top_marg;
    llong bot = bot_marg == 0 ? ws.vis_rows : bot_marg;
    llong scrolloff = bot < ws.vis_rows ? ws.vis_rows - bot : 0;
    lines[cur_row].pack();
    for (uint i = 0; i < arg; i++) {
        if (cur_row < lines.size()) {
            lines.erase(lines.begin() + cur_row);
            lines.insert(lines.end() - scrolloff, tty_line{});
        }
    }
    lines[cur_row].unpack();
    cur_col = 0;
}

void tty_teletype::delete_chars(uint arg)
{
    Trace("delete_chars: %d\n", arg);
    for (size_t i = 0; i < arg; i++) {
        if (cur_col < lines[cur_row].cells.size()) {
            lines[cur_row].cells.erase(
                lines[cur_row].cells.begin() + cur_col
            );
        }
    }
}

void tty_teletype::handle_bell()
{
    Trace("handle_bell: unimplemented\n");
}

void tty_teletype::handle_backspace()
{
    Trace("handle_backspace\n");
    if (cur_col > 0) cur_col--;
}

void tty_teletype::handle_horizontal_tab()
{
    Trace("handle_horizontal_tab\n");
    cur_col = (cur_col + 8) & ~7;
}

void tty_teletype::handle_line_feed()
{
    Trace("handle_line_feed\n");
    set_row(cur_row + 1);
}

void tty_teletype::handle_carriage_return()
{
    Trace("handle_carriage_return\n");
    cur_col = 0;
}

void tty_teletype::handle_bare(uint c)
{
    Trace("handle_bare: %s\n", char_str(c).c_str());
    if (cur_col >= lines[cur_row].cells.size()) {
        lines[cur_row].cells.resize(cur_col + 1);
    }
    lines[cur_row].cells[cur_col++] =
        tty_cell{c, tmpl.flags, tmpl.fg, tmpl.bg};
}

void tty_teletype::handle_control_character(uint c)
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

void tty_teletype::xtwinops()
{
    Debug("xtwinops: %s unimplemented\n", args_str().c_str());
}

void tty_teletype::handle_charset(uint cmd, uint set)
{
    Debug("handle_charset: %c %c unimplemented\n", cmd, set);
}

void tty_teletype::osc(uint c)
{
    Debug("osc: %s %s unimplemented\n",
        args_str().c_str(), char_str(c).c_str());
    if (argc == 1 && argv[0] == 555) {
        Debug("osc: screen-capture\n");
        needs_capture = 1;
    }
}

void tty_teletype::osc_string(uint c)
{
    Debug("osc_string: %s %s \"%s\" unimplemented\n",
        args_str().c_str(), char_str(c).c_str(), osc_data.c_str());
}

struct tty_private_mode_rec
{
    uint code;
    uint flag;
    const char *name;
    const char *desc;
};

static tty_private_mode_rec dec_flags[] = {
    {  1, tty_flag_DECCKM,  "DECCKM",  "DEC Cursor Key Mode" },
    {  7, tty_flag_DECAWM,  "DECAWM",  "DEC Auto Wrap Mode" },
    { 25, tty_flag_DECTCEM, "DECTCEM", "DEC Text Cursor Enable Mode" },
};

static tty_private_mode_rec* tty_lookup_private_mode_rec(uint code)
{
    for (size_t i = 0; i < array_size(dec_flags); i++) {
        if (dec_flags[i].code == code) return dec_flags + i;
    }
    return NULL;
}

void tty_teletype::csi_private_mode(uint code, uint set)
{
    tty_private_mode_rec *rec = tty_lookup_private_mode_rec(code);
    if (rec == NULL) {
        Debug("csi_private_mode: %s flag %d: unimplemented\n",
            set ? "set" : "clear", code);
    } else {
        Debug("csi_private_mode: %s flag %d: %s /* %s */\n",
            set ? "set" : "clear", code, rec->name, rec->desc);
        if (set) {
            flags |= rec->flag;
        } else {
            flags &= ~rec->flag;
        }
    }
}

void tty_teletype::csi_dec(uint c)
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

void tty_teletype::csi_dec2(uint c)
{
    Debug("csi_dec2: %s %s unimplemented\n",
        char_str(c).c_str(), args_str().c_str());
}

void tty_teletype::csi_dec3(uint c)
{
    Debug("csi_dec3: %s %s unimplemented\n",
        char_str(c).c_str(), args_str().c_str());
}

void tty_teletype::csi_dsr()
{
    Trace("csi_dsr: %s\n", args_str().c_str());
    switch (opt_arg(0, 0)) {
    case 6: { /* report cursor position */
        char buf[32];
        update_offsets();
        llong vis_lines = visible_lines();
        llong col = cur_col + 1;
        llong row = cur_row - (lines.size() - vis_lines) + 1;
        row = std::max(1ll, std::min(row, vis_lines));
        int len = snprintf(buf, sizeof(buf), "\x1b[%llu;%lluR", row, col);
        write(buf, len);
        break;
    }
    default:
        Debug("csi_dsr: %s\n", args_str().c_str());
        break;
    }
}

void tty_teletype::csi(uint c)
{
    Trace("csi: %s %s\n",
        args_str().c_str(), char_str(c).c_str());

    switch (c) {
    case '@': /* insert blanks */
    {
        tty_line &line = lines[cur_row];
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

void tty_teletype::absorb(uint c)
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

ssize_t tty_teletype::io()
{
    struct pollfd pfds[1];
    ssize_t len;
    int ret;

    int do_poll_in  = -(in_buf.size() - in_end > 0);
    int do_poll_out = -(out_end - out_start > 0);

    pfds[0].fd = fd;
    pfds[0].events = (do_poll_in & POLLIN) | (do_poll_out & POLLOUT);
    ret = poll(pfds, array_size(pfds), io_poll_timeout);

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
            if ((len = ::write(fd, &out_buf[out_start], count)) < 0) {
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

ssize_t tty_teletype::proc()
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

ssize_t tty_teletype::write(const char *buf, size_t len)
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

int tty_teletype::keycode_to_char(int key, int mods)
{
    // We convert simple Ctrl and Shift modifiers into ASCII
    if (key >= GLFW_KEY_SPACE && key <= GLFW_KEY_EQUAL) {
        if (mods == 0) {
            return key - GLFW_KEY_SPACE + ' ';
        }
    }
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        // convert Ctrl+<Key> into its ASCII control code
        if (mods == GLFW_MOD_CONTROL) {
            return key - GLFW_KEY_A + 1;
        }
        // convert Shift <Key> into ASCII
        if (mods == GLFW_MOD_SHIFT) {
            return key - GLFW_KEY_A + 'A';
        }
        // convert plain <Key> into ASCII
        if (mods == 0) {
            return key - GLFW_KEY_A + 'a';
        }
    }
    if (key >= GLFW_KEY_LEFT_BRACKET && key < GLFW_KEY_GRAVE_ACCENT) {
        // convert plain <Key> into ASCII
        if (mods == GLFW_MOD_SHIFT) {
            return key - GLFW_KEY_LEFT_BRACKET + '{';
        }
        if (mods == 0) {
            return key - GLFW_KEY_LEFT_BRACKET + '[';
        }
    }
    // convert Shift <Key> for miscellaneous characters
    if (mods == GLFW_MOD_SHIFT) {
        switch (key) {
        case GLFW_KEY_0:          /* ' */ return ')';
        case GLFW_KEY_1:          /* ' */ return '!';
        case GLFW_KEY_2:          /* ' */ return '@';
        case GLFW_KEY_3:          /* ' */ return '#';
        case GLFW_KEY_4:          /* ' */ return '$';
        case GLFW_KEY_5:          /* ' */ return '%';
        case GLFW_KEY_6:          /* ' */ return '^';
        case GLFW_KEY_7:          /* ' */ return '&';
        case GLFW_KEY_8:          /* ' */ return '*';
        case GLFW_KEY_9:          /* ' */ return '(';
        case GLFW_KEY_APOSTROPHE: /* ' */ return '"';
        case GLFW_KEY_COMMA:      /* , */ return '<';
        case GLFW_KEY_MINUS:      /* - */ return '_';
        case GLFW_KEY_PERIOD:     /* . */ return '>';
        case GLFW_KEY_SLASH:      /* / */ return '?';
        case GLFW_KEY_SEMICOLON:  /* ; */ return ':';
        case GLFW_KEY_EQUAL:      /* = */ return '+';
        case GLFW_KEY_GRAVE_ACCENT: /* ` */ return '~';
        }
    }
    return 0;
}

void tty_teletype::keyboard(int key, int scancode, int action, int mods)
{
    int c;
    switch (action) {
    case GLFW_PRESS:
        if ((flags & tty_flag_DECCKM) > 0) {
            switch (key) {
            case GLFW_KEY_ESCAPE: send(tty_char_ESC); break;
            case GLFW_KEY_ENTER: send(tty_char_LF); break;
            case GLFW_KEY_TAB: send(tty_char_HT); break;
            case GLFW_KEY_BACKSPACE: send(tty_char_BS); break;
            case GLFW_KEY_INSERT: write("\x1b[2~", 4); break; // ?
            case GLFW_KEY_DELETE: write("\x1b[3~", 4); break; // ?
            case GLFW_KEY_PAGE_UP: write("\x1b[5~", 4); break; // ?
            case GLFW_KEY_PAGE_DOWN: write("\x1b[6~", 4); break; // ?
            case GLFW_KEY_UP: write("\x1bOA", 3); break;
            case GLFW_KEY_DOWN: write("\x1bOB", 3); break;
            case GLFW_KEY_RIGHT: write("\x1bOC", 3); break;
            case GLFW_KEY_LEFT: write("\x1bOD", 3); break;
            case GLFW_KEY_HOME: write("\x1bOH", 3); break;
            case GLFW_KEY_END: write("\x1bOF", 3); break;
            case GLFW_KEY_F1: write("\x1bOP", 3); break;
            case GLFW_KEY_F2: write("\x1bOQ", 3); break;
            case GLFW_KEY_F3: write("\x1bOR", 3); break;
            case GLFW_KEY_F4: write("\x1bOS", 3); break;
            case GLFW_KEY_F5: write("\x1b[15~", 5); break; // ?
            case GLFW_KEY_F6: write("\x1b[17~", 5); break; // ?
            case GLFW_KEY_F7: write("\x1b[18~", 5); break; // ?
            case GLFW_KEY_F8: write("\x1b[19~", 5); break; // ?
            case GLFW_KEY_F9: write("\x1b[20~", 5); break; // ?
            case GLFW_KEY_F10: write("\x1b[21~", 5); break; // ?
            case GLFW_KEY_F11: write("\x1b[23~", 5); break; // ?
            case GLFW_KEY_F12: write("\x1b[24~", 5); break; // ?
            default:
                c = keycode_to_char(key, mods);
                if (c) send(c);
                break;
            }
        } else {
            switch (key) {
            case GLFW_KEY_ESCAPE: send(tty_char_ESC); break;
            case GLFW_KEY_ENTER: send(tty_char_LF); break;
            case GLFW_KEY_TAB: send(tty_char_HT); break;
            case GLFW_KEY_BACKSPACE: send(tty_char_BS); break;
            case GLFW_KEY_INSERT: write("\x1b[2~", 4); break;
            case GLFW_KEY_DELETE: write("\x1b[3~", 4); break;
            case GLFW_KEY_PAGE_UP: write("\x1b[5~", 4); break;
            case GLFW_KEY_PAGE_DOWN: write("\x1b[6~", 4); break;
            case GLFW_KEY_UP: write("\x1b[A", 3); break;
            case GLFW_KEY_DOWN: write("\x1b[B", 3); break;
            case GLFW_KEY_RIGHT: write("\x1b[C", 3); break;
            case GLFW_KEY_LEFT: write("\x1b[D", 3); break;
            case GLFW_KEY_HOME: write("\x1b[H", 3); break;
            case GLFW_KEY_END: write("\x1b[F", 3); break;
            case GLFW_KEY_F1: write("\x1b[11~", 5); break;
            case GLFW_KEY_F2: write("\x1b[12~", 5); break;
            case GLFW_KEY_F3: write("\x1b[13~", 5); break;
            case GLFW_KEY_F4: write("\x1b[14~", 5); break;
            case GLFW_KEY_F5: write("\x1b[15~", 5); break;
            case GLFW_KEY_F6: write("\x1b[17~", 5); break;
            case GLFW_KEY_F7: write("\x1b[18~", 5); break;
            case GLFW_KEY_F8: write("\x1b[19~", 5); break;
            case GLFW_KEY_F9: write("\x1b[20~", 5); break;
            case GLFW_KEY_F10: write("\x1b[21~", 5); break;
            case GLFW_KEY_F11: write("\x1b[23~", 5); break;
            case GLFW_KEY_F12: write("\x1b[24~", 5); break;
            default:
                c = keycode_to_char(key, mods);
                if (c) send(c);
                break;
            }
        }
        break;
    }
}
