#include <cstdio>
#include <cstring>
#include <cerrno>
#include <climits>
#include <cctype>

#include <functional>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <atomic>

#include "binpack.h"
#include "image.h"
#include "color.h"
#include "utf8.h"
#include "draw.h"
#include "font.h"
#include "glyph.h"
#include "canvas.h"
#include "color.h"
#include "logger.h"
#include "file.h"
#include "app.h"
#include "ui9.h"

#include "timestamp.h"
#include "teletype.h"
#include "cellgrid.h"
#include "typeface.h"

static llong pow10[19] = {
    /*  0 */ 1ll,
    /*  1 */ 10ll,
    /*  2 */ 100ll,
    /*  3 */ 1000ll,
    /*  4 */ 10000ll,
    /*  5 */ 100000ll,
    /*  6 */ 1000000ll,
    /*  7 */ 10000000ll,
    /*  8 */ 100000000ll,
    /*  9 */ 1000000000ll,
    /* 10 */ 10000000000ll,
    /* 11 */ 100000000000ll,
    /* 12 */ 1000000000000ll,
    /* 13 */ 10000000000000ll,
    /* 14 */ 100000000000000ll,
    /* 15 */ 1000000000000000ll,
    /* 16 */ 10000000000000000ll,
    /* 17 */ 100000000000000000ll,
    /* 18 */ 1000000000000000000ll,
};

enum tty_cellgrid_field_id
{
    tty_cellgrid_field_timestamp,
    tty_cellgrid_field_linedata
};

struct tty_cellgrid_field
{
    tty_cellgrid_field_id field_id;
    float field_width;
};

/* visual cell span, row is visible line before wrap */
struct tty_cellgrid_ref { float row; float col; };
struct tty_cellgrid_span { tty_cellgrid_ref start, end; };

static const tty_cellgrid_ref null_cellgrid_ref = { 1.f/0.f, 1.f/0.f };

inline bool operator== (const tty_cellgrid_ref &a, const tty_cellgrid_ref &b)
{ return std::tie(a.row, a.col) == std::tie(b.row, b.col); }

struct tty_cellgrid_impl : tty_cellgrid
{
    tty_teletype *tty;
    font_manager_ft *manager;
    tty_font_metric fm;
    tty_font_metric fmc;
    tty_style style;
    const char* text_lang;
    font_face *mono1_emoji;
    font_face *mono1_regular;
    font_face *mono1_bold;
    font_face *mono1_condensed_regular;
    font_face *mono1_condensed_bold;
    int flags;
    ui9::Root root;
    MVGCanvas canvas;
    ui9::Scroller *vscroll;
    ui9::Scroller *hscroll;
    bool in_select;
    tty_cellgrid_span vsel;

    static constexpr float column_padding = 5.0f;
    static const uint linenumber_fgcolor = 0xff484848;
    static const uint linenumber_bgcolor = 0xffe8e8e8;
    static const tty_cellgrid_face linenumber_face = tty_cellgrid_face_condensed_regular;
    static const int linenumber_width = 9;
    static const uint timestamp_fgcolor = 0xff484848;
    static const uint timestamp_bgcolor = 0xffe8e8e8;
    static const tty_cellgrid_face timestamp_face = tty_cellgrid_face_condensed_regular;
    static const tty_timestamp_fmt timestamp_format = tty_timestamp_fmt_iso_datetime_us;

    tty_cellgrid_impl(font_manager_ft *manager, tty_teletype *tty, bool test_mode);

    void draw_background(draw_list &batch);
    void draw_timestamps(draw_list &batch, tty_winsize ws, float ox, float oy, float field_width);
    void draw_linenumbers(draw_list &batch, tty_winsize ws, float ox, float oy, float field_width);
    void draw_cellgrid(draw_list &batch, tty_winsize ws, float ox, float oy, float field_width);
    void draw_cursor(draw_list &batch, tty_winsize ws, float ox, float oy, float field_width);
    void draw_scrollbars(draw_list &batch);

