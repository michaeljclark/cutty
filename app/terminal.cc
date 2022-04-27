#include <cstdlib>
#include <cerrno>
#include <cassert>

#include <time.h>
#include <poll.h>
#include <unistd.h>

#include "app.h"
#include "utf8.h"
#include "colors.h"
#include "terminal.h"
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

cu_term* cu_term_new()
{
    cu_term* t = new cu_term{};

    t->state =  cu_state_normal;
    t->flags =  cu_flag_DECAWM | cu_flag_DECTCEM;
    t->charset = cu_charset_utf8;

    t->in_buf.resize(io_buffer_size);
    t->in_start = 0;
    t->in_end = 0;

    t->out_buf.resize(io_buffer_size);
    t->out_start = 0;
    t->out_end = 0;

    t->lines.push_back(cu_line{});
    t->min_row = 0;
    t->cur_row = 0;
    t->cur_col = 0;

    t->needs_update = 1;

    return t;
}

void cu_term_close(cu_term *t)
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

static std::string esc_args(cu_term *t)
{
    std::string s;
    for (size_t i = 0; i < t->argc; i++) {
        if (i > 0) s.append(";");
        s.append(std::to_string(t->argv[i]));
    }
    return s;
}

static int opt_arg(cu_term *t, int arg, int opt)
{
    return arg < t->argc ? t->argv[arg] : opt;
}

static void cu_term_send(cu_term *t, uint c)
{
    Trace("cu_term_send: %s\n", char_str(c).c_str());
    char b = (char)c;
    cu_term_write(t, &b, 1);
}

/*
 * - unpacked lines: cells vector has one element for every character and
 *   each cell has a utf32 codepoint. style flags, foreground and background
 *   colors. the cell count for the line is in cells.size().
 * - packed lines: cells vector holds style changes. the codepoint element
 *   contains an offset into utf8_data and the cell count is in pcount.
 */

size_t cu_line::count() { return pcount > 0 ? pcount : cells.size(); }

bool cu_line::ispacked() { return pcount > 0; }

