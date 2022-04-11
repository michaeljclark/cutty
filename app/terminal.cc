#include <cstdlib>
#include <cerrno>
#include <cassert>

#include "app.h"
#include "utf8.h"
#include "cuterm.h"

static int io_buffer_size = 65536;
static int io_buffer_min = 1024;
static int io_poll_timeout = 1;

static const char* ctrl_code[128] = {
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

void cuterm_init(cu_term *t)
{
    t->state =  cu_state_normal;
    t->flags =  cu_term_wrap;
    t->charset = cu_charset_utf8;

    t->in_buf.resize(io_buffer_size);
    t->in_start = 0;
    t->in_end = 0;

    t->out_buf.resize(io_buffer_size);
    t->out_start = 0;
    t->out_end = 0;

    t->tmpl.fg_col = cu_cell_color_black;
    t->tmpl.bg_col = cu_cell_color_white;

    t->lines.push_back(cu_line{});
    t->cur_row = 0;
    t->cur_col = 0;

    t->needs_update++;
}

void cuterm_close(cu_term *t)
{
    close(t->fd);
}

int cuterm_fork(cu_term *t, uint cols, uint rows)
{
    memset(&t->ws, 0, sizeof(t->ws));
    t->ws.ws_col = cols;
    t->ws.ws_row = rows;

    memset(&t->tio, 0, sizeof(t->tio));
    t->tio.c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOKE | ECHOCTL;
    t->tio.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | IUTF8 | BRKINT;
    t->tio.c_oflag = OPOST | ONLCR;
    t->tio.c_cflag = CREAD | CS8 | HUPCL;

    t->tio.c_cc[VINTR] = CTRL('c');
    t->tio.c_cc[VQUIT] = CTRL('\\');
    t->tio.c_cc[VERASE] = 0177; /* DEL */
    t->tio.c_cc[VKILL] = CTRL('u');
    t->tio.c_cc[VEOF] = CTRL('d');
    t->tio.c_cc[VEOL] = 255;
    t->tio.c_cc[VEOL2] = 255;
    t->tio.c_cc[VSTART] = CTRL('q');
    t->tio.c_cc[VSTOP] = CTRL('s');
    t->tio.c_cc[VSUSP] = CTRL('z');
    t->tio.c_cc[VREPRINT] = CTRL('r');
    t->tio.c_cc[VWERASE] = CTRL('w');
    t->tio.c_cc[VLNEXT] = CTRL('v');
    t->tio.c_cc[VDISCARD] = CTRL('o');
#if defined(__APPLE__) || defined(__FreeBSD__)
    t->tio.c_cc[VDSUSP] = CTRL('y');
    t->tio.c_cc[VSTATUS] = CTRL('t');
#endif
    t->tio.c_cc[VMIN] = 1;
    t->tio.c_cc[VTIME] = 0;

    cfsetispeed(&t->tio, B9600);
    cfsetospeed(&t->tio, B9600);

    switch (forkpty((int*)&t->fd, t->slave, &t->tio, &t->ws)) {
    case -1:
        Panic("forkpty: %s", strerror(errno));
    case 0:
        setenv("TERM", "xterm-256color", 1);
        setenv("LC_CTYPE", "UTF-8", 0);
        execlp("bash", "-bash", NULL);
        _exit(1);
    }

    Debug("cuterm_fork: fd=%d ws_row=%d ws_col=%d slave=%s\n",
        t->fd, t->ws.ws_row, t->ws.ws_col, t->slave);

    return 0;
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

static void cuterm_send(cu_term *t, uint c)
{
    Trace("cuterm_send: %s\n", char_str(c).c_str());
    char b = (char)c;
    cuterm_write(t, &b, 1);
}

/*
 * - unpacked lines: cells vector has one element for every character and
 *   each cell has a utf32 codepoint. style flags, foreground and background
 *   colors. the cell count for the line is in cells.size().
 * - packed lines: cells vector holds style changes. the codepoint element
 *   contains an offset into utf8_data and the cell count is in pcount.
 */

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
        if (s.flags != t.flags || s.fg_col != t.fg_col || s.bg_col != t.bg_col) {
            t = cu_cell{(uint)utf8.size(), s.flags, s.fg_col, s.bg_col};
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
        ucells.push_back(cu_cell{(uint)v.code, t.flags, t.fg_col, t.bg_col});
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

static void cuterm_set_row(cu_term *t, int row)
{
    if (row != t->cur_row) {
        t->lines[t->cur_row].pack();
        if (row >= t->lines.size()) {
            t->lines.resize(row + 1);
        } else {
            t->lines[row].unpack();
        }
        t->cur_row = row;
    }
}

static void cuterm_set_col(cu_term *t, int col)
{
    if (col != t->cur_col) {
        t->cur_col = col;
    }
}

static void cuterm_resize(cu_term *t)
{
    /* make sure there as enough lines to encompass the current row */
    if (t->cur_row >= t->lines.size()) {
        t->lines.resize(t->cur_row + 1);
    }

    /* make we have as many physical lines as there are visible lines */
    if (t->lines.size() < t->vis_lines) {
        size_t new_rows = t->vis_lines - t->lines.size();
        for (size_t i = 0; i < new_rows; i++) {
            t->lines.insert(t->lines.begin(),cu_line{});
        }
        cuterm_set_row(t, t->cur_row + new_rows);
    }
}

void cuterm_move_abs(cu_term *t, int row, int col)
{
    if (row != -1) {
        cuterm_resize(t);
        size_t new_row = std::max(size_t(0), std::min(t->lines.size() - 1,
                                  t->lines.size() - t->vis_lines + row - 1));
        cuterm_set_row(t, new_row);
    }

    if (col != -1) {
        cuterm_set_col(t, std::max(0, col - 1));
    }
}

void cuterm_move_rel(cu_term *t, int row, int col)
{
    int new_row = t->cur_row + row;
    int new_col = t->cur_col + col;

    if (new_row < 0) new_row = 0;
    if (new_col < 0) new_col = 0;

    if (new_row >= t->lines.size()) {
        t->lines.resize(new_row + 1);
    }

    cuterm_set_row(t, new_row);
    cuterm_set_col(t, new_col);
}

static void cuterm_erase_screen(cu_term *t, uint arg)
{
    Trace("cuterm_erase_screen: arg=%d\n", arg);

    cuterm_resize(t);

    switch (arg) {
    case cuterm_clear_end:
        for (size_t row = t->cur_row; row < t->lines.size(); row++) {
            t->lines[row].clear();
        }
        // move where?
        break;
    case cuterm_clear_start:
        for (size_t row = t->cur_row + 1; row < t->lines.size(); row++) {
            t->lines[row].clear();
        }
        // move where?
        break;
    case cuterm_clear_all:
        for (size_t row = 0; row < t->lines.size(); row++) {
            t->lines[row].clear();
        }
        // move 0,0 ?
        break;
    }
}

static void cuterm_erase_line(cu_term *t, uint arg)
{
    Trace("cuterm_erase_line: arg=%d\n", arg);

    switch (arg) {
    case cuterm_clear_end:
        if (t->cur_col < t->lines[t->cur_row].cells.size()) {
            t->lines[t->cur_row].cells.resize(t->cur_col);
        }
        break;
    case cuterm_clear_start:
        if (t->cur_col < t->lines[t->cur_row].cells.size()) {
            cu_cell cell = t->tmpl;
            cell.codepoint = ' ';
            for (size_t col = 0; col < t->cur_col; col++) {
                t->lines[t->cur_row].cells[col] = cell;
            }
        }
        break;
    case cuterm_clear_all:
        t->lines[t->cur_row].cells.resize(0);
        break;
    }
}

static void cuterm_BEL(cu_term *t)
{
    Trace("cuterm_BEL: unimplemented\n");
}

static void cuterm_BS(cu_term *t)
{
    Trace("cuterm_BS\n");
    if (t->cur_col > 0) {
        t->cur_col--;
    }
}

static void cuterm_HT(cu_term *t)
{
    Trace("cuterm_HT\n");
    t->cur_col = (t->cur_col + 8) & ~7;
}

static void cuterm_LF(cu_term *t)
{
    Trace("cuterm_LF\n");
    cuterm_set_row(t, t->cur_row + 1);
}

static void cuterm_CR(cu_term *t)
{
    Trace("cuterm_CR\n");
    t->cur_col = 0;
}

static void cuterm_bare(cu_term *t, uint c)
{
    Trace("cuterm_bare: %s\n", char_str(c).c_str());
    if (t->cur_col >= t->lines[t->cur_row].cells.size()) {
        t->lines[t->cur_row].cells.resize(t->cur_col + 1);
    }
    t->lines[t->cur_row].cells[t->cur_col++] =
        cu_cell{c, t->tmpl.flags, t->tmpl.fg_col, t->tmpl.bg_col};
}

static void cuterm_ctrl(cu_term *t, uint c)
{
    Trace("cuterm_ctrl: %s\n", char_str(c).c_str());
    switch (c) {
    case cu_char_BEL: cuterm_BEL(t); break;
    case cu_char_BS: cuterm_BS(t); break;
    case cu_char_HT: cuterm_HT(t); break;
    case cu_char_LF: cuterm_LF(t); break;
    case cu_char_CR: cuterm_CR(t); break;
    default:
        Debug("cuterm_ctrl: unhandled control character %s\n",
            char_str(c).c_str());
    }
}

static void cuterm_charset(cu_term *t, uint cmd, uint set)
{
    Debug("cuterm_charset: %c %c\n", cmd, set);
}

static void cuterm_osc(cu_term *t, uint c)
{
    Debug("cuterm_osc: %s %s\n",
        esc_args(t).c_str(), char_str(c).c_str());
}

static void cuterm_osc_string(cu_term *t, uint c)
{
    Debug("cuterm_osc_string: %s %s \"%s\"\n",
        esc_args(t).c_str(), char_str(c).c_str(), t->osc_string.c_str());
}

static void cuterm_csi_dec(cu_term *t, uint c)
{
    Trace("cuterm_csi_dec: %s %s\n",
        char_str(c).c_str(), esc_args(t).c_str());
    switch (c) {
    default:
    Debug("cuterm_csi_dec: %s %s unimplemented\n",
        char_str(c).c_str(), esc_args(t).c_str());
        break;
    }
}

static void cuterm_csi_dec2(cu_term *t, uint c)
{
    Trace("cuterm_csi_dec2: %s %s\n",
        char_str(c).c_str(), esc_args(t).c_str());
    switch (c) {
    default:
    Debug("cuterm_csi_dec2: %s %s unimplemented\n",
        char_str(c).c_str(), esc_args(t).c_str());
        break;
    }
}

static void cuterm_csi_dec3(cu_term *t, uint c)
{
    Trace("cuterm_csi_dec3: %s %s\n",
        char_str(c).c_str(), esc_args(t).c_str());
    switch (c) {
    default:
    Debug("cuterm_csi_dec3: %s %s unimplemented\n",
        char_str(c).c_str(), esc_args(t).c_str());
        break;
    }
}

static void cuterm_csi(cu_term *t, uint c)
{
    Trace("cuterm_csi: %s %s\n",
        esc_args(t).c_str(),
        char_str(c).c_str());

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
        cuterm_move_rel(t, -1, 0);
        break;
    case 'B': /* move down */
        cuterm_move_rel(t, 1,  0);
        break;
    case 'C': /* move right */
        cuterm_move_rel(t, 0,  1);
        break;
    case 'D': /* move left */
        cuterm_move_rel(t, 0, -1);
        break;
    case 'E': /* move beginning */
        Debug("cuterm_csi: CSI E unimplemented");
        break;
    case 'F': /* move end */
        Debug("cuterm_csi: CSI F unimplemented");
        break;
    case 'G': /* move to {col} */
        cuterm_move_abs(t, -1, opt_arg(t, 0, 1));
        break;
    case 'H': /* move to {line};{col} */
        cuterm_move_abs(t, opt_arg(t, 0, 1), opt_arg(t, 1, 1));
        break;
    case 'd': /* move to {line};1 absolute */
        cuterm_move_abs(t, opt_arg(t, 0, 1), 1);
        break;
    case 'e': /* move to {line};1 relative */
        cuterm_move_rel(t, opt_arg(t, 0, 1), 0);
        break;
    case 'f': /* move to {line};{col} absolute */
        cuterm_move_abs(t, opt_arg(t, 0, 1), opt_arg(t, 1, 1));
        break;
    case 'J':
        switch (opt_arg(t, 0, 0)) {
        case 0: cuterm_erase_screen(t, cuterm_clear_end); break;
        case 1: cuterm_erase_screen(t, cuterm_clear_start); break;
        case 2: cuterm_erase_screen(t, cuterm_clear_all); break;
        default:
            Debug("cuterm_csi: CSI J: invalid arg: %d\n", opt_arg(t, 0, 0));
            break;
        }
        break;
    case 'K':
        switch (opt_arg(t, 0, 0)) {
        case 0: cuterm_erase_line(t, cuterm_clear_end); break;
        case 1: cuterm_erase_line(t, cuterm_clear_start); break;
        case 2: cuterm_erase_line(t, cuterm_clear_all); break;
        default:
            Debug("cuterm_csi: CSI K: invalid arg: %d\n", opt_arg(t, 0, 0));
            break;
        }
        break;
    case 'm': /* color and formatting */
        if (t->argc == 0) { /* reset formatting */
            t->tmpl.flags = 0;
            t->tmpl.fg_col = cu_cell_color_black;
            t->tmpl.bg_col = cu_cell_color_white;
            break;
        }
        for (size_t i = 0; i < t->argc; i++) {
            uint code = t->argv[i];
            switch (code) {
            case 0: /* reset formatting */
                t->tmpl.flags = 0;
                t->tmpl.fg_col = cu_cell_color_black;
                t->tmpl.bg_col = cu_cell_color_white;
                break;
            case 1: t->tmpl.flags |= cu_cell_bold; break;
            case 2: t->tmpl.flags |= cu_cell_faint; break;
            case 3: t->tmpl.flags |= cu_cell_italic; break;
            case 4: t->tmpl.flags |= cu_cell_underline; break;
            case 5: t->tmpl.flags |= cu_cell_blink; break;
            case 7: t->tmpl.flags |= cu_cell_inverse; break;
            case 8: t->tmpl.flags |= cu_cell_hidden; break;
            case 9: t->tmpl.flags |= cu_cell_strikethrough; break;
            /* case 10 through 19 custom fonts */
            case 20: t->tmpl.flags |= cu_cell_fraktur; break;
            case 21: t->tmpl.flags |= cu_cell_doubleunderline; break;
            case 22: t->tmpl.flags &= ~(cu_cell_bold | cu_cell_faint); break;
            case 23: t->tmpl.flags &= ~(cu_cell_italic | cu_cell_fraktur); break;
            case 24: t->tmpl.flags &= ~(cu_cell_underline | cu_cell_doubleunderline); break;
            case 25: t->tmpl.flags &= ~cu_cell_blink; break;
            case 27: t->tmpl.flags &= ~cu_cell_inverse; break;
            case 28: t->tmpl.flags &= ~cu_cell_hidden; break;
            case 29: t->tmpl.flags &= ~cu_cell_strikethrough; break;
            case 30: t->tmpl.fg_col = cu_cell_color_black; break;
            case 31: t->tmpl.fg_col = cu_cell_color_red; break;
            case 32: t->tmpl.fg_col = cu_cell_color_green; break;
            case 33: t->tmpl.fg_col = cu_cell_color_yellow; break;
            case 34: t->tmpl.fg_col = cu_cell_color_blue; break;
            case 35: t->tmpl.fg_col = cu_cell_color_magenta; break;
            case 36: t->tmpl.fg_col = cu_cell_color_cyan; break;
            case 37: t->tmpl.fg_col = cu_cell_color_white; break;
                break;
            default:
                break;
            }
        }
        break;
    }
}

static void cuterm_absorb(cu_term *t, uint c)
{
    Trace("cuterm_absorb: %s\n", char_str(c).c_str());
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
                cuterm_ctrl(t, c);
            } else {
                cuterm_bare(t, c);
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
        cuterm_bare(t, t->code);
        t->state = cu_state_normal;
        break;
    case cu_state_escape:
        switch (c) {
        case '[':
            t->state = cu_state_csi_init;
            return;
        case ']':
            t->state = cu_state_osc;
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
            Debug("cuterm_absorb: enter alternate keypad mode: unimplemented\n");
            t->state = cu_state_normal;
            return;
        case '>':
            Debug("cuterm_absorb: exit alternate keypad mode: unimplemented\n");
            t->state = cu_state_normal;
            return;
        default:
            Debug("cuterm_absorb: invalid ESC char '%c' (0x%02x)\n", c, c);
            t->state = cu_state_normal;
            break;
        }
        break;
    case cu_state_charset:
        cuterm_charset(t, t->code, c);
        t->state = cu_state_normal;
        break;
    case cu_state_csi_init:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->state = cu_state_csi;
            goto restart;
        case '@': case 'A': case 'B': case 'C': case 'D':
        case 'E': case 'F': case 'G': case 'H': case 'I':
        case 'J': case 'K': case 'm': case 'd':
            cuterm_csi(t, c);
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
            Debug("cuterm_absorb: invalid CSI char '%c' (0x%02x)\n", c, c);
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
                Debug("cuterm_absorb: CSI too many args, ignoring %d\n",
                    t->code);
            }
            break;
        case '@': case 'A': case 'B': case 'C': case 'D':
        case 'E': case 'F': case 'G': case 'H': case 'I':
        case 'J': case 'K': case 'm': case 'd':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("cuterm_absorb: CSI too many args, ignoring %d\n",
                    t->code);
            }
            cuterm_csi(t, c);
            t->state = cu_state_normal;
            break;
        default:
            Debug("cuterm_absorb: invalid CSI char '%c' (0x%02x)\n", c, c);
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
                Debug("cuterm_absorb: CSI ? too many args, ignoring %d\n",
                    t->code);
            }
            break;
        case 'c':
        case 'h':
        case 'i':
        case 'l':
        case 'n':
        case 'r':
        case 's':
        case 'S':
        case 'J':
        case 'K':
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("cuterm_absorb: CSI ? too many args, ignoring %d\n",
                    t->code);
            }
            cuterm_csi_dec(t, c);
            t->state = cu_state_normal;
            break;
        default:
            Debug("cuterm_absorb: invalid CSI ? char '%c' (0x%02x)\n", c, c);
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
                Debug("cuterm_absorb: CSI > too many args, ignoring %d\n",
                    t->code);
            }
            cuterm_csi_dec2(t, c);
            t->state = cu_state_normal;
            break;
        case 'c': /* device report */
            Debug("cuterm_absorb: CSI > device report\n");
            t->state = cu_state_normal;
            break;
        default:
            Debug("cuterm_absorb: invalid CSI > char '%c' (0x%02x)\n", c, c);
            t->state = cu_state_normal;
            break;
        }
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
                Debug("cuterm_absorb: CSI = too many args, ignoring %d\n",
                    t->code);
            }
            break;
        case 'c': /* device report */
            if (t->argc < array_size(t->argv)) {
                t->argv[t->argc++] = t->code; t->code = 0;
            } else {
                Debug("cuterm_absorb: CSI = too many args, ignoring %d\n",
                    t->code);
            }
            cuterm_csi_dec3(t, c);
            t->state = cu_state_normal;
            break;
        default:
            Debug("cuterm_absorb: invalid CSI = char '%c' (0x%02x)\n", c, c);
            t->state = cu_state_normal;
            break;
        }
    case cu_state_osc_init:
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            t->state = cu_state_osc;
            goto restart;
        case cu_char_BEL:
            cuterm_osc(t, c);
            t->state = cu_state_normal;
            break;
        default:
            Debug("cuterm_absorb: invalid OSC char '%c' (0x%02x)\n", c, c);
            //t->state = cu_state_normal;
            break;
        }
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
                Debug("cuterm_absorb: OSC too many args, ignoring %d\n",
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
                Debug("cuterm_absorb: OSC too many args, ignoring %d\n",
                    t->code);
            }
            cuterm_osc(t, c);
            t->state = cu_state_normal;
            break;
        default:
            Debug("cuterm_absorb: invalid OSC char '%c' (0x%02x)\n", c, c);
            //t->state = cu_state_normal;
            break;
        }
        break;
    case cu_state_osc_string:
        if (c == cu_char_BEL) {
            cuterm_osc_string(t, c);
            t->state = cu_state_normal;
        } else {
            t->osc_string.append(std::string(1, c));
        }
        break;
    }
    t->needs_update++;
}