    virtual void draw(draw_list &batch);
    virtual void write_sbox(std::string filename);
    virtual bool has_flag(uint f);
    virtual void set_flag(uint f, bool val);
    virtual const char* get_lang();
    virtual tty_style get_style();
    virtual void set_style(tty_style s);
    virtual tty_winsize get_winsize();
    virtual tty_font_metric get_font_metric();
    virtual font_face* get_font_face(tty_cellgrid_face face);

    virtual font_manager_ft* get_manager();
    virtual tty_teletype* get_teletype();
    virtual MVGCanvas* get_canvas();
    virtual ui9::Root* get_root();
    virtual void update_scroll();
    virtual bool mouse_event(ui9::MouseEvent *e);

    void scroll_event(ui9::axis_2D axis, float val);

    font_face* cell_font(tty_cell &cell);
    tty_cell_ref vcell_to_lcell(tty_cellgrid_ref cell);
    tty_cell cell_col(tty_cell &cell);
    void draw_loop(int rows, int cols,
        std::function<void(tty_line&,size_t,size_t,size_t,size_t)> linepre_cb,
        std::function<void(tty_cell&,size_t,size_t,size_t,size_t)> cell_cb,
        std::function<void(tty_line&,size_t,size_t,size_t,size_t)> linepost_cb);
};


tty_cellgrid_impl::tty_cellgrid_impl(font_manager_ft *manager, tty_teletype *tty, bool test_mode)
    : tty(tty), manager(manager), root(manager), canvas(manager)
{
    if (test_mode) {
        style = {
            1200.f, 800.f, 0.f, 25.f, 1.f,
            0xffffffff, 0x40000000, 0xffd8d8d8, 0xffe8e8e8
        };
    } else {
        #if defined __APPLE__
        style = {
            //630.f, 440.f, 15.f, 12.5f, 1.f,
            800.f, 440.f, 15.f, 12.5f, 1.f,
            //1020.f, 440.f, 15.f, 12.5f, 1.f,
            0xffe8e8e8, 0x40000000, 0xffd8d8d8, 0xffe8e8e8
        };
        #else
        style = {
            1230.f, 850.f, 15.f, 25.0f, 1.f,
            0xffe8e8e8, 0x40000000, 0xffd8d8d8, 0xffe8e8e8
        };
        #endif
    }
    text_lang = "en";
    flags = tty_cellgrid_background | tty_cellgrid_focused;

    vscroll = new ui9::Scroller();
    vscroll->set_orientation(ui9::axis_2D::vertical);
    vscroll->set_callback([&](float val) {
        scroll_event(ui9::axis_2D::vertical, val);
    });
    root.add_child(vscroll);

    hscroll = new ui9::Scroller();
    hscroll->set_orientation(ui9::axis_2D::horizontal);
    hscroll->set_callback([&](float val) {
        scroll_event(ui9::axis_2D::horizontal, val);
    });
    root.add_child(hscroll);

    /* fetch our font */
    mono1_emoji = tty_typeface_get_font(manager, tty_cellgrid_face_emoji);
    mono1_emoji->flags |= font_face_color;
    mono1_regular = tty_typeface_get_font(manager, tty_cellgrid_face_regular);
    mono1_bold = tty_typeface_get_font(manager, tty_cellgrid_face_bold);
    mono1_condensed_regular = tty_typeface_get_font(manager, tty_cellgrid_face_condensed_regular);
    mono1_condensed_bold = tty_typeface_get_font(manager, tty_cellgrid_face_condensed_bold);

    fm = tty_typeface_get_metrics(mono1_regular, style.font_size, 'M');
    fmc = tty_typeface_get_metrics(mono1_condensed_regular, style.font_size, 'M');
    tty_typeface_print_metrics(mono1_regular, fm);

    in_select = false;
    vsel = { null_cellgrid_ref, null_cellgrid_ref };
}

tty_cellgrid* tty_cellgrid_new(font_manager_ft *manager, tty_teletype *tty, bool test_mode)
{
    return new tty_cellgrid_impl(manager, tty, test_mode);
}

bool tty_cellgrid_impl::has_flag(uint f)
{
    return (flags & f) == f;
}

void tty_cellgrid_impl::set_flag(uint f, bool val)
{
    if (val) flags |= f;
    else flags &= ~f;
}