void cu_line::pack()
{
    if (pcount > 0) return;

    std::vector<cu_cell> &ucells = cells;
    std::vector<cu_cell> pcells;

    utf8.clear();

    cu_cell t = { (uint)-1 };
    for (size_t i = 0; i < ucells.size(); i++) {
        cu_cell s = ucells[i];
        if (s.flags != t.flags || s.fg != t.fg || s.bg != t.bg) {
            t = cu_cell{(uint)utf8.size(), s.flags, s.fg, s.bg};
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

void cu_line::unpack()
{
    if (pcount == 0) return;

    std::vector<cu_cell> &pcells = cells;
    std::vector<cu_cell> ucells;

    cu_cell t = { 0 };
    size_t o = 0, q = 0, l = utf8.size();
    while (o < l) {
        if (q < pcells.size() && pcells[q].codepoint == o) {
            t = pcells[q++];
        }
        utf32_code v = utf8_to_utf32_code(&utf8[o]);
        ucells.push_back(cu_cell{(uint)v.code, t.flags, t.fg, t.bg});
        o += v.len;
    }

    pcount = 0;
    cells = ucells;
    utf8.clear();
}

void cu_line::clear()
{
    pcount = 0;
    cells.clear();
    utf8.clear();
    cells.shrink_to_fit();
    utf8.shrink_to_fit();
}

void cu_term_update_offsets(cu_term *t)
{
    size_t cols = t->vis_cols;
    bool wrap_enabled = (t->flags & cu_flag_DECAWM) > 0;
    size_t vlstart, vl;

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

void cu_term_set_dim(cu_term *t, cu_winsize d)
{
    t->vis_lines = d.vis_lines;
    t->vis_rows = d.vis_rows;
    t->vis_cols = d.vis_cols;
    t->min_row = 0;
}

static void cu_term_set_row(cu_term *t, llong row)
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

static void cu_term_set_col(cu_term *t, llong col)
{
    if (col < 0) col = 0;
    if (col != t->cur_col) {
        t->cur_col = col;
    }
}

static void cu_term_move_abs(cu_term *t, llong row, llong col)
{
    Trace("cu_term_move_abs: %lld %lld\n", row, col);
    if (row != -1) {
        size_t new_row = std::max(0ll, std::min((llong)t->lines.size() - 1,
                                  (llong)t->lines.size() - (llong)t->vis_lines + (llong)row - 1));
        cu_term_set_row(t, new_row);
    }
    if (col != -1) {
        cu_term_set_col(t, std::max(0ll, col - 1));
    }
}

static void cu_term_move_rel(cu_term *t, llong row, llong col)
{
    Trace("cu_term_move_rel: %lld %lld\n", row, col);
    llong new_row = t->cur_row + row;
    llong new_col = col == cu_term_col_home ? 0 : t->cur_col + col;
    if (new_row < 0) new_row = 0;
    if (new_col < 0) new_col = 0;
    cu_term_set_row(t, new_row);
    cu_term_set_col(t, new_col);
}

static void cu_term_scroll_region(cu_term *t, llong line0, llong line1)
{
    Trace("cu_term_scroll_region: %lld %lld\n", line0, line1);
    t->top_marg = line0;
    t->bot_marg = line1;
}

static void cu_term_reset_style(cu_term *t)
{
    t->tmpl.flags = 0;
    t->tmpl.fg = cu_cell_color_fg_dfl;
    t->tmpl.bg = cu_cell_color_bg_dfl;
}

void cu_term_set_fd(cu_term *t, int fd)
{
    t->fd = fd;
}

void cu_term_reset(cu_term *t)
{
    Trace("cu_term_reset\n");
    cu_term_move_abs(t, 1, 1);
    cu_term_reset_style(t);
}

static void cu_term_erase_screen(cu_term *t, uint arg)
{
    Trace("cu_term_erase_screen: %d\n", arg);
    switch (arg) {
    case cu_term_clear_end:
        for (size_t row = t->cur_row; row < t->lines.size(); row++) {
            t->lines[row].clear();
        }
        break;
    case cu_term_clear_start:
        for (size_t row = t->cur_row + 1; row < t->lines.size(); row++) {
            t->lines[row].clear();
        }
        break;
    case cu_term_clear_all:
        for (size_t row = 0; row < t->lines.size(); row++) {
            t->lines[row].clear();
        }
        cu_term_move_abs(t, 1, 1);
        t->vis_lines = t->vis_rows;
        break;
    }
}

static void cu_term_erase_line(cu_term *t, uint arg)
{
    Trace("cu_term_erase_line: %d\n", arg);
    switch (arg) {
    case cu_term_clear_end:
        if (t->cur_col < t->lines[t->cur_row].cells.size()) {
            t->lines[t->cur_row].cells.resize(t->cur_col);
        }
        break;
    case cu_term_clear_start:
        if (t->cur_col < t->lines[t->cur_row].cells.size()) {
            cu_cell cell = t->tmpl;
            cell.codepoint = ' ';
            for (size_t col = 0; col < t->cur_col; col++) {
                t->lines[t->cur_row].cells[col] = cell;
            }
        }
        break;
    case cu_term_clear_all:
        t->lines[t->cur_row].cells.resize(0);
        break;
    }
}

static void cu_term_insert_lines(cu_term *t, uint arg)
{
    Trace("cu_term_insert_lines: %d\n", arg);
    if (arg == 0) return;
    // consider line editing mode: *following*, or preceding
    // consider bottom margin for following mode. add bounds checks
    llong top = t->top_marg == 0 ? 1           : t->top_marg;
    llong bot = t->bot_marg == 0 ? t->vis_rows : t->bot_marg;
    llong scrolloff = bot < t->vis_rows ? t->vis_rows - bot : 0;
    t->lines[t->cur_row].pack();
    for (uint i = 0; i < arg; i++) {
        t->lines.insert(t->lines.begin() + t->cur_row, cu_line{});
        t->lines.erase(t->lines.end() - 1 - scrolloff);
    }
    t->lines[t->cur_row].unpack();
    t->cur_col = 0;
}

static void cu_term_delete_lines(cu_term *t, uint arg)
{
    Trace("cu_term_delete_lines: %d\n", arg);
    if (arg == 0) return;
    // consider line editing mode: *following*, or preceding
    // consider bottom margin for following mode. add bounds checks
    llong top = t->top_marg == 0 ? 1           : t->top_marg;
    llong bot = t->bot_marg == 0 ? t->vis_rows : t->bot_marg;
    llong scrolloff = bot < t->vis_rows ? t->vis_rows - bot : 0;
    t->lines[t->cur_row].pack();
    for (uint i = 0; i < arg; i++) {
        if (t->cur_row < t->lines.size()) {
            t->lines.erase(t->lines.begin() + t->cur_row);
            t->lines.insert(t->lines.end() - scrolloff, cu_line{});
        }
    }
    t->lines[t->cur_row].unpack();
    t->cur_col = 0;
}

static void cu_term_delete_chars(cu_term *t, uint arg)
{
    Trace("cu_term_delete_chars: %d\n", arg);
    for (size_t i = 0; i < arg; i++) {
        if (t->cur_col < t->lines[t->cur_row].cells.size()) {
            t->lines[t->cur_row].cells.erase(
                t->lines[t->cur_row].cells.begin() + t->cur_col
            );
        }
    }
}

static void cu_term_BEL(cu_term *t)
{
    Trace("cu_term_BEL: unimplemented\n");
}

static void cu_term_BS(cu_term *t)
{
    Trace("cu_term_BS\n");
    if (t->cur_col > 0) t->cur_col--;
}

static void cu_term_HT(cu_term *t)
{
    Trace("cu_term_HT\n");
    t->cur_col = (t->cur_col + 8) & ~7;
}

static void cu_term_LF(cu_term *t)
{
    Trace("cu_term_LF\n");
    cu_term_set_row(t, t->cur_row + 1);
}

static void cu_term_CR(cu_term *t)
{
    Trace("cu_term_CR\n");
    t->cur_col = 0;
}

static void cu_term_bare(cu_term *t, uint c)
{
    Trace("cu_term_bare: %s\n", char_str(c).c_str());
    if (t->cur_col >= t->lines[t->cur_row].cells.size()) {
        t->lines[t->cur_row].cells.resize(t->cur_col + 1);
    }
    t->lines[t->cur_row].cells[t->cur_col++] =
        cu_cell{c, t->tmpl.flags, t->tmpl.fg, t->tmpl.bg};
}

static void cu_term_ctrl(cu_term *t, uint c)
{
    Trace("cu_term_ctrl: %s\n", char_str(c).c_str());
    switch (c) {
    case cu_char_BEL: cu_term_BEL(t); break;
    case cu_char_BS: cu_term_BS(t); break;
    case cu_char_HT: cu_term_HT(t); break;
    case cu_char_LF: cu_term_LF(t); break;
    case cu_char_CR: cu_term_CR(t); break;
    default:
        Debug("cu_term_ctrl: unhandled control character %s\n",
            char_str(c).c_str());
    }
}

static void cu_term_xtwinops(cu_term *t)
{
    Debug("cu_term_xtwinops: %s unimplemented\n", esc_args(t).c_str());
}

static void cu_term_charset(cu_term *t, uint cmd, uint set)
{
    Debug("cu_term_charset: %c %c unimplemented\n", cmd, set);
}

static void cu_term_osc(cu_term *t, uint c)
{
    Debug("cu_term_osc: %s %s unimplemented\n",
        esc_args(t).c_str(), char_str(c).c_str());
    if (t->argc == 1 && t->argv[0] == 555) {
        Debug("cu_term_osc: screen-capture\n");
        t->needs_capture = 1;
    }
}

static void cu_term_osc_string(cu_term *t, uint c)
{
    Debug("cu_term_osc_string: %s %s \"%s\" unimplemented\n",
        esc_args(t).c_str(), char_str(c).c_str(), t->osc_string.c_str());
}

struct cu_term_private_mode_rec
{
    uint code;
    uint flag;
    const char *name;
    const char *desc;
};

static cu_term_private_mode_rec dec_flags[] = {
    {  1, cu_flag_DECCKM,  "DECCKM",  "DEC Cursor Key Mode" },
    {  7, cu_flag_DECAWM,  "DECAWM",  "DEC Auto Wrap Mode" },
    { 25, cu_flag_DECTCEM, "DECTCEM", "DEC Text Cursor Enable Mode" },
};

static cu_term_private_mode_rec* cu_term_lookup_private_mode_rec(uint code)
{
    for (size_t i = 0; i < array_size(dec_flags); i++) {
        if (dec_flags[i].code == code) return dec_flags + i;
    }
    return NULL;
}

static void cu_term_csi_private_mode(cu_term *t, uint code, uint set)
{
    cu_term_private_mode_rec *rec = cu_term_lookup_private_mode_rec(code);
    if (rec == NULL) {
        Debug("cu_term_csi_private_mode: %s flag %d: unimplemented\n",
            set ? "set" : "clear", code);
    } else {
        Debug("cu_term_csi_private_mode: %s flag %d: %s /* %s */\n",
            set ? "set" : "clear", code, rec->name, rec->desc);
        if (set) {
            t->flags |= rec->flag;
        } else {
            t->flags &= ~rec->flag;
        }
    }
}

static void cu_term_csi_dec(cu_term *t, uint c)
{
    switch (c) {
    case 'l': cu_term_csi_private_mode(t, opt_arg(t, 0, 0), 0); break;
    case 'h': cu_term_csi_private_mode(t, opt_arg(t, 0, 0), 1); break;
    default:
        Debug("cu_term_csi_dec: %s %s unimplemented\n",
            char_str(c).c_str(), esc_args(t).c_str());
        break;
    }
}

static void cu_term_csi_dec2(cu_term *t, uint c)
{
    Debug("cu_term_csi_dec2: %s %s unimplemented\n",
        char_str(c).c_str(), esc_args(t).c_str());
}

static void cu_term_csi_dec3(cu_term *t, uint c)
{
    Debug("cu_term_csi_dec3: %s %s unimplemented\n",
        char_str(c).c_str(), esc_args(t).c_str());
}

static void zterm_csi_dsr(cu_term *t)
{
    Trace("zterm_csi_dsr: %s\n", esc_args(t).c_str());
    switch (opt_arg(t, 0, 0)) {
    case 6: { /* report cursor position */
        char buf[32];
        llong col = t->cur_col + 1;
        llong row = t->cur_row - (t->lines.size() - t->vis_lines) + 1;
        row = std::max(1ll, std::min(row, t->vis_lines));
        int len = snprintf(buf, sizeof(buf), "\x1b[%llu;%lluR", row, col);
        cu_term_write(t, buf, len);
        break;
    }
    default:
        Debug("zterm_csi_dsr: %s\n", esc_args(t).c_str());
        break;
    }
}

static void cu_term_csi(cu_term *t, uint c)
{
    Trace("cu_term_csi: %s %s\n",
        esc_args(t).c_str(), char_str(c).c_str());

    switch (c) {
    case '@': /* insert blanks */
    {
        cu_line &line = t->lines[t->cur_row];
        int n = opt_arg(t, 0, 0);
        if (t->cur_col < line.cells.size()) {
            cu_cell cell = t->tmpl;
            cell.codepoint = ' ';
            for (size_t i = 0; i < n; i++) {
                line.cells.insert(line.cells.begin() + t->cur_col, cell);
            }
        }
        break;
    }
    case 'A': /* move up */
        cu_term_move_rel(t, -opt_arg(t, 0, 1), 0);
        break;
    case 'B': /* move down */
        cu_term_move_rel(t, opt_arg(t, 0, 1),  0);
        break;
    case 'C': /* move right */
        cu_term_move_rel(t, 0,  opt_arg(t, 0, 1));
        break;
    case 'D': /* move left */
        cu_term_move_rel(t, 0, -opt_arg(t, 0, 1));
        break;
    case 'E': /* move next line */
        cu_term_move_rel(t, opt_arg(t, 0, 1), cu_term_col_home);
        break;
    case 'F': /* move prev line */
        cu_term_move_rel(t, -opt_arg(t, 0, 1), cu_term_col_home);
        break;
    case 'G': /* move to {col} */
        cu_term_move_abs(t, -1, opt_arg(t, 0, 1));
        break;
    case 'H': /* move to {line};{col} */
        cu_term_move_abs(t, opt_arg(t, 0, 1), opt_arg(t, 1, 1));
        break;
    case 'J': /* erase lines {0=to-end,1=from-start,2=all} */
        switch (opt_arg(t, 0, 0)) {
        case 0: cu_term_erase_screen(t, cu_term_clear_end); break;
        case 1: cu_term_erase_screen(t, cu_term_clear_start); break;
        case 2: cu_term_erase_screen(t, cu_term_clear_all); break;
        default:
            Debug("cu_term_csi: CSI J: invalid arg: %d\n", opt_arg(t, 0, 0));
            break;
        }
        break;
    case 'K': /* erase chars {0=to-end,1=from-start,2=all} */
        switch (opt_arg(t, 0, 0)) {
        case 0: cu_term_erase_line(t, cu_term_clear_end); break;
        case 1: cu_term_erase_line(t, cu_term_clear_start); break;
        case 2: cu_term_erase_line(t, cu_term_clear_all); break;
        default:
            Debug("cu_term_csi: CSI K: invalid arg: %d\n", opt_arg(t, 0, 0));
            break;
        }
        break;
    case 'L': /* insert lines */
        cu_term_insert_lines(t, opt_arg(t, 0, 1));
        break;
    case 'M': /* delete lines */
        cu_term_delete_lines(t, opt_arg(t, 0, 1));
        break;
    case 'P': /* delete characters */
        cu_term_delete_chars(t, opt_arg(t, 0, 1));
        break;
    case 'd': /* move to {line};1 absolute */
        cu_term_move_abs(t, opt_arg(t, 0, 1), -1);
        break;
    case 'e': /* move to {line};1 relative */
        cu_term_move_rel(t, opt_arg(t, 0, 1), 0);
        break;
    case 'f': /* move to {line};{col} absolute */
        cu_term_move_abs(t, opt_arg(t, 0, 1), opt_arg(t, 1, 1));
        break;
    case 'm': /* color and formatting */
        if (t->argc == 0) {
            cu_term_reset_style(t);
            break;
        }
        for (size_t i = 0; i < t->argc; i++) {
            uint code = t->argv[i];
            switch (code) {
            case 0: cu_term_reset_style(t); break;
            case 1: t->tmpl.flags |= cu_cell_bold; break;
            case 2: t->tmpl.flags |= cu_cell_faint; break;
            case 3: t->tmpl.flags |= cu_cell_italic; break;
            case 4: t->tmpl.flags |= cu_cell_underline; break;
            case 5: t->tmpl.flags |= cu_cell_blink; break;
            case 6: t->tmpl.flags |= cu_cell_rblink; break;
            case 7: t->tmpl.flags |= cu_cell_inverse; break;
            case 8: t->tmpl.flags |= cu_cell_hidden; break;
            case 9: t->tmpl.flags |= cu_cell_strikeout; break;
            /* case 10 through 19 custom fonts */
            case 20: t->tmpl.flags |= cu_cell_fraktur; break;
            case 21: t->tmpl.flags |= cu_cell_dunderline; break;
            case 22: t->tmpl.flags &= ~(cu_cell_bold | cu_cell_faint); break;
            case 23: t->tmpl.flags &= ~(cu_cell_italic | cu_cell_fraktur); break;
            case 24: t->tmpl.flags &= ~(cu_cell_underline | cu_cell_dunderline); break;
            case 25: t->tmpl.flags &= ~cu_cell_blink; break;
            case 26: t->tmpl.flags &= ~cu_cell_rblink; break;
            case 27: t->tmpl.flags &= ~cu_cell_inverse; break;
            case 28: t->tmpl.flags &= ~cu_cell_hidden; break;
            case 29: t->tmpl.flags &= ~cu_cell_strikeout; break;
            case 30: t->tmpl.fg = cu_cell_color_nr_black; break;
            case 31: t->tmpl.fg = cu_cell_color_nr_red; break;
            case 32: t->tmpl.fg = cu_cell_color_nr_green; break;
            case 33: t->tmpl.fg = cu_cell_color_nr_yellow; break;
            case 34: t->tmpl.fg = cu_cell_color_nr_blue; break;
            case 35: t->tmpl.fg = cu_cell_color_nr_magenta; break;
            case 36: t->tmpl.fg = cu_cell_color_nr_cyan; break;
            case 37: t->tmpl.fg = cu_cell_color_nr_white; break;
            case 38:
                if (i + 2 < t->argc && t->argv[i+1] == 5) {
                    t->tmpl.fg = cu_term_colors_256[t->argv[i+2]];
                    i += 2;
                }
                else if (i + 4 < t->argc && t->argv[i+1] == 2) {
                    uint r = cu_term_colors_256[t->argv[i+2]];
                    uint g = cu_term_colors_256[t->argv[i+3]];
                    uint b = cu_term_colors_256[t->argv[i+4]];
                    t->tmpl.fg = (((uint)r << 0) | ((uint)g << 8) |
                                  ((uint)b << 16) | ((uint)0xff << 24));
                    i += 4;
                }
                break;
            case 39: t->tmpl.fg = cu_cell_color_fg_dfl; break;
            case 40: t->tmpl.bg = cu_cell_color_nr_black; break;
            case 41: t->tmpl.bg = cu_cell_color_nr_red; break;
            case 42: t->tmpl.bg = cu_cell_color_nr_green; break;
            case 43: t->tmpl.bg = cu_cell_color_nr_yellow; break;
            case 44: t->tmpl.bg = cu_cell_color_nr_blue; break;
            case 45: t->tmpl.bg = cu_cell_color_nr_magenta; break;
            case 46: t->tmpl.bg = cu_cell_color_nr_cyan; break;
            case 47: t->tmpl.bg = cu_cell_color_nr_white; break;
            case 48:
                if (i + 2 < t->argc && t->argv[i+1] == 5) {
                    t->tmpl.bg = cu_term_colors_256[t->argv[i+2]];
                    i += 2;
                }
                else if (i + 4 < t->argc && t->argv[i+1] == 2) {
                    uint r = cu_term_colors_256[t->argv[i+2]];
                    uint g = cu_term_colors_256[t->argv[i+3]];
                    uint b = cu_term_colors_256[t->argv[i+4]];
                    t->tmpl.bg = (((uint)r << 0) | ((uint)g << 8) |
                                  ((uint)b << 16) | ((uint)0xff << 24));
                    i += 4;
                }
                break;
            case 49: t->tmpl.bg = cu_cell_color_bg_dfl; break;
            case 90: t->tmpl.fg = cu_cell_color_br_black; break;
            case 91: t->tmpl.fg = cu_cell_color_br_red; break;
            case 92: t->tmpl.fg = cu_cell_color_br_green; break;
            case 93: t->tmpl.fg = cu_cell_color_br_yellow; break;
            case 94: t->tmpl.fg = cu_cell_color_br_blue; break;
            case 95: t->tmpl.fg = cu_cell_color_br_magenta; break;
            case 96: t->tmpl.fg = cu_cell_color_br_cyan; break;
            case 97: t->tmpl.fg = cu_cell_color_br_white; break;
            case 100: t->tmpl.bg = cu_cell_color_br_black; break;
            case 101: t->tmpl.bg = cu_cell_color_br_red; break;
            case 102: t->tmpl.bg = cu_cell_color_br_green; break;
            case 103: t->tmpl.bg = cu_cell_color_br_yellow; break;
            case 104: t->tmpl.bg = cu_cell_color_br_blue; break;
            case 105: t->tmpl.bg = cu_cell_color_br_magenta; break;
            case 106: t->tmpl.bg = cu_cell_color_br_cyan; break;
            case 107: t->tmpl.bg = cu_cell_color_br_white; break;
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
        cu_term_scroll_region(t, opt_arg(t, 0, 1), opt_arg(t, 1, 1));
        break;
    case 't': /* window manager hints */
        cu_term_xtwinops(t);
        break;
    }
}

static void cu_term_absorb(cu_term *t, uint c)
{
    Trace("cu_term_absorb: %s\n", char_str(c).c_str());
restart:
    switch (t->state) {
    case cu_state_normal:
        if ((c & 0xf8) == 0xf8) {
        } else if ((c & 0xf0) == 0xf0) {
            t->state = cu_state_utf4;
            t->code = c & 0x07;
        } else if ((c & 0xe0) == 0xe0) {
            t->state = cu_state_utf3;
            t->code = c & 0x0f;
        } else if ((c & 0xc0) == 0xc0) {
            t->state = cu_state_utf2;
            t->code = c & 0x1f;
        } else {
            if (c == cu_char_ESC) {
                t->state = cu_state_escape;
                t->argc = t->code = 0;
            } else if (c < 0x20) {
                cu_term_ctrl(t, c);
            } else {
                cu_term_bare(t, c);
            }
        }
        break;
    case cu_state_utf4:
        t->code = (t->code << 6) | (c & 0x3f);
        t->state = cu_state_utf3;
        break;
    case cu_state_utf3:
        t->code = (t->code << 6) | (c & 0x3f);
        t->state = cu_state_utf2;
        break;
    case cu_state_utf2:
        t->code = (t->code << 6) | (c & 0x3f);
        cu_term_bare(t, t->code);
        t->state = cu_state_normal;
        break;
    case cu_state_escape:
        switch (c) {
        case '[':
            t->state = cu_state_csi0;
            return;
        case ']':
            t->state = cu_state_osc0;
            return;
        case '(':
        case '*':
        case '+':
        case '-':
        case '.':
        case '/':
            t->code = c;
            t->state = cu_state_charset;
            return;
        case '=':
            Debug("cu_term_absorb: enter alternate keypad mode: unimplemented\n");
            t->state = cu_state_normal;
            return;
        case '>':
            Debug("cu_term_absorb: exit alternate keypad mode: unimplemented\n");
            t->state = cu_state_normal;
            return;
        default:
            Debug("cu_term_absorb: invalid ESC char '%c' (0x%02x)\n", c, c);
            t->state = cu_state_normal;
            break;
        }
        break;
    case cu_state_charset:
        cu_term_charset(t, t->code, c);
        t->state = cu_state_normal;
        break;
    case cu_state_csi0:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->state = cu_state_csi;
            goto restart;
        case '@': case 'A': case 'B': case 'C': case 'D':
        case 'E': case 'F': case 'G': case 'H': case 'I':
        case 'J': case 'K': case 'L': case 'M': case 'P':
        case 'd': case 'e': case 'f': case 'm': case 'n':
        case 'r': case 't':
            cu_term_csi(t, c);
            t->state = cu_state_normal;
            break;
        case '?': /* DEC */
            t->state = cu_state_csi_dec;
            break;
        case '>': /* DEC2 */
            t->state = cu_state_csi_dec2;
            break;
        case '=': /* DEC3 */
            t->state = cu_state_csi_dec3;
            break;
        default:
            Debug("cu_term_absorb: invalid CSI char '%c' (0x%02x)\n", c, c);
            t->state = cu_state_normal;
            goto restart;
        }
        break;
    case cu_state_csi:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->code = t->code * 10 + (c - '0');
            break;
        case ';':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("cu_term_absorb: CSI too many args, ignoring %d\n",
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
                Debug("cu_term_absorb: CSI too many args, ignoring %d\n",
                    t->code);
            }
            cu_term_csi(t, c);
            t->state = cu_state_normal;
            break;
        default:
            Debug("cu_term_absorb: invalid CSI char '%c' (0x%02x)\n", c, c);
            t->state = cu_state_normal;
            break;
        }
        break;
    case cu_state_csi_dec:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->code = t->code * 10 + (c - '0');
            break;
        case ';':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("cu_term_absorb: CSI ? too many args, ignoring %d\n",
                    t->code);
            }
            break;
        case 'c': case 'h': case 'i': case 'l': case 'n':
        case 'r': case 's': case 'S': case 'J': case 'K':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("cu_term_absorb: CSI ? too many args, ignoring %d\n",
                    t->code);
            }
            cu_term_csi_dec(t, c);
            t->state = cu_state_normal;
            break;
        default:
            Debug("cu_term_absorb: invalid CSI ? char '%c' (0x%02x)\n", c, c);
            t->state = cu_state_normal;
            break;
        }
        break;
    case cu_state_csi_dec2:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->code = t->code * 10 + (c - '0');
            break;
        case ';':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("cu_term_absorb: CSI > too many args, ignoring %d\n",
                    t->code);
            }
            cu_term_csi_dec2(t, c);
            t->state = cu_state_normal;
            break;
        case 'c': /* device report */
            Debug("cu_term_absorb: CSI > device report\n");
            t->state = cu_state_normal;
            break;
        default:
            Debug("cu_term_absorb: invalid CSI > char '%c' (0x%02x)\n", c, c);
            t->state = cu_state_normal;
            break;
        }
        break;
    case cu_state_csi_dec3:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->code = t->code * 10 + (c - '0');
            break;
        case ';':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("cu_term_absorb: CSI = too many args, ignoring %d\n",
                    t->code);
            }
            break;
        case 'c': /* device report */
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("cu_term_absorb: CSI = too many args, ignoring %d\n",
                    t->code);
            }
            cu_term_csi_dec3(t, c);
            t->state = cu_state_normal;
            break;
        default:
            Debug("cu_term_absorb: invalid CSI = char '%c' (0x%02x)\n", c, c);
            t->state = cu_state_normal;
            break;
        }
        break;
    case cu_state_osc0:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->state = cu_state_osc;
            goto restart;
        case cu_char_BEL:
            cu_term_osc(t, c);
            t->state = cu_state_normal;
            break;
        default:
            Debug("cu_term_absorb: invalid OSC char '%c' (0x%02x)\n", c, c);
            //t->state = cu_state_normal;
            break;
        }
        break;
    case cu_state_osc:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->code = t->code * 10 + (c - '0');
            break;
        case ';':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("cu_term_absorb: OSC too many args, ignoring %d\n",
                    t->code);
            }
            if (t->argc == 1 && t->argv[0] == 7) {
                t->state = cu_state_osc_string;
                t->osc_string.clear();
            }
            break;
        case cu_char_BEL:
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("cu_term_absorb: OSC too many args, ignoring %d\n",
                    t->code);
            }
            cu_term_osc(t, c);
            t->state = cu_state_normal;
            break;
        default:
            Debug("cu_term_absorb: invalid OSC char '%c' (0x%02x)\n", c, c);
            //t->state = cu_state_normal;
            break;
        }
        break;
    case cu_state_osc_string:
        if (c == cu_char_BEL) {
            cu_term_osc_string(t, c);
            t->state = cu_state_normal;
        } else {
            t->osc_string.append(std::string(1, c));            
        }
        break;
    }
    t->needs_update = 1;
}

