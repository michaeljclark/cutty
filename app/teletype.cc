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

tty_teletype* tty_new()
{
    tty_teletype* t = new tty_teletype{};

    t->state =  tty_state_normal;
    t->flags =  tty_flag_DECAWM | tty_flag_DECTCEM;
    t->charset = tty_charset_utf8;

    t->in_buf.resize(io_buffer_size);
    t->in_start = 0;
    t->in_end = 0;

    t->out_buf.resize(io_buffer_size);
    t->out_start = 0;
    t->out_end = 0;

    t->lines.push_back(tty_line{});
    t->ws = { 0, 0, 0, 0};
    t->min_row = 0;
    t->cur_row = 0;
    t->cur_col = 0;

    t->needs_update = 1;

    return t;
}

void tty_close(tty_teletype *t)
{
    close(t->fd);
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

static std::string esc_args(tty_teletype *t)
{
    std::string s;
    for (size_t i = 0; i < t->argc; i++) {
        if (i > 0) s.append(";");
        s.append(std::to_string(t->argv[i]));
    }
    return s;
}

static int opt_arg(tty_teletype *t, int arg, int opt)
{
    return arg < t->argc ? t->argv[arg] : opt;
}

static void tty_send(tty_teletype *t, uint c)
{
    Trace("tty_send: %s\n", char_str(c).c_str());
    char b = (char)c;
    tty_write(t, &b, 1);
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

void tty_update_offsets(tty_teletype *t)
{
    bool wrap_enabled = (t->flags & tty_flag_DECAWM) > 0;
    size_t cols = t->ws.vis_cols;
    size_t vlstart, vl;

    if (!wrap_enabled) {
        t->voffsets.clear();
        t->loffsets.clear();
        return;
    }

    /* recompute line offsets incrementally from min_row */
    if (t->min_row == 0) {
        vlstart = 0;
    } else {
        auto &loff = t->loffsets[t->min_row - 1];
        vlstart = loff.vline + loff.count;
    }

    /* count lines with wrap incrementally from min row */
    vl = vlstart;
    for (size_t k = t->min_row; k < t->lines.size(); k++) {
        size_t cell_count = t->lines[k].count();
        size_t wrap_count = cell_count == 0 ? 1
            : wrap_enabled ? (cell_count + cols - 1) / cols : 1;
        vl += wrap_count;
    }

    /* write out indices incrementally from min row */
    t->voffsets.resize(vl);
    t->loffsets.resize(t->lines.size());
    vl = vlstart;
    for (size_t k = t->min_row; k < t->lines.size(); k++) {
        size_t cell_count = t->lines[k].count();
        size_t wrap_count = cell_count == 0 ? 1
            : wrap_enabled ? (cell_count + cols - 1) / cols : 1;
        t->loffsets[k] = { vl, wrap_count };
        for (size_t j = 0; j < wrap_count; j++, vl++) {
            t->voffsets[vl] = { k, j * cols };
        }
    }

    /* set min_row to cur_row */
    t->min_row = t->cur_row;
}

tty_line_voff tty_visible_to_logical(tty_teletype *t, llong vline)
{
    bool wrap_enabled = (t->flags & tty_flag_DECAWM) > 0;

    if (wrap_enabled) {
        return t->voffsets[vline];
    } else {
        return tty_line_voff{ (size_t)vline, 0 };
    }
}

tty_line_loff tty_logical_to_visible(tty_teletype *t, llong lline)
{
    bool wrap_enabled = (t->flags & tty_flag_DECAWM) > 0;

    if (wrap_enabled) {
        return t->loffsets[lline];
    } else {
        return tty_line_loff{ (size_t)lline, 0 };
    }
}

llong tty_total_rows(tty_teletype *t)
{
    bool wrap_enabled = (t->flags & tty_flag_DECAWM) > 0;

    return wrap_enabled ? t->voffsets.size() : t->lines.size();
}

llong tty_total_lines(tty_teletype *t)
{
    return t->lines.size();
}

llong tty_visible_rows(tty_teletype *t)
{
    return t->ws.vis_rows;
}

llong tty_visible_lines(tty_teletype *t)
{
    bool wrap_enabled = (t->flags & tty_flag_DECAWM) > 0;
    llong rows = t->ws.vis_rows;

    if (wrap_enabled) {
        /*
         * calculate the number of visible lines so that we can calculate
         * absolute position while considering dynamically wrapped lines.
         */
        llong total_rows = wrap_enabled ? t->voffsets.size() : t->lines.size();
        llong wrapped_rows = 0;
        for (llong j = total_rows - 1, l = 0; l < rows && j >= 0; j--, l++)
        {
            if (j >= total_rows) continue;
            if (tty_visible_to_logical(t, j).offset > 0) wrapped_rows++;
        }
        return rows - wrapped_rows;
    } else {
        return rows;
    }
}

tty_winsize tty_get_winsize(tty_teletype *t)
{
    return t->ws;
}

void tty_set_winsize(tty_teletype *t, tty_winsize d)
{
    if (t->ws != d) {
        t->ws = d;
        t->min_row = 0;
    }
}

static void tty_set_row(tty_teletype *t, llong row)
{
    if (row < 0) row = 0;
    if (row != t->cur_row) {
        t->lines[t->cur_row].pack();
        if (row >= t->lines.size()) {
            t->lines.resize(row + 1);
        } else {
            t->lines[row].unpack();
        }
        t->cur_row = row;
        t->min_row = std::min(t->min_row, row);
    }
}

static void tty_set_col(tty_teletype *t, llong col)
{
    if (col < 0) col = 0;
    if (col != t->cur_col) {
        t->cur_col = col;
    }
}

static void tty_move_abs(tty_teletype *t, llong row, llong col)
{
    Trace("tty_move_abs: %lld %lld\n", row, col);
    if (row != -1) {
        tty_update_offsets(t);
        llong visible_lines = tty_visible_lines(t);
        size_t new_row = std::max(0ll, std::min((llong)t->lines.size() - 1,
                        (llong)t->lines.size() - visible_lines + row - 1));
        tty_set_row(t, new_row);
    }
    if (col != -1) {
        tty_set_col(t, std::max(0ll, col - 1));
    }
}

static void tty_move_rel(tty_teletype *t, llong row, llong col)
{
    Trace("tty_move_rel: %lld %lld\n", row, col);
    llong new_row = t->cur_row + row;
    llong new_col = col == tty_col_home ? 0 : t->cur_col + col;
    if (new_row < 0) new_row = 0;
    if (new_col < 0) new_col = 0;
    tty_set_row(t, new_row);
    tty_set_col(t, new_col);
}

static void tty_scroll_region(tty_teletype *t, llong line0, llong line1)
{
    Trace("tty_scroll_region: %lld %lld\n", line0, line1);
    t->top_marg = line0;
    t->bot_marg = line1;
}

static void tty_reset_style(tty_teletype *t)
{
    t->tmpl.flags = 0;
    t->tmpl.fg = tty_cell_color_fg_dfl;
    t->tmpl.bg = tty_cell_color_bg_dfl;
}

void tty_set_fd(tty_teletype *t, int fd)
{
    t->fd = fd;
}

void tty_reset(tty_teletype *t)
{
    Trace("tty_reset\n");
    tty_move_abs(t, 1, 1);
    tty_reset_style(t);
}

static void tty_erase_screen(tty_teletype *t, uint arg)
{
    Trace("tty_erase_screen: %d\n", arg);
    switch (arg) {
    case tty_clear_end:
        for (size_t row = t->cur_row; row < t->lines.size(); row++) {
            t->lines[row].clear();
        }
        break;
    case tty_clear_start:
        for (ssize_t row = t->cur_row; row < t->lines.size(); row--) {
            t->lines[row].clear();
        }
        break;
    case tty_clear_all:
        for (size_t row = 0; row < t->lines.size(); row++) {
            t->lines[row].clear();
        }
        tty_move_abs(t, 1, 1);
        break;
    }
}

static void tty_erase_line(tty_teletype *t, uint arg)
{
    Trace("tty_erase_line: %d\n", arg);
    switch (arg) {
    case tty_clear_end:
        if (t->cur_col < t->lines[t->cur_row].cells.size()) {
            t->lines[t->cur_row].cells.resize(t->cur_col);
        }
        break;
    case tty_clear_start:
        if (t->cur_col < t->lines[t->cur_row].cells.size()) {
            tty_cell cell = t->tmpl;
            cell.codepoint = ' ';
            for (size_t col = 0; col < t->cur_col; col++) {
                t->lines[t->cur_row].cells[col] = cell;
            }
        }
        break;
    case tty_clear_all:
        t->lines[t->cur_row].cells.resize(0);
        break;
    }
}

static void tty_insert_lines(tty_teletype *t, uint arg)
{
    Trace("tty_insert_lines: %d\n", arg);
    if (arg == 0) return;
    // consider line editing mode: *following*, or preceding
    // consider bottom margin for following mode. add bounds checks
    llong top = t->top_marg == 0 ? 1           : t->top_marg;
    llong bot = t->bot_marg == 0 ? t->ws.vis_rows : t->bot_marg;
    llong scrolloff = bot < t->ws.vis_rows ? t->ws.vis_rows - bot : 0;
    t->lines[t->cur_row].pack();
    for (uint i = 0; i < arg; i++) {
        t->lines.insert(t->lines.begin() + t->cur_row, tty_line{});
        t->lines.erase(t->lines.end() - 1 - scrolloff);
    }
    t->lines[t->cur_row].unpack();
    t->cur_col = 0;
}

static void tty_delete_lines(tty_teletype *t, uint arg)
{
    Trace("tty_delete_lines: %d\n", arg);
    if (arg == 0) return;
    // consider line editing mode: *following*, or preceding
    // consider bottom margin for following mode. add bounds checks
    llong top = t->top_marg == 0 ? 1           : t->top_marg;
    llong bot = t->bot_marg == 0 ? t->ws.vis_rows : t->bot_marg;
    llong scrolloff = bot < t->ws.vis_rows ? t->ws.vis_rows - bot : 0;
    t->lines[t->cur_row].pack();
    for (uint i = 0; i < arg; i++) {
        if (t->cur_row < t->lines.size()) {
            t->lines.erase(t->lines.begin() + t->cur_row);
            t->lines.insert(t->lines.end() - scrolloff, tty_line{});
        }
    }
    t->lines[t->cur_row].unpack();
    t->cur_col = 0;
}

static void tty_delete_chars(tty_teletype *t, uint arg)
{
    Trace("tty_delete_chars: %d\n", arg);
    for (size_t i = 0; i < arg; i++) {
        if (t->cur_col < t->lines[t->cur_row].cells.size()) {
            t->lines[t->cur_row].cells.erase(
                t->lines[t->cur_row].cells.begin() + t->cur_col
            );
        }
    }
}

static void tty_BEL(tty_teletype *t)
{
    Trace("tty_BEL: unimplemented\n");
}

static void tty_BS(tty_teletype *t)
{
    Trace("tty_BS\n");
    if (t->cur_col > 0) t->cur_col--;
}

static void tty_HT(tty_teletype *t)
{
    Trace("tty_HT\n");
    t->cur_col = (t->cur_col + 8) & ~7;
}

static void tty_LF(tty_teletype *t)
{
    Trace("tty_LF\n");
    tty_set_row(t, t->cur_row + 1);
}

static void tty_CR(tty_teletype *t)
{
    Trace("tty_CR\n");
    t->cur_col = 0;
}

static void tty_bare(tty_teletype *t, uint c)
{
    Trace("tty_bare: %s\n", char_str(c).c_str());
    if (t->cur_col >= t->lines[t->cur_row].cells.size()) {
        t->lines[t->cur_row].cells.resize(t->cur_col + 1);
    }
    t->lines[t->cur_row].cells[t->cur_col++] =
        tty_cell{c, t->tmpl.flags, t->tmpl.fg, t->tmpl.bg};
}

static void tty_ctrl(tty_teletype *t, uint c)
{
    Trace("tty_ctrl: %s\n", char_str(c).c_str());
    switch (c) {
    case tty_char_BEL: tty_BEL(t); break;
    case tty_char_BS: tty_BS(t); break;
    case tty_char_HT: tty_HT(t); break;
    case tty_char_LF: tty_LF(t); break;
    case tty_char_CR: tty_CR(t); break;
    default:
        Debug("tty_ctrl: unhandled control character %s\n",
            char_str(c).c_str());
    }
}

static void tty_xtwinops(tty_teletype *t)
{
    Debug("tty_xtwinops: %s unimplemented\n", esc_args(t).c_str());
}

static void tty_charset(tty_teletype *t, uint cmd, uint set)
{
    Debug("tty_charset: %c %c unimplemented\n", cmd, set);
}

static void tty_osc(tty_teletype *t, uint c)
{
    Debug("tty_osc: %s %s unimplemented\n",
        esc_args(t).c_str(), char_str(c).c_str());
    if (t->argc == 1 && t->argv[0] == 555) {
        Debug("tty_osc: screen-capture\n");
        t->needs_capture = 1;
    }
}

static void tty_osc_string(tty_teletype *t, uint c)
{
    Debug("tty_osc_string: %s %s \"%s\" unimplemented\n",
        esc_args(t).c_str(), char_str(c).c_str(), t->osc_string.c_str());
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

static void tty_csi_private_mode(tty_teletype *t, uint code, uint set)
{
    tty_private_mode_rec *rec = tty_lookup_private_mode_rec(code);
    if (rec == NULL) {
        Debug("tty_csi_private_mode: %s flag %d: unimplemented\n",
            set ? "set" : "clear", code);
    } else {
        Debug("tty_csi_private_mode: %s flag %d: %s /* %s */\n",
            set ? "set" : "clear", code, rec->name, rec->desc);
        if (set) {
            t->flags |= rec->flag;
        } else {
            t->flags &= ~rec->flag;
        }
    }
}

static void tty_csi_dec(tty_teletype *t, uint c)
{
    switch (c) {
    case 'l': tty_csi_private_mode(t, opt_arg(t, 0, 0), 0); break;
    case 'h': tty_csi_private_mode(t, opt_arg(t, 0, 0), 1); break;
    default:
        Debug("tty_csi_dec: %s %s unimplemented\n",
            char_str(c).c_str(), esc_args(t).c_str());
        break;
    }
}

static void tty_csi_dec2(tty_teletype *t, uint c)
{
    Debug("tty_csi_dec2: %s %s unimplemented\n",
        char_str(c).c_str(), esc_args(t).c_str());
}

static void tty_csi_dec3(tty_teletype *t, uint c)
{
    Debug("tty_csi_dec3: %s %s unimplemented\n",
        char_str(c).c_str(), esc_args(t).c_str());
}

static void zterm_csi_dsr(tty_teletype *t)
{
    Trace("zterm_csi_dsr: %s\n", esc_args(t).c_str());
    switch (opt_arg(t, 0, 0)) {
    case 6: { /* report cursor position */
        char buf[32];
        tty_update_offsets(t);
        llong visible_lines = tty_visible_lines(t);
        llong col = t->cur_col + 1;
        llong row = t->cur_row - (t->lines.size() - visible_lines) + 1;
        row = std::max(1ll, std::min(row, visible_lines));
        int len = snprintf(buf, sizeof(buf), "\x1b[%llu;%lluR", row, col);
        tty_write(t, buf, len);
        break;
    }
    default:
        Debug("zterm_csi_dsr: %s\n", esc_args(t).c_str());
        break;
    }
}

static void tty_csi(tty_teletype *t, uint c)
{
    Trace("tty_csi: %s %s\n",
        esc_args(t).c_str(), char_str(c).c_str());

    switch (c) {
    case '@': /* insert blanks */
    {
        tty_line &line = t->lines[t->cur_row];
        int n = opt_arg(t, 0, 0);
        if (t->cur_col < line.cells.size()) {
            tty_cell cell = t->tmpl;
            cell.codepoint = ' ';
            for (size_t i = 0; i < n; i++) {
                line.cells.insert(line.cells.begin() + t->cur_col, cell);
            }
        }
        break;
    }
    case 'A': /* move up */
        tty_move_rel(t, -opt_arg(t, 0, 1), 0);
        break;
    case 'B': /* move down */
        tty_move_rel(t, opt_arg(t, 0, 1),  0);
        break;
    case 'C': /* move right */
        tty_move_rel(t, 0,  opt_arg(t, 0, 1));
        break;
    case 'D': /* move left */
        tty_move_rel(t, 0, -opt_arg(t, 0, 1));
        break;
    case 'E': /* move next line */
        tty_move_rel(t, opt_arg(t, 0, 1), tty_col_home);
        break;
    case 'F': /* move prev line */
        tty_move_rel(t, -opt_arg(t, 0, 1), tty_col_home);
        break;
    case 'G': /* move to {col} */
        tty_move_abs(t, -1, opt_arg(t, 0, 1));
        break;
    case 'H': /* move to {line};{col} */
        tty_move_abs(t, opt_arg(t, 0, 1), opt_arg(t, 1, 1));
        break;
    case 'J': /* erase lines {0=to-end,1=from-start,2=all} */
        switch (opt_arg(t, 0, 0)) {
        case 0: tty_erase_screen(t, tty_clear_end); break;
        case 1: tty_erase_screen(t, tty_clear_start); break;
        case 2: tty_erase_screen(t, tty_clear_all); break;
        default:
            Debug("tty_csi: CSI J: invalid arg: %d\n", opt_arg(t, 0, 0));
            break;
        }
        break;
    case 'K': /* erase chars {0=to-end,1=from-start,2=all} */
        switch (opt_arg(t, 0, 0)) {
        case 0: tty_erase_line(t, tty_clear_end); break;
        case 1: tty_erase_line(t, tty_clear_start); break;
        case 2: tty_erase_line(t, tty_clear_all); break;
        default:
            Debug("tty_csi: CSI K: invalid arg: %d\n", opt_arg(t, 0, 0));
            break;
        }
        break;
    case 'L': /* insert lines */
        tty_insert_lines(t, opt_arg(t, 0, 1));
        break;
    case 'M': /* delete lines */
        tty_delete_lines(t, opt_arg(t, 0, 1));
        break;
    case 'P': /* delete characters */
        tty_delete_chars(t, opt_arg(t, 0, 1));
        break;
    case 'd': /* move to {line};1 absolute */
        tty_move_abs(t, opt_arg(t, 0, 1), -1);
        break;
    case 'e': /* move to {line};1 relative */
        tty_move_rel(t, opt_arg(t, 0, 1), 0);
        break;
    case 'f': /* move to {line};{col} absolute */
        tty_move_abs(t, opt_arg(t, 0, 1), opt_arg(t, 1, 1));
        break;
    case 'm': /* color and formatting */
        if (t->argc == 0) {
            tty_reset_style(t);
            break;
        }
        for (size_t i = 0; i < t->argc; i++) {
            uint code = t->argv[i];
            switch (code) {
            case 0: tty_reset_style(t); break;
            case 1: t->tmpl.flags |= tty_cell_bold; break;
            case 2: t->tmpl.flags |= tty_cell_faint; break;
            case 3: t->tmpl.flags |= tty_cell_italic; break;
            case 4: t->tmpl.flags |= tty_cell_underline; break;
            case 5: t->tmpl.flags |= tty_cell_blink; break;
            case 6: t->tmpl.flags |= tty_cell_rblink; break;
            case 7: t->tmpl.flags |= tty_cell_inverse; break;
            case 8: t->tmpl.flags |= tty_cell_hidden; break;
            case 9: t->tmpl.flags |= tty_cell_strikeout; break;
            /* case 10 through 19 custom fonts */
            case 20: t->tmpl.flags |= tty_cell_fraktur; break;
            case 21: t->tmpl.flags |= tty_cell_dunderline; break;
            case 22: t->tmpl.flags &= ~(tty_cell_bold | tty_cell_faint); break;
            case 23: t->tmpl.flags &= ~(tty_cell_italic | tty_cell_fraktur); break;
            case 24: t->tmpl.flags &= ~(tty_cell_underline | tty_cell_dunderline); break;
            case 25: t->tmpl.flags &= ~tty_cell_blink; break;
            case 26: t->tmpl.flags &= ~tty_cell_rblink; break;
            case 27: t->tmpl.flags &= ~tty_cell_inverse; break;
            case 28: t->tmpl.flags &= ~tty_cell_hidden; break;
            case 29: t->tmpl.flags &= ~tty_cell_strikeout; break;
            case 30: t->tmpl.fg = tty_cell_color_nr_black; break;
            case 31: t->tmpl.fg = tty_cell_color_nr_red; break;
            case 32: t->tmpl.fg = tty_cell_color_nr_green; break;
            case 33: t->tmpl.fg = tty_cell_color_nr_yellow; break;
            case 34: t->tmpl.fg = tty_cell_color_nr_blue; break;
            case 35: t->tmpl.fg = tty_cell_color_nr_magenta; break;
            case 36: t->tmpl.fg = tty_cell_color_nr_cyan; break;
            case 37: t->tmpl.fg = tty_cell_color_nr_white; break;
            case 38:
                if (i + 2 < t->argc && t->argv[i+1] == 5) {
                    t->tmpl.fg = tty_colors_256[t->argv[i+2]];
                    i += 2;
                }
                else if (i + 4 < t->argc && t->argv[i+1] == 2) {
                    uint r = tty_colors_256[t->argv[i+2]];
                    uint g = tty_colors_256[t->argv[i+3]];
                    uint b = tty_colors_256[t->argv[i+4]];
                    t->tmpl.fg = (((uint)r << 0) | ((uint)g << 8) |
                                  ((uint)b << 16) | ((uint)0xff << 24));
                    i += 4;
                }
                break;
            case 39: t->tmpl.fg = tty_cell_color_fg_dfl; break;
            case 40: t->tmpl.bg = tty_cell_color_nr_black; break;
            case 41: t->tmpl.bg = tty_cell_color_nr_red; break;
            case 42: t->tmpl.bg = tty_cell_color_nr_green; break;
            case 43: t->tmpl.bg = tty_cell_color_nr_yellow; break;
            case 44: t->tmpl.bg = tty_cell_color_nr_blue; break;
            case 45: t->tmpl.bg = tty_cell_color_nr_magenta; break;
            case 46: t->tmpl.bg = tty_cell_color_nr_cyan; break;
            case 47: t->tmpl.bg = tty_cell_color_nr_white; break;
            case 48:
                if (i + 2 < t->argc && t->argv[i+1] == 5) {
                    t->tmpl.bg = tty_colors_256[t->argv[i+2]];
                    i += 2;
                }
                else if (i + 4 < t->argc && t->argv[i+1] == 2) {
                    uint r = tty_colors_256[t->argv[i+2]];
                    uint g = tty_colors_256[t->argv[i+3]];
                    uint b = tty_colors_256[t->argv[i+4]];
                    t->tmpl.bg = (((uint)r << 0) | ((uint)g << 8) |
                                  ((uint)b << 16) | ((uint)0xff << 24));
                    i += 4;
                }
                break;
            case 49: t->tmpl.bg = tty_cell_color_bg_dfl; break;
            case 90: t->tmpl.fg = tty_cell_color_br_black; break;
            case 91: t->tmpl.fg = tty_cell_color_br_red; break;
            case 92: t->tmpl.fg = tty_cell_color_br_green; break;
            case 93: t->tmpl.fg = tty_cell_color_br_yellow; break;
            case 94: t->tmpl.fg = tty_cell_color_br_blue; break;
            case 95: t->tmpl.fg = tty_cell_color_br_magenta; break;
            case 96: t->tmpl.fg = tty_cell_color_br_cyan; break;
            case 97: t->tmpl.fg = tty_cell_color_br_white; break;
            case 100: t->tmpl.bg = tty_cell_color_br_black; break;
            case 101: t->tmpl.bg = tty_cell_color_br_red; break;
            case 102: t->tmpl.bg = tty_cell_color_br_green; break;
            case 103: t->tmpl.bg = tty_cell_color_br_yellow; break;
            case 104: t->tmpl.bg = tty_cell_color_br_blue; break;
            case 105: t->tmpl.bg = tty_cell_color_br_magenta; break;
            case 106: t->tmpl.bg = tty_cell_color_br_cyan; break;
            case 107: t->tmpl.bg = tty_cell_color_br_white; break;
                break;
            default:
                break;
            }
        }
        break;
    case 'n': /* device status report */
        zterm_csi_dsr(t);
        break;
    case 'r': /* set scrolling region {line-start};{line-end}*/
        tty_scroll_region(t, opt_arg(t, 0, 1), opt_arg(t, 1, 1));
        break;
    case 't': /* window manager hints */
        tty_xtwinops(t);
        break;
    }
}

static void tty_absorb(tty_teletype *t, uint c)
{
    Trace("tty_absorb: %s\n", char_str(c).c_str());
restart:
    switch (t->state) {
    case tty_state_normal:
        if ((c & 0xf8) == 0xf8) {
        } else if ((c & 0xf0) == 0xf0) {
            t->state = tty_state_utf4;
            t->code = c & 0x07;
        } else if ((c & 0xe0) == 0xe0) {
            t->state = tty_state_utf3;
            t->code = c & 0x0f;
        } else if ((c & 0xc0) == 0xc0) {
            t->state = tty_state_utf2;
            t->code = c & 0x1f;
        } else {
            if (c == tty_char_ESC) {
                t->state = tty_state_escape;
                t->argc = t->code = 0;
            } else if (c < 0x20) {
                tty_ctrl(t, c);
            } else {
                tty_bare(t, c);
            }
        }
        break;
    case tty_state_utf4:
        t->code = (t->code << 6) | (c & 0x3f);
        t->state = tty_state_utf3;
        break;
    case tty_state_utf3:
        t->code = (t->code << 6) | (c & 0x3f);
        t->state = tty_state_utf2;
        break;
    case tty_state_utf2:
        t->code = (t->code << 6) | (c & 0x3f);
        tty_bare(t, t->code);
        t->state = tty_state_normal;
        break;
    case tty_state_escape:
        switch (c) {
        case '[':
            t->state = tty_state_csi0;
            return;
        case ']':
            t->state = tty_state_osc0;
            return;
        case '(':
        case '*':
        case '+':
        case '-':
        case '.':
        case '/':
            t->code = c;
            t->state = tty_state_charset;
            return;
        case '=':
            Debug("tty_absorb: enter alternate keypad mode: unimplemented\n");
            t->state = tty_state_normal;
            return;
        case '>':
            Debug("tty_absorb: exit alternate keypad mode: unimplemented\n");
            t->state = tty_state_normal;
            return;
        default:
            Debug("tty_absorb: invalid ESC char '%c' (0x%02x)\n", c, c);
            t->state = tty_state_normal;
            break;
        }
        break;
    case tty_state_charset:
        tty_charset(t, t->code, c);
        t->state = tty_state_normal;
        break;
    case tty_state_csi0:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->state = tty_state_csi;
            goto restart;
        case '@': case 'A': case 'B': case 'C': case 'D':
        case 'E': case 'F': case 'G': case 'H': case 'I':
        case 'J': case 'K': case 'L': case 'M': case 'P':
        case 'd': case 'e': case 'f': case 'm': case 'n':
        case 'r': case 't':
            tty_csi(t, c);
            t->state = tty_state_normal;
            break;
        case '?': /* DEC */
            t->state = tty_state_csi_dec;
            break;
        case '>': /* DEC2 */
            t->state = tty_state_csi_dec2;
            break;
        case '=': /* DEC3 */
            t->state = tty_state_csi_dec3;
            break;
        default:
            Debug("tty_absorb: invalid CSI char '%c' (0x%02x)\n", c, c);
            t->state = tty_state_normal;
            goto restart;
        }
        break;
    case tty_state_csi:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->code = t->code * 10 + (c - '0');
            break;
        case ';':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("tty_absorb: CSI too many args, ignoring %d\n",
                    t->code);
            }
            break;
        case '@': case 'A': case 'B': case 'C': case 'D':
        case 'E': case 'F': case 'G': case 'H': case 'I':
        case 'J': case 'K': case 'L': case 'M': case 'P':
        case 'd': case 'e': case 'f': case 'm': case 'n':
        case 'r': case 't':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("tty_absorb: CSI too many args, ignoring %d\n",
                    t->code);
            }
            tty_csi(t, c);
            t->state = tty_state_normal;
            break;
        default:
            Debug("tty_absorb: invalid CSI char '%c' (0x%02x)\n", c, c);
            t->state = tty_state_normal;
            break;
        }
        break;
    case tty_state_csi_dec:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->code = t->code * 10 + (c - '0');
            break;
        case ';':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("tty_absorb: CSI ? too many args, ignoring %d\n",
                    t->code);
            }
            break;
        case 'c': case 'h': case 'i': case 'l': case 'n':
        case 'r': case 's': case 'S': case 'J': case 'K':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("tty_absorb: CSI ? too many args, ignoring %d\n",
                    t->code);
            }
            tty_csi_dec(t, c);
            t->state = tty_state_normal;
            break;
        default:
            Debug("tty_absorb: invalid CSI ? char '%c' (0x%02x)\n", c, c);
            t->state = tty_state_normal;
            break;
        }
        break;
    case tty_state_csi_dec2:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->code = t->code * 10 + (c - '0');
            break;
        case ';':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("tty_absorb: CSI > too many args, ignoring %d\n",
                    t->code);
            }
            tty_csi_dec2(t, c);
            t->state = tty_state_normal;
            break;
        case 'c': /* device report */
            Debug("tty_absorb: CSI > device report\n");
            t->state = tty_state_normal;
            break;
        default:
            Debug("tty_absorb: invalid CSI > char '%c' (0x%02x)\n", c, c);
            t->state = tty_state_normal;
            break;
        }
        break;
    case tty_state_csi_dec3:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->code = t->code * 10 + (c - '0');
            break;
        case ';':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("tty_absorb: CSI = too many args, ignoring %d\n",
                    t->code);
            }
            break;
        case 'c': /* device report */
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("tty_absorb: CSI = too many args, ignoring %d\n",
                    t->code);
            }
            tty_csi_dec3(t, c);
            t->state = tty_state_normal;
            break;
        default:
            Debug("tty_absorb: invalid CSI = char '%c' (0x%02x)\n", c, c);
            t->state = tty_state_normal;
            break;
        }
        break;
    case tty_state_osc0:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->state = tty_state_osc;
            goto restart;
        case tty_char_BEL:
            tty_osc(t, c);
            t->state = tty_state_normal;
            break;
        default:
            Debug("tty_absorb: invalid OSC char '%c' (0x%02x)\n", c, c);
            //t->state = tty_state_normal;
            break;
        }
        break;
    case tty_state_osc:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->code = t->code * 10 + (c - '0');
            break;
        case ';':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("tty_absorb: OSC too many args, ignoring %d\n",
                    t->code);
            }
            if (t->argc == 1 && t->argv[0] == 7) {
                t->state = tty_state_osc_string;
                t->osc_string.clear();
            }
            break;
        case tty_char_BEL:
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("tty_absorb: OSC too many args, ignoring %d\n",
                    t->code);
            }
            tty_osc(t, c);
            t->state = tty_state_normal;
            break;
        default:
            Debug("tty_absorb: invalid OSC char '%c' (0x%02x)\n", c, c);
            //t->state = tty_state_normal;
            break;
        }
        break;
    case tty_state_osc_string:
        if (c == tty_char_BEL) {
            tty_osc_string(t, c);
            t->state = tty_state_normal;
        } else {
            t->osc_string.append(std::string(1, c));            
        }
        break;
    }
    t->needs_update = 1;
}