font_face* tty_cellgrid_impl::get_font_face(tty_cellgrid_face face)
{
    switch (face) {
    case tty_cellgrid_face_emoji: return mono1_emoji;
    case tty_cellgrid_face_regular: return mono1_regular;
    case tty_cellgrid_face_bold: return mono1_bold;
    case tty_cellgrid_face_condensed_regular: return mono1_condensed_regular;
    case tty_cellgrid_face_condensed_bold: return mono1_condensed_bold;
    }
    return nullptr;
}

static float timestamp_field_width(tty_timestamp_fmt fmt,
    tty_font_metric &fm, float available_width)
{
    float width = timestamp_isostring(fmt, NULL, 0, NULL) * fm.advance;
    return std::min(available_width, width);
}

static float linenumber_field_width(int linenumber_width,
    tty_font_metric &fm, float available_width)
{
    float width = linenumber_width * fm.advance;
    return std::min(available_width, width);
}

tty_winsize tty_cellgrid_impl::get_winsize()
{
    int rows = 0, cols = 0;

    float available_height = style.height - style.margin * 2.f;
    float available_width = style.width - style.margin * 2.f;

    if ((flags & tty_cellgrid_linenumbers) > 0) {
        float field_width = linenumber_field_width(linenumber_width, fmc, available_width);
        available_width = std::max(0.f, available_width - (field_width + column_padding));
    }

    if ((flags & tty_cellgrid_timestamps) > 0) {
        float field_width = timestamp_field_width(timestamp_format, fmc, available_width);
        available_width = std::max(0.f, available_width - (field_width + column_padding));
    }

    rows = (int)std::max(0.f, available_height) / fm.leading;
    cols = (int)std::max(0.f, available_width) / fm.advance;

    cols = std::max(20, cols);

    return tty_winsize { rows, cols, (int)available_width, (int)available_height };
}

const char* tty_cellgrid_impl::get_lang() { return text_lang; }
tty_style tty_cellgrid_impl::get_style() { return style; }
void tty_cellgrid_impl::set_style(tty_style s) { style = s; }
tty_font_metric tty_cellgrid_impl::get_font_metric() { return fm; }
font_manager_ft* tty_cellgrid_impl::get_manager() { return manager; }
tty_teletype* tty_cellgrid_impl::get_teletype() { return tty; }
MVGCanvas* tty_cellgrid_impl::get_canvas() { return &canvas; }
ui9::Root* tty_cellgrid_impl::get_root() { return &root; }

font_face* tty_cellgrid_impl::cell_font(tty_cell &cell)
{
    font_face *face;

    if (cell.codepoint >= 0x1f000 && cell.codepoint <= 0x1ffff) {
        face = mono1_emoji;
    }
    else if (cell.flags & tty_cell_bold) {
        face = mono1_bold;
    }
    else {
        face = mono1_regular;
    }

    return face;
}

tty_cell_ref tty_cellgrid_impl::vcell_to_lcell(tty_cellgrid_ref vcell)
{
    llong row = floorf(vcell.row), vcol = floorf(vcell.col);
    llong visible_rows = tty->visible_rows(), total_rows = tty->total_rows();
    llong offset = total_rows < visible_rows ? visible_rows - total_rows : 0;
    tty_log_loc loff = tty->visible_to_logical(row + offset);
    tty_line line = tty->get_line(loff.lline);
    return { loff.lline, std::min(loff.loff + vcol, (llong)line.cells.size()) };
}

tty_cell tty_cellgrid_impl::cell_col(tty_cell &cell)
{
    uint fg = cell.fg;
    uint bg = cell.bg;

    if ((cell.flags & tty_cell_faint) > 0) {
        color col = color(cell.fg);
        col = col.blend(color(0.5f,0.5f,0.5f,1.f), 0.5f);
        fg = col.rgba32();
    }

    if ((cell.flags & tty_cell_inverse) > 0) {
        return tty_cell{ 0, 0, bg, fg };
    } else {
        return tty_cell{ 0, 0, fg, bg };
    }
}