ssize_t cuterm_io(cu_term *t)
{
    struct pollfd pfds[1];
    ssize_t len;
    int ret;

    int do_poll_in  = -(t->in_buf.size() - t->in_end > 0);
    int do_poll_out = -(t->out_end - t->out_start > 0);

    pfds[0].fd = t->fd;
    pfds[0].events = (do_poll_in & POLLIN) | (do_poll_out & POLLOUT);
    ret = poll(pfds, array_size(pfds), io_poll_timeout);

    //Trace("cuterm_io: pollin=%d pollout=%d\n",
    //    pfds[0].revents & POLLIN, pfds[0].revents & POLLOUT);

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
            Debug("cuterm_io: wrote %zu bytes -> pty\n", len);
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
            Debug("cuterm_io: read %zu bytes <- pty\n", len);
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

ssize_t cuterm_process(cu_term *t)
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
        cuterm_absorb(t, t->in_buf[t->in_start]);
        t->in_start++;
    }
    if (t->in_end < t->in_start && t->in_start == t->in_buf.size()) {
        /* zero xxxxxxxx end _________________ start limit */
        /* zero start xxxxxxxx end _________________ limit */
        t->in_start = 0;
    }
    if (count > 0) {
        Debug("cuterm_process: processed %zu bytes of input\n", count);
    }
    return count;
}