ssize_t tty_io(tty_teletype *t)
{
    struct pollfd pfds[1];
    ssize_t len;
    int ret;

    int do_poll_in  = -(t->in_buf.size() - t->in_end > 0);
    int do_poll_out = -(t->out_end - t->out_start > 0);

    pfds[0].fd = t->fd;
    pfds[0].events = (do_poll_in & POLLIN) | (do_poll_out & POLLOUT);
    ret = poll(pfds, array_size(pfds), io_poll_timeout);

    if (pfds[0].revents & POLLOUT) {
        ssize_t count;
        if (t->out_start > t->out_end) {
            /* zero xxxxxxxx end ________ start <xxxxxx> limit */
            count = t->out_buf.size() - t->out_start;
        } else {
            /* zero ________ start <xxxxxx> end ________ limit */
            count = t->out_end - t->out_start;
        }
        if (count > 0) {
            if ((len = write(t->fd, &t->out_buf[t->out_start], count)) < 0) {
                Panic("write failed: %s\n", strerror(errno));
            }
            Debug("tty_io: wrote %zu bytes -> pty\n", len);
            t->out_start += len;
        }
        if (t->out_start == t->out_buf.size()) {
            /* zero xxxxxxxx end ________________ start limit */
            /* zero start xxxxxxxx end ________________ limit */
            t->out_start = 0;
        }
    }

    if (pfds[0].revents & POLLIN) {
        ssize_t count;
        if (t->in_start > t->in_end) {
            /* zero xxxxxxxx end <xxx---> start xxxxxxxx limit */
            count = t->in_start - t->in_end;
        } else {
            /* zero ________ start xxxxxxxx end <xxx---> limit */
            count = t->in_buf.size() - t->in_end;
        }
        if (count > 0) {
            if ((len = read(t->fd, &t->in_buf[t->in_end], count)) < 0) {
                Panic("read failed: %s\n", strerror(errno));
            }
            Debug("tty_io: read %zu bytes <- pty\n", len);
            t->in_end += len;
            if (len == 0) return -1; /* EOF */
        }
        if (t->in_start < t->in_end && t->in_end == t->in_buf.size()) {
            /* zero ________ start xxxxxxxxxxxxxxxx end limit */
            /* zero end ________ start xxxxxxxxxxxxxxxx limit */
            t->in_end = 0;
        }
    }

    return 0;
}