void tty_cellgrid_impl::draw_loop(int rows, int cols,
    std::function<void(tty_line&,size_t,size_t,size_t,size_t)> linepre_cb,
    std::function<void(tty_cell&,size_t,size_t,size_t,size_t)> cell_cb,
    std::function<void(tty_line&,size_t,size_t,size_t,size_t)> linepost_cb)
{
    llong total_rows = tty->total_rows();
    llong scroll_row = tty->scroll_row();
    llong offset = total_rows < rows ? rows - total_rows : 0;
    for (llong j = total_rows - 1 - scroll_row + offset, l = 0; l < rows; j--, l++)
    {
        if (j < 0 || j >= total_rows) continue;

        tty_log_loc loff = tty->visible_to_logical(j);
        size_t k = loff.lline, o = loff.loff;
        tty_line line = tty->get_line(k);
        size_t limit = std::min(o + cols, line.cells.size());

        linepre_cb(line, k, l, o, o);
        for (size_t i = o; i < limit; i++) {
            tty_cell &cell = line.cells[i];
            cell_cb(cell, k, l, o, i);
        }
        linepost_cb(line, k, l, o, limit);
    }
}

static void rect(draw_list &batch, float y, float x, float h, float w, uint c)
{
    float u1 = 0.f, v1 = 0.f, u2 = 0.f, v2 = 0.f;
    float x2 = x, x1 = x2 + w, y1 = y, y2 = y1 - h;
    uint o0 = draw_list_vertex(batch, {{x1, y1, 0}, {u1, v1}, c});
    uint o1 = draw_list_vertex(batch, {{x2, y1, 0}, {u2, v1}, c});
    uint o2 = draw_list_vertex(batch, {{x2, y2, 0}, {u2, v2}, c});
    uint o3 = draw_list_vertex(batch, {{x1, y2, 0}, {u1, v2}, c});
    draw_list_indices(batch, image_none, mode_triangles,
        shader_flat, {o0, o3, o1, o1, o3, o2});
};

void tty_cellgrid_impl::draw_background(draw_list &batch)
{
    color white = color(1.0f, 1.0f, 1.0f, 1.0f);
    color black = color(0.0f, 0.0f, 0.0f, 1.0f);
    float w = 2.0f, m = style.margin/2.f + w;
    float tx = style.width/2.0f;
    float ty = style.height/2.0f;
    canvas.clear();
    canvas.set_fill_brush(MVGBrush{MVGBrushSolid, { }, { white }});
    canvas.set_stroke_brush(MVGBrush{MVGBrushSolid, { }, { black }});
    canvas.set_stroke_width(w);
    canvas.new_rounded_rectangle(vec2(tx, ty), vec2(tx - m, ty - m), m);
    canvas.emit(batch);
}

void tty_cellgrid_impl::draw_timestamps(draw_list &batch, tty_winsize ws,
    float ox, float oy, float field_width)
{
    text_renderer_ft renderer(manager, style.rscale);
    std::vector<glyph_shape> shapes;

    int rows = ws.vis_rows;
    int fit_cols = (int)roundf(std::max(0.f, field_width) / fmc.advance);
    int font_size = (int)(fm.size * 64.0f);
    float glyph_height = fm.height - fm.descender;
    float y_offset = floorf((fm.leading - glyph_height)/2.f) + fm.descender;

    auto render_text = [&](float x, float y, font_face *face)
    {
        text_segment segment("", text_lang, face, font_size, x, y-y_offset, 0);
        renderer.render(batch, shapes, segment);
        shapes.clear();
    };

    draw_loop(rows, 1,
        [&] (auto line, auto k, auto l, auto o, auto i) {
            if (o != 0) return;
            if (line.tv.vec[0] == 0 && line.tv.vec[1] == 0 && line.tv.vec[2] == 0) return;
            font_face *face = get_font_face(timestamp_face);
            char buf[32];
            int len = timestamp_isostring(timestamp_format,
                buf, sizeof(buf), &line.tv);
            for (int i = 0; i < std::min(fit_cols,len); i++) {
                uint glyph = tty_typeface_lookup_glyph(face, buf[i]);
                int advance_cx = (int)(fmc.advance * 64.0f);
                shapes.push_back({
                    glyph, (unsigned)i, 0, 0, advance_cx, 0, timestamp_fgcolor
               });
            }
            rect(batch, oy - l * fm.leading, ox, fm.leading, field_width, timestamp_bgcolor);
            render_text(ox, oy - l * fm.leading, face);
        },
        [&] (auto cell, auto k, auto l, auto o, auto i) {},
        [&] (auto line, auto k, auto l, auto o, auto i) {}
    );
}