ssize_t cuterm_write(cu_term *t, const char *buf, size_t len)
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
        Debug("cuterm_write: buffered %zu bytes of output\n", len);
        t->out_end += ncopy;
    }
    if (t->out_start < t->out_end && t->out_end == t->out_buf.size()) {
        /* zero ________ start xxxxxxxxxxxxxxxx end limit */
        /* zero end ________ start xxxxxxxxxxxxxxxx limit */
        t->out_end = 0;
    }
    return ncopy;
}

static int cuterm_keycode_to_char(int key, int mods)
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

void cuterm_keyboard(cu_term *t, int key, int scancode, int action, int mods)
{
    int c;
    switch (action) {
    case GLFW_PRESS:
        switch (key) {
        case GLFW_KEY_ESCAPE: cuterm_send(t, cu_char_ESC); break;
        case GLFW_KEY_ENTER: cuterm_send(t, cu_char_LF); break;
        case GLFW_KEY_TAB: cuterm_send(t, cu_char_HT); break;
        case GLFW_KEY_BACKSPACE: cuterm_send(t, cu_char_BS); break;
        case GLFW_KEY_INSERT: cuterm_write(t, "\x1b[2~", 4); break;
        case GLFW_KEY_DELETE: cuterm_write(t, "\x1b[3~", 4); break;
        case GLFW_KEY_PAGE_UP: cuterm_write(t, "\x1b[5~", 4); break;
        case GLFW_KEY_PAGE_DOWN: cuterm_write(t, "\x1b[6~", 4); break;
        case GLFW_KEY_UP: cuterm_write(t, "\x1b[A", 3); break;
        case GLFW_KEY_DOWN: cuterm_write(t, "\x1b[B", 3); break;
        case GLFW_KEY_RIGHT: cuterm_write(t, "\x1b[C", 3); break;
        case GLFW_KEY_LEFT: cuterm_write(t, "\x1b[D", 3); break;
        case GLFW_KEY_HOME: cuterm_write(t, "\x1b[H", 3); break;
        case GLFW_KEY_END: cuterm_write(t, "\x1b[F", 3); break;
        default:
            c = cuterm_keycode_to_char(key, mods);
            if (c) cuterm_send(t, c);
        }
        break;
    }
}