ssize_t tty_proc(tty_teletype *t)
{
    size_t count;

    if (t->in_start > t->in_end) {
        /* zero xxxxxxxx end ________ start xxxxxxxx limit */
        count = t->in_buf.size() - t->in_start;
    } else {
        /* zero ________ start xxxxxxxx end ________ limit */
        count = t->in_end - t->in_start;
    }
    for (size_t i = 0; i < count; i++) {
        tty_absorb(t, t->in_buf[t->in_start]);
        t->in_start++;
    }
    if (t->in_end < t->in_start && t->in_start == t->in_buf.size()) {
        /* zero xxxxxxxx end _________________ start limit */
        /* zero start xxxxxxxx end _________________ limit */
        t->in_start = 0;
    }
    if (count > 0) {
        Debug("tty_proc: processed %zu bytes of input\n", count);
    }
    return count;
}

ssize_t tty_write(tty_teletype *t, const char *buf, size_t len)
{
    ssize_t count, ncopy = 0;
    if (t->out_start > t->out_end) {
        /* zero xxxxxxxx end <xxx---> start xxxxxxxx limit */
        count = t->out_start - t->out_end;
    } else {
        /* zero ________ start xxxxxxxx end <xxx---> limit */
        count = t->out_buf.size() - t->out_end;
    }
    if (count > 0) {
        ncopy = len < count ? len : count;
        memcpy(&t->out_buf[t->out_end], buf, ncopy);
        Debug("tty_write: buffered %zu bytes of output\n", len);
        t->out_end += ncopy;
    }
    if (t->out_start < t->out_end && t->out_end == t->out_buf.size()) {
        /* zero ________ start xxxxxxxxxxxxxxxx end limit */
        /* zero end ________ start xxxxxxxxxxxxxxxx limit */
        t->out_end = 0;
    }
    return ncopy;
}