void tty_cellgrid_impl::draw_linenumbers(draw_list &batch, tty_winsize ws,
    float ox, float oy, float field_width)
{
    text_renderer_ft renderer(manager, style.rscale);
    std::vector<glyph_shape> shapes;

    int rows = ws.vis_rows;
    int fit_cols = (int)roundf(std::max(0.f, field_width) / fmc.advance);
    int font_size = (int)(fm.size * 64.0f);
    float glyph_height = fm.height - fm.descender;
    float y_offset = floorf((fm.leading - glyph_height)/2.f) + fm.descender;

    auto render_text = [&](float x, float y, font_face *face)
    {
        text_segment segment("", text_lang, face, font_size, x, y-y_offset, 0);
        renderer.render(batch, shapes, segment);
        shapes.clear();
    };

    draw_loop(rows, 1,
        [&] (auto line, auto k, auto l, auto o, auto i) {
            if (o != 0) return;
            font_face *face = get_font_face(linenumber_face);
            char buf[32];
            llong linenumber = (k + 1) % pow10[linenumber_width];
            snprintf(buf, sizeof(buf), "%*llu", linenumber_width, linenumber);
            for (int i = 0; i < std::min(fit_cols,linenumber_width); i++) {
                uint glyph = tty_typeface_lookup_glyph(face, buf[i]);
                int advance_cx = (int)(fmc.advance * 64.0f);
                shapes.push_back({
                    glyph, (unsigned)i, 0, 0, advance_cx, 0, linenumber_fgcolor
               });
            }
            rect(batch, oy - l * fm.leading, ox, fm.leading, field_width, linenumber_bgcolor);
            render_text(ox, oy - l * fm.leading, face);
        },
        [&] (auto cell, auto k, auto l, auto o, auto i) {},
        [&] (auto line, auto k, auto l, auto o, auto i) {}
    );
}