ssize_t cu_term_io(cu_term *t)
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
            Debug("cu_term_io: wrote %zu bytes -> pty\n", len);
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
            Debug("cu_term_io: read %zu bytes <- pty\n", len);
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

ssize_t cu_term_process(cu_term *t)
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
        cu_term_absorb(t, t->in_buf[t->in_start]);
        t->in_start++;
    }
    if (t->in_end < t->in_start && t->in_start == t->in_buf.size()) {
        /* zero xxxxxxxx end _________________ start limit */
        /* zero start xxxxxxxx end _________________ limit */
        t->in_start = 0;
    }
    if (count > 0) {
        Debug("cu_term_process: processed %zu bytes of input\n", count);
    }
    return count;
}

ssize_t cu_term_write(cu_term *t, const char *buf, size_t len)
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
        Debug("cu_term_write: buffered %zu bytes of output\n", len);
        t->out_end += ncopy;
    }
    if (t->out_start < t->out_end && t->out_end == t->out_buf.size()) {
        /* zero ________ start xxxxxxxxxxxxxxxx end limit */
        /* zero end ________ start xxxxxxxxxxxxxxxx limit */
        t->out_end = 0;
    }
    return ncopy;
}

static int cu_term_keycode_to_char(int key, int mods)
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

void cu_term_keyboard(cu_term *t, int key, int scancode, int action, int mods)
{
    int c;
    switch (action) {
    case GLFW_PRESS:
        if ((t->flags & cu_flag_DECCKM) > 0) {
            switch (key) {
            case GLFW_KEY_ESCAPE: cu_term_send(t, cu_char_ESC); break;
            case GLFW_KEY_ENTER: cu_term_send(t, cu_char_LF); break;
            case GLFW_KEY_TAB: cu_term_send(t, cu_char_HT); break;
            case GLFW_KEY_BACKSPACE: cu_term_send(t, cu_char_BS); break;
            case GLFW_KEY_INSERT: cu_term_write(t, "\x1b[2~", 4); break; // ?
            case GLFW_KEY_DELETE: cu_term_write(t, "\x1b[3~", 4); break; // ?
            case GLFW_KEY_PAGE_UP: cu_term_write(t, "\x1b[5~", 4); break; // ?
            case GLFW_KEY_PAGE_DOWN: cu_term_write(t, "\x1b[6~", 4); break; // ?
            case GLFW_KEY_UP: cu_term_write(t, "\x1bOA", 3); break;
            case GLFW_KEY_DOWN: cu_term_write(t, "\x1bOB", 3); break;
            case GLFW_KEY_RIGHT: cu_term_write(t, "\x1bOC", 3); break;
            case GLFW_KEY_LEFT: cu_term_write(t, "\x1bOD", 3); break;
            case GLFW_KEY_HOME: cu_term_write(t, "\x1bOH", 3); break;
            case GLFW_KEY_END: cu_term_write(t, "\x1bOF", 3); break;
            case GLFW_KEY_F1: cu_term_write(t, "\x1bOP", 3); break;
            case GLFW_KEY_F2: cu_term_write(t, "\x1bOQ", 3); break;
            case GLFW_KEY_F3: cu_term_write(t, "\x1bOR", 3); break;
            case GLFW_KEY_F4: cu_term_write(t, "\x1bOS", 3); break;
            case GLFW_KEY_F5: cu_term_write(t, "\x1b[15~", 5); break; // ?
            case GLFW_KEY_F6: cu_term_write(t, "\x1b[17~", 5); break; // ?
            case GLFW_KEY_F7: cu_term_write(t, "\x1b[18~", 5); break; // ?
            case GLFW_KEY_F8: cu_term_write(t, "\x1b[19~", 5); break; // ?
            case GLFW_KEY_F9: cu_term_write(t, "\x1b[20~", 5); break; // ?
            case GLFW_KEY_F10: cu_term_write(t, "\x1b[21~", 5); break; // ?
            case GLFW_KEY_F11: cu_term_write(t, "\x1b[23~", 5); break; // ?
            case GLFW_KEY_F12: cu_term_write(t, "\x1b[24~", 5); break; // ?
            default:
                c = cu_term_keycode_to_char(key, mods);
                if (c) cu_term_send(t, c);
                break;
            }
        } else {
            switch (key) {
            case GLFW_KEY_ESCAPE: cu_term_send(t, cu_char_ESC); break;
            case GLFW_KEY_ENTER: cu_term_send(t, cu_char_LF); break;
            case GLFW_KEY_TAB: cu_term_send(t, cu_char_HT); break;
            case GLFW_KEY_BACKSPACE: cu_term_send(t, cu_char_BS); break;
            case GLFW_KEY_INSERT: cu_term_write(t, "\x1b[2~", 4); break;
            case GLFW_KEY_DELETE: cu_term_write(t, "\x1b[3~", 4); break;
            case GLFW_KEY_PAGE_UP: cu_term_write(t, "\x1b[5~", 4); break;
            case GLFW_KEY_PAGE_DOWN: cu_term_write(t, "\x1b[6~", 4); break;
            case GLFW_KEY_UP: cu_term_write(t, "\x1b[A", 3); break;
            case GLFW_KEY_DOWN: cu_term_write(t, "\x1b[B", 3); break;
            case GLFW_KEY_RIGHT: cu_term_write(t, "\x1b[C", 3); break;
            case GLFW_KEY_LEFT: cu_term_write(t, "\x1b[D", 3); break;
            case GLFW_KEY_HOME: cu_term_write(t, "\x1b[H", 3); break;
            case GLFW_KEY_END: cu_term_write(t, "\x1b[F", 3); break;
            case GLFW_KEY_F1: cu_term_write(t, "\x1b[11~", 5); break;
            case GLFW_KEY_F2: cu_term_write(t, "\x1b[12~", 5); break;
            case GLFW_KEY_F3: cu_term_write(t, "\x1b[13~", 5); break;
            case GLFW_KEY_F4: cu_term_write(t, "\x1b[14~", 5); break;
            case GLFW_KEY_F5: cu_term_write(t, "\x1b[15~", 5); break;
            case GLFW_KEY_F6: cu_term_write(t, "\x1b[17~", 5); break;
            case GLFW_KEY_F7: cu_term_write(t, "\x1b[18~", 5); break;
            case GLFW_KEY_F8: cu_term_write(t, "\x1b[19~", 5); break;
            case GLFW_KEY_F9: cu_term_write(t, "\x1b[20~", 5); break;
            case GLFW_KEY_F10: cu_term_write(t, "\x1b[21~", 5); break;
            case GLFW_KEY_F11: cu_term_write(t, "\x1b[23~", 5); break;
            case GLFW_KEY_F12: cu_term_write(t, "\x1b[24~", 5); break;
            default:
                c = cu_term_keycode_to_char(key, mods);
                if (c) cu_term_send(t, c);
                break;
            }
        }
        break;
    }
}