static int tty_keycode_to_char(int key, int mods)
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

void tty_keyboard(tty_teletype *t, int key, int scancode, int action, int mods)
{
    int c;
    switch (action) {
    case GLFW_PRESS:
        if ((t->flags & tty_flag_DECCKM) > 0) {
            switch (key) {
            case GLFW_KEY_ESCAPE: tty_send(t, tty_char_ESC); break;
            case GLFW_KEY_ENTER: tty_send(t, tty_char_LF); break;
            case GLFW_KEY_TAB: tty_send(t, tty_char_HT); break;
            case GLFW_KEY_BACKSPACE: tty_send(t, tty_char_BS); break;
            case GLFW_KEY_INSERT: tty_write(t, "\x1b[2~", 4); break; // ?
            case GLFW_KEY_DELETE: tty_write(t, "\x1b[3~", 4); break; // ?
            case GLFW_KEY_PAGE_UP: tty_write(t, "\x1b[5~", 4); break; // ?
            case GLFW_KEY_PAGE_DOWN: tty_write(t, "\x1b[6~", 4); break; // ?
            case GLFW_KEY_UP: tty_write(t, "\x1bOA", 3); break;
            case GLFW_KEY_DOWN: tty_write(t, "\x1bOB", 3); break;
            case GLFW_KEY_RIGHT: tty_write(t, "\x1bOC", 3); break;
            case GLFW_KEY_LEFT: tty_write(t, "\x1bOD", 3); break;
            case GLFW_KEY_HOME: tty_write(t, "\x1bOH", 3); break;
            case GLFW_KEY_END: tty_write(t, "\x1bOF", 3); break;
            case GLFW_KEY_F1: tty_write(t, "\x1bOP", 3); break;
            case GLFW_KEY_F2: tty_write(t, "\x1bOQ", 3); break;
            case GLFW_KEY_F3: tty_write(t, "\x1bOR", 3); break;
            case GLFW_KEY_F4: tty_write(t, "\x1bOS", 3); break;
            case GLFW_KEY_F5: tty_write(t, "\x1b[15~", 5); break; // ?
            case GLFW_KEY_F6: tty_write(t, "\x1b[17~", 5); break; // ?
            case GLFW_KEY_F7: tty_write(t, "\x1b[18~", 5); break; // ?
            case GLFW_KEY_F8: tty_write(t, "\x1b[19~", 5); break; // ?
            case GLFW_KEY_F9: tty_write(t, "\x1b[20~", 5); break; // ?
            case GLFW_KEY_F10: tty_write(t, "\x1b[21~", 5); break; // ?
            case GLFW_KEY_F11: tty_write(t, "\x1b[23~", 5); break; // ?
            case GLFW_KEY_F12: tty_write(t, "\x1b[24~", 5); break; // ?
            default:
                c = tty_keycode_to_char(key, mods);
                if (c) tty_send(t, c);
                break;
            }
        } else {
            switch (key) {
            case GLFW_KEY_ESCAPE: tty_send(t, tty_char_ESC); break;
            case GLFW_KEY_ENTER: tty_send(t, tty_char_LF); break;
            case GLFW_KEY_TAB: tty_send(t, tty_char_HT); break;
            case GLFW_KEY_BACKSPACE: tty_send(t, tty_char_BS); break;
            case GLFW_KEY_INSERT: tty_write(t, "\x1b[2~", 4); break;
            case GLFW_KEY_DELETE: tty_write(t, "\x1b[3~", 4); break;
            case GLFW_KEY_PAGE_UP: tty_write(t, "\x1b[5~", 4); break;
            case GLFW_KEY_PAGE_DOWN: tty_write(t, "\x1b[6~", 4); break;
            case GLFW_KEY_UP: tty_write(t, "\x1b[A", 3); break;
            case GLFW_KEY_DOWN: tty_write(t, "\x1b[B", 3); break;
            case GLFW_KEY_RIGHT: tty_write(t, "\x1b[C", 3); break;
            case GLFW_KEY_LEFT: tty_write(t, "\x1b[D", 3); break;
            case GLFW_KEY_HOME: tty_write(t, "\x1b[H", 3); break;
            case GLFW_KEY_END: tty_write(t, "\x1b[F", 3); break;
            case GLFW_KEY_F1: tty_write(t, "\x1b[11~", 5); break;
            case GLFW_KEY_F2: tty_write(t, "\x1b[12~", 5); break;
            case GLFW_KEY_F3: tty_write(t, "\x1b[13~", 5); break;
            case GLFW_KEY_F4: tty_write(t, "\x1b[14~", 5); break;
            case GLFW_KEY_F5: tty_write(t, "\x1b[15~", 5); break;
            case GLFW_KEY_F6: tty_write(t, "\x1b[17~", 5); break;
            case GLFW_KEY_F7: tty_write(t, "\x1b[18~", 5); break;
            case GLFW_KEY_F8: tty_write(t, "\x1b[19~", 5); break;
            case GLFW_KEY_F9: tty_write(t, "\x1b[20~", 5); break;
            case GLFW_KEY_F10: tty_write(t, "\x1b[21~", 5); break;
            case GLFW_KEY_F11: tty_write(t, "\x1b[23~", 5); break;
            case GLFW_KEY_F12: tty_write(t, "\x1b[24~", 5); break;
            default:
                c = tty_keycode_to_char(key, mods);
                if (c) tty_send(t, c);
                break;
            }
        }
        break;
    }
}