void tty_cellgrid_impl::draw_cellgrid(draw_list &batch, tty_winsize ws,
    float ox, float oy, float field_width)
{
    text_renderer_ft renderer(manager, style.rscale);
    std::vector<glyph_shape> shapes;

    int rows = ws.vis_rows, cols = ws.vis_cols;
    int fit_cols = (int)floorf(std::max(0.f, field_width) / fm.advance);
    int font_size = (int)(fm.size * 64.0f);
    float glyph_height = fm.height - fm.descender;
    float y_offset = floorf((fm.leading - glyph_height)/2.f) + fm.descender;

    auto render_block = [&](tty_font_metric &fm, int row, int col, int h, int w, uint c)
    {
        rect(batch, oy - row * fm.leading, ox + col * fm.advance,
            h * fm.leading, w * fm.advance, c);
    };

    auto render_underline = [&](tty_font_metric &fm, int row, int col, int w, uint c)
    {
        float lw = fm.advance * w, sw = 2.0f;
        float x1 = ox + col * fm.advance, x2 = x1 + lw;
        float y = oy - row * fm.leading - (y_offset + fm.underline_position - sw);
        canvas.set_stroke_width(sw);
        canvas.set_fill_brush(MVGBrush{MVGBrushNone, { }, { }});
        canvas.set_stroke_brush(MVGBrush{MVGBrushSolid, { }, { color(c) }});
        MVGPath *p1 = canvas.new_path({ (x1+x2)*0.5f, y },{lw,sw});
        p1->new_line({0.0f,0.0f}, {lw,0.0f});
    };

    auto render_text = [&](float x, float y, font_face *face)
    {
        text_segment segment("", text_lang, face, font_size, x, y-y_offset, 0);
        renderer.render(batch, shapes, segment);
        shapes.clear();
    };

    tty_cell_span selected = tty->get_selection();

    // todo: add offset adjustment

    auto is_selected = [&](tty_cell_ref cellref) -> bool
    {
        if (selected.start == null_cell_ref && selected.end == null_cell_ref) {
            return false;
        } else if (selected.end > selected.start) {
            return cellref >= selected.start && cellref <= selected.end;
        } else {
            return cellref >= selected.end && cellref <= selected.start;
        }
    };

    /* render background colors */
    draw_loop(rows, fit_cols,
        [&] (auto line, auto k, auto l, auto o, auto i) {},
        [&] (auto cell, auto k, auto l, auto o, auto i) {
            tty_cell_ref cellref = { (llong)k, (llong)i };
            uint bg = is_selected(cellref) ?
                has_flag(tty_cellgrid_focused) ?
                style.select_focus_color :
                style.select_nofocus_color :
                cell_col(cell).bg;
            render_block(fm, l, i-o, 1, 1, bg);
        },
        [&] (auto line, auto k, auto l, auto o, auto i) {}
    );

    /* render text */
    draw_loop(rows, fit_cols,
        [&] (auto line, auto k, auto l, auto o, auto i) {},
        [&] (auto cell, auto k, auto l, auto o, auto i) {
            font_face *face = cell_font(cell);
            uint glyph = tty_typeface_lookup_glyph(face, cell.codepoint);
            int advance_x = (int)(fm.advance * 64.0f);
            shapes.push_back({
                glyph, (unsigned)o, 0, 0, advance_x, 0, cell_col(cell).fg
            });
            render_text(ox + (i-o) * fm.advance, oy - l * fm.leading, face);
        },
        [&] (auto line, auto k, auto l, auto o, auto i) {}
    );

    /* render underline */
    bool u, lu;
    uint lou, fg, lfg;
    draw_loop(rows, fit_cols,
        [&] (auto line, auto k, auto l, auto o, auto i) {
            lou = 0;
            lu = u = false;
            lfg = fg = 0;
        },
        [&] (auto cell, auto k, auto l, auto o, auto i) {
            u = (cell.flags & tty_cell_underline) > 0;
            fg = cell_col(cell).fg;
            if ((i-o)-lou > 0 && (u != lu || fg != lfg)) {
                if (lu) render_underline(fm, l, lou, (i-o)-lou, lfg);
            }
            if (u != lu || fg != lfg) {
                if (u) lou = i-o;
            }
            lfg = fg;
            lu = u;
        },
        [&] (auto line, auto k, auto l, auto o, auto i) {
            if ((i-o)-lou > 0) {
                if (lu) render_underline(fm, l, lou, (i-o)-lou, lfg);
            }
        }
    );

    canvas.emit(batch);
}

void tty_cellgrid_impl::draw_cursor(draw_list &batch, tty_winsize ws,
    float ox, float oy, float field_width)
{
    int rows = ws.vis_rows;
    int fit_cols = (int)floorf(std::max(0.f, field_width) / fm.advance);
    int lline = tty->cursor_line(), loff = tty->cursor_offset();

    auto render_block = [&](tty_font_metric &fm, int row, int col, int h, int w, uint c)
    {
        rect(batch, oy - row * fm.leading, ox + col * fm.advance,
            h * fm.leading, w * fm.advance, c);
    };

    auto render_rect = [&](tty_font_metric &fm, int row, int col, int h, int w, uint c)
    {
        float sw = 2.0f;
        float x1 = ox + col * fm.advance, x2 = x1 + w * fm.advance;
        float y2 = oy - row * fm.leading, y1 = y2 - h * fm.leading;
        canvas.set_stroke_width(sw);
        canvas.set_fill_brush(MVGBrush{MVGBrushNone, { }, { }});
        canvas.set_stroke_brush(MVGBrush{MVGBrushSolid, { }, { color(c) }});
        MVGRect *r = canvas.new_rectangle({(x1+x2)*0.5f, (y1+y2)*0.5f},{(x2-x1)*0.5f, (y2-y1)*0.5f});
    };

    /* render cursor */
    draw_loop(rows, fit_cols,
        [&] (auto line, auto k, auto l, auto o, auto i) {
            if (lline == k && loff >= o && loff < o + fit_cols) {
                if (has_flag(tty_cellgrid_focused)) {
                    render_block(fm, l, loff - o, 1, 1, style.cursor_color);
                } else {
                    render_rect(fm, l, loff - o, 1, 1, style.cursor_color);
                }
            }
        },
        [&] (auto cell, auto k, auto l, auto o, auto i) {},
        [&] (auto line, auto k, auto l, auto o, auto i) {}
    );
}

void tty_cellgrid_impl::draw_scrollbars(draw_list &batch)
{
    vscroll->set_visible(true);
    vscroll->set_position({style.width-20, style.height/2, 0});
    vscroll->set_preferred_size({15, style.height - style.margin, 0});

    hscroll->set_visible(false);
    hscroll->set_position({style.width/2, style.height-20, 0});
    hscroll->set_preferred_size({style.width - style.margin, 15, 0});

    root.layout(&canvas);
    canvas.emit(batch);
}

void tty_cellgrid_impl::draw(draw_list &batch)
{
    text_renderer_ft renderer(manager, style.rscale);
    std::vector<glyph_shape> shapes;

    tty->update_offsets();

    /* set up scale/translate matrix */
    float s = 1.0f;
    canvas.set_transform(mat3(s,  0,  0,
                              0,  s,  0,
                              0,  0,  1));
    canvas.set_scale(0.5f);

    float ox = style.margin;
    float oy = style.height - style.margin;
    float available_width = style.width - style.margin * 2.f;

    tty_winsize ws = get_winsize();

    if ((flags & tty_cellgrid_background) > 0) {
        draw_background(batch);
    }

    if ((flags & tty_cellgrid_linenumbers) > 0) {
        float field_width = linenumber_field_width(linenumber_width, fmc, available_width);
        draw_linenumbers(batch, ws, ox, oy, field_width);
        available_width = std::max(0.f, available_width - (field_width + column_padding));
        ox += field_width + column_padding;
    }

    if ((flags & tty_cellgrid_timestamps) > 0) {
        float field_width = timestamp_field_width(timestamp_format, fmc, available_width);
        draw_timestamps(batch, ws, ox, oy, field_width);
        available_width = std::max(0.f, available_width - (field_width + column_padding));
        ox += field_width + column_padding;
    }

    draw_cellgrid(batch, ws, ox, oy, available_width);

    if (tty->has_flag(tty_flag_DECTCEM)) {
        draw_cursor(batch, ws, ox, oy, available_width);
    }

    if ((flags & tty_cellgrid_scrollbars) > 0) {
        draw_scrollbars(batch);
    }
}

void tty_cellgrid_impl::write_sbox(std::string filename)
{
    FILE *out;
    tty_winsize ws = get_winsize();
    int rows = ws.vis_rows, cols = ws.vis_cols;
    std::vector<std::string> lines(rows);

    /* extract screen layout */
    llong total_rows = tty->total_rows();
    llong scroll_row = tty->scroll_row();
    llong offset = total_rows < rows ? rows - total_rows : 0;
    for (llong j = total_rows - 1 - scroll_row + offset, l = 0; l < rows; j--, l++)
    {
        if (j < 0 || j >= total_rows) continue;

        tty_log_loc loff = tty->visible_to_logical(j);
        size_t k = loff.lline, o = loff.loff;
        tty_line line = tty->get_line(k);
        size_t limit = std::min(o + cols, line.cells.size());

        for (size_t i = o; i < limit; i++) {
            tty_cell &cell = line.cells[i];
            char u[8];
            size_t b = utf32_to_utf8(u, sizeof(u), cell.codepoint);
            lines[rows-l-1].append(std::string(u, b));
        }
    }

    /* write sbox file */
    if ((out = fopen(filename.c_str(), "wb")) == NULL) {
        Panic("write_sbox: error: %s\n", strerror(errno));
    }
    for (auto i = lines.begin(); i != lines.end(); i++)
    {
        auto si = std::find_if(i->begin(), i->end(),
            [](int c) {return !std::isspace(c);});
        auto ei = std::find_if(i->rbegin(), i->rend(),
            [](int c) {return !std::isspace(c);}).base();
        if (std::distance(si, ei) > 0) {
            fprintf(out, "%zu,%zu \"%s\"\n",
                1 + std::distance(lines.begin(), i),
                1 + std::distance(i->begin(), si),
                std::string(si, ei).c_str());
        }
    }
    fclose(out);
}

static GLFWcursor *ibeam_cursor = NULL;

bool tty_cellgrid_impl::mouse_event(ui9::MouseEvent *me)
{
    float ox = style.margin;
    float oy = style.height - style.margin;
    float available_width = style.width - style.margin * 2.f;

    if ((flags & tty_cellgrid_linenumbers) > 0) {
        float field_width = linenumber_field_width(linenumber_width, fmc, available_width);
        ox += field_width + column_padding;
    }

    if ((flags & tty_cellgrid_timestamps) > 0) {
        float field_width = timestamp_field_width(timestamp_format, fmc, available_width);
        ox += field_width + column_padding;
    }

    llong visible_rows = tty->visible_rows();
    llong total_rows = tty->total_rows();
    llong scroll_row = tty->scroll_row();
    llong scroll_col = tty->scroll_col();
    llong vrange = tty->scroll_row_limit();

    vec2 v = { me->pos.x - ox, oy - me->pos.y };
    vec2 p = { v.x / fm.advance, v.y / fm.leading };
    tty_cellgrid_ref vcell = { total_rows - scroll_row - p.y, scroll_col + p.x };
    llong new_scroll_row = scroll_row;

    switch (me->header.qualifier) {
    case ui9::pressed:
        vsel = { vcell, vcell };
        in_select = true;
        break;
    case ui9::motion:
        if (in_select) {
            if (p.y < 0) {
                new_scroll_row = std::max(scroll_row - 1, 0ll);
            } else if (p.y > visible_rows) {
                new_scroll_row = std::min(scroll_row + 1, vrange);
            }
        }
        [[fallthrough]];
    case ui9::released:
        vsel = { vsel.start, in_select ? vcell : vsel.end };
        if (me->header.qualifier == ui9::released) in_select = false;
        break;
    case ui9::wheel:
        if (me->pos.y < 0.f) {
            new_scroll_row = std::max(scroll_row + (int)(me->pos.y) - 1, 0ll);
        } else if (me->pos.y > 0.f) {
            new_scroll_row = std::min(scroll_row + (int)(me->pos.y) + 1, vrange);
        }
        break;
    }

    tty_cell_span lsel = { vcell_to_lcell(vsel.start), vcell_to_lcell(vsel.end) };

    if (lsel.start < lsel.end) {
        if (fmodf(vsel.start.col, 1.f) > 0.5f) lsel.start.col++;
        if (fmodf(vsel.end.col, 1.f) < 0.5f) lsel.end.col--;
        if (lsel.start > lsel.end) lsel = { null_cell_ref, null_cell_ref };
    } else {
        if (fmodf(vsel.end.col, 1.f) > 0.5f) lsel.end.col++;
        if (fmodf(vsel.start.col, 1.f) < 0.5f) lsel.start.col--;
        if (lsel.start < lsel.end) lsel = { null_cell_ref, null_cell_ref };
    }

    tty->set_scroll_row(new_scroll_row);
    tty->set_selection(lsel);

    return true;
}

void tty_cellgrid_impl::update_scroll()
{
    float vscroll_val = (float)tty->scroll_row() / tty->scroll_row_limit();
    float hscroll_val = (float)tty->scroll_col() / tty->scroll_col_limit();
    vscroll->set_value(vscroll_val);
    hscroll->set_value(hscroll_val);
}

void tty_cellgrid_impl::scroll_event(ui9::axis_2D axis, float val)
{
    val = std::min(1.0f, std::max(0.0f, val));
    switch (axis) {
    case ui9::axis_2D::vertical:
        tty->set_scroll_row((size_t)(val * (float)tty->scroll_row_limit()));
        break;
    case ui9::axis_2D::horizontal:
        tty->set_scroll_col((size_t)(val * (float)tty->scroll_col_limit()));
        break;
    }
}
