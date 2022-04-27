#include <cstdio>
#include <cstring>
#include <cerrno>

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
#include "ui9.h"
#include "app.h"

#include "terminal.h"
#include "cellgrid.h"
#include "typeface.h"

struct cu_cellgrid_ui9 : cu_cellgrid
{
    cu_term *term;
    font_manager_ft *manager;

    ui9::Root root;
    MVGCanvas canvas;
    ui9::Scroller *vscroll;
    ui9::Scroller *hscroll;

    cu_cellgrid_ui9(font_manager_ft *manager, cu_term *term, bool test_mode);

    virtual cu_winsize visible();
    virtual cu_winsize draw(draw_list &batch);

    virtual font_manager_ft* get_manager();
    virtual cu_term* get_terminal();
    virtual MVGCanvas* get_canvas();
    virtual ui9::Root* get_root();

    void scroll_event(ui9::axis_2D axis, float val);

    font_face_ft* cell_font(cu_cell &cell);
    cu_cell cell_col(cu_cell &cell);
    cu_winsize draw_loop(int rows, int cols,
        std::function<void(cu_line &line,size_t k,size_t l,size_t o,size_t i)> linepre_cb,
        std::function<void(cu_cell &cell,size_t k,size_t l,size_t o,size_t i)> cell_cb,
        std::function<void(cu_line &line,size_t k,size_t l,size_t o,size_t i)> linepost_cb);
};


cu_cellgrid_ui9::cu_cellgrid_ui9(font_manager_ft *manager, cu_term *term, bool test_mode)
    : term(term), manager(manager), root(manager), canvas(manager)
{
    if (test_mode) {
        width = 1200;
        height = 800;
        font_size = 25.0f;
        margin = 0.0f;
        background_color = 0xffffffff;
    } else {
        #if defined __APPLE__
            width = 630;
            height = 440;
            font_size = 12.5f;
            margin = 15.0f;
        #else
            width = 1260;
            height = 860;
            font_size = 25.0f;
            margin = 30.0f;
        #endif
        background_color = 0xffe8e8e8;
    }
    cursor_color = 0x40000000;
    text_lang = "en";
    rscale = 1.0f;
    flags = cu_cellgrid_background;
    vdelta = 0;

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

    cu_typeface_init(this);
}

cu_cellgrid* cu_cellgrid_new(font_manager_ft *manager, cu_term *term, bool test_mode)
{
    return new cu_cellgrid_ui9(manager, term, test_mode);
}

font_manager_ft* cu_cellgrid_ui9::get_manager() { return manager; }
cu_term* cu_cellgrid_ui9::get_terminal() { return term; }
MVGCanvas* cu_cellgrid_ui9::get_canvas() { return &canvas; }
ui9::Root* cu_cellgrid_ui9::get_root() { return &root; }

font_face_ft* cu_cellgrid_ui9::cell_font(cu_cell &cell)
{
    font_face *face;

    if (cell.codepoint >= 0x1f000 && cell.codepoint <= 0x1ffff) {
        face = mono1_emoji;
    }
    else if (cell.flags & cu_cell_bold) {
        face = mono1_bold;
    }
    else {
        face = mono1_regular;
    }

    return static_cast<font_face_ft*>(face);
}

cu_cell cu_cellgrid_ui9::cell_col(cu_cell &cell)
{
    uint fg = cell.fg;
    uint bg = cell.bg;

    if ((cell.flags & cu_cell_faint) > 0) {
        color col = color(cell.fg);
        col = col.blend(color(0.5f,0.5f,0.5f,1.f), 0.5f);
        fg = col.rgba32();
    }

    if ((cell.flags & cu_cell_inverse) > 0) {
        return cu_cell{ 0, 0, bg, fg };
    } else {
        return cu_cell{ 0, 0, fg, bg };
    }
}

cu_winsize cu_cellgrid_ui9::visible()
{
    /* vis_lines == vis_rows initially */
    int vis_rows = (int)std::max(0.f, height - margin*2.f) / fm.leading;
    int vis_cols = (int)std::max(0.f, width  - margin*2.f) / fm.advance;
    return cu_winsize { vis_rows, vis_rows, vis_cols };
}

cu_winsize cu_cellgrid_ui9::draw_loop(int rows, int cols,
    std::function<void(cu_line &line,size_t k,size_t l,size_t o,size_t i)> linepre_cb,
    std::function<void(cu_cell &cell,size_t k,size_t l,size_t o,size_t i)> cell_cb,
    std::function<void(cu_line &line,size_t k,size_t l,size_t o,size_t i)> linepost_cb)
{
    int wrap_count = 0;
    bool wrap_enabled = (term->flags & cu_flag_DECAWM) > 0;
    ssize_t tot_lines = wrap_enabled ? term->voffsets.size() : term->lines.size();

    for (ssize_t j = tot_lines - 1 - vdelta, l = 0; l < rows; j--, l++)
    {
        if (j >= tot_lines) continue;
        if (j < 0) break;

        size_t k, o;
        cu_line line;

        if (wrap_enabled) {
            cu_line_voff voff = term->voffsets[j];
            k = voff.lline;
            o = voff.offset;
            if (o > 0) wrap_count++;
        } else {
            k = j;
            o = 0;
        }

        line = term->lines[k];
        line.unpack();
        size_t limit = std::min(o + cols, line.count());
        linepre_cb(line, k, l, o, o);
        for (size_t i = o; i < limit; i++) {
            cu_cell &cell = line.cells[i];
            cell_cb(cell, k, l, o, i);
        }
        linepost_cb(line, k, l, o, limit);
    }

    return cu_winsize{
        rows - wrap_count, rows, cols,
        (int)width, (int)height
    };
}

cu_winsize cu_cellgrid_ui9::draw(draw_list &batch)
{
    text_renderer_ft renderer(manager, rscale);
    std::vector<glyph_shape> shapes;

    cu_term_update_offsets(term);

    int rows = (int)std::max(0.f, height - margin*2.f) / fm.leading;
    int cols = (int)std::max(0.f, width  - margin*2.f) / fm.advance;
    int font_size = (int)(fm.size * 64.0f);
    int advance_x = (int)(fm.advance * 64.0f);
    float glyph_height = fm.height - fm.descender;
    float y_offset = floorf((fm.leading - glyph_height)/2.f) + fm.descender;
    int clrow = term->cur_row, clcol = term->cur_col;
    float ox = margin, oy = height - margin;

    auto render_block = [&](int row, int col, int h, int w, uint c)
    {
        float u1 = 0.f, v1 = 0.f, u2 = 0.f, v2 = 0.f;
        float x2 = ox + col * fm.advance, x1 = x2 + (fm.advance * w);
        float y1 = oy - row * fm.leading, y2 = y1 - (fm.leading * h);
        uint o0 = draw_list_vertex(batch, {{x1, y1, 0}, {u1, v1}, c});
        uint o1 = draw_list_vertex(batch, {{x2, y1, 0}, {u2, v1}, c});
        uint o2 = draw_list_vertex(batch, {{x2, y2, 0}, {u2, v2}, c});
        uint o3 = draw_list_vertex(batch, {{x1, y2, 0}, {u1, v2}, c});
        draw_list_indices(batch, image_none, mode_triangles,
            shader_flat, {o0, o3, o1, o1, o3, o2});
    };

    auto render_underline = [&](int row, int col, int w, uint fg)
    {
        float lw = fm.advance * w, sw = 2.0f;
        float x1 = ox + col * fm.advance, x2 = x1 + lw;
        float y = oy - row * fm.leading - (y_offset + fm.underline_position - sw);
        canvas.set_stroke_width(sw);
        canvas.set_fill_brush(MVGBrush{MVGBrushNone, { }, { }});
        canvas.set_stroke_brush(MVGBrush{MVGBrushSolid, { }, { color(fg) }});
        MVGPath *p1 = canvas.new_path({0.0f,0.0f},{lw,sw});
        p1->pos = { (x1+x2)*0.5f, y };
        p1->new_line({0.0f,0.0f}, {lw,0.0f});
    };

    auto render_text = [&](float x, float y, font_face_ft *face)
    {
        text_segment segment("", text_lang, face, font_size, x, y-y_offset, 0);
        renderer.render(batch, shapes, segment);
        shapes.clear();
    };

    /* set up scale/translate matrix */
    float s = 1.0f;
    float tx = width/2.0f;
    float ty = height/2.0f;
    canvas.set_transform(mat3(s,  0,  0,
                                  0,  s,  0,
                                  0,  0,  1));
    canvas.set_scale(rscale);

    /* render border */

    if ((flags & cu_cellgrid_background) > 0) {
        color white = color(1.0f, 1.0f, 1.0f, 1.0f);
        color black = color(0.0f, 0.0f, 0.0f, 1.0f);
        float w = 2.0f, m = margin/2.f + w;
        float tx = width/2.0f;
        float ty = height/2.0f;
        canvas.clear();
        canvas.set_fill_brush(MVGBrush{MVGBrushSolid, { }, { white }});
        canvas.set_stroke_brush(MVGBrush{MVGBrushSolid, { }, { black }});
        canvas.set_stroke_width(w);
        canvas.new_rounded_rectangle(vec2(tx, ty), vec2(tx - m, ty - m), m);
        canvas.emit(batch);
    }

    /* render background colors */

    cu_winsize dim = draw_loop(rows, cols,
        [&] (auto line, auto k, auto l, auto o, auto i) {},
        [&] (auto cell, auto k, auto l, auto o, auto i) {
            uint bg = cell_col(cell).bg;
            render_block(l, i-o, 1, 1, bg);
        },
        [&] (auto line, auto k, auto l, auto o, auto i) {}
    );

    /* render text */

    draw_loop(rows, cols,
        [&] (auto line, auto k, auto l, auto o, auto i) {},
        [&] (auto cell, auto k, auto l, auto o, auto i) {
            font_face_ft *face = cell_font(cell);
            uint glyph = cu_typeface_lookup_glyph(face, cell.codepoint);
            shapes.push_back({
                glyph, (unsigned)o, 0, 0, advance_x, 0, cell_col(cell).fg
            });
            render_text(ox + (i-o) * fm.advance, oy - l * fm.leading, face);
        },
        [&] (auto line, auto k, auto l, auto o, auto i) {}
    );

    /* render underline */

    uint lounder;
    bool under, lunder;
    uint fg, lfg;

    draw_loop(rows, cols,
        [&] (auto line, auto k, auto l, auto o, auto i) {
            lounder = 0;
            lunder = under = false;
            lfg = fg = 0;
        },
        [&] (auto cell, auto k, auto l, auto o, auto i) {
            under = (cell.flags & cu_cell_underline) > 0;
            fg = cell_col(cell).fg;
            if ((i-o)-lounder > 0 && (under != lunder || fg != lfg)) {
                if (lunder) render_underline(l, lounder, (i-o)-lounder, lfg);
            }
            if (under != lunder || fg != lfg) {
                if (under) lounder = i-o;
            }
            lfg = fg;
            lunder = under;
        },
        [&] (auto line, auto k, auto l, auto o, auto i) {
            if ((i-o)-lounder > 0) {
                if (lunder) render_underline(l, lounder, (i-o)-lounder, lfg);
            }
        }
    );

    canvas.emit(batch);

    /* render cursor */

    if ((term->flags & cu_flag_DECTCEM) > 0) {
        draw_loop(rows, cols,
            [&] (auto line, auto k, auto l, auto o, auto i) {
                if (clrow == k && clcol >= o && clcol < o + cols) {
                    render_block(l, clcol - o, 1, 1, cursor_color);
                }
            },
            [&] (auto cell, auto k, auto l, auto o, auto i) {},
            [&] (auto line, auto k, auto l, auto o, auto i) {}
        );
    }

    canvas.emit(batch);

    /* render scrollbars */

    if ((flags & cu_cellgrid_background) > 0)
    {
        vscroll->set_visible(true);
        vscroll->set_position({width-20, height/2, 0});
        vscroll->set_preferred_size({15, height - margin, 0});

        hscroll->set_visible(false);
        hscroll->set_position({width/2, height-20, 0});
        hscroll->set_preferred_size({width - margin, 15, 0});

        root.layout(&canvas);
        canvas.emit(batch);
    }

    return dim;
}

void cu_cellgrid_ui9::scroll_event(ui9::axis_2D axis, float val)
{
    ssize_t vis_lines = term->vis_lines;
    bool wrap_enabled = (term->flags & cu_flag_DECAWM) > 0;
    ssize_t tot_lines = wrap_enabled ? term->voffsets.size() : term->lines.size();;
    ssize_t vrange = tot_lines - vis_lines > 0 ? tot_lines - vis_lines : 0;

    switch (axis) {
    case ui9::axis_2D::vertical:
        vdelta = (size_t)(val * (float)vrange);
        break;
    case ui9::axis_2D::horizontal:
        //hdelta = (size_t)(val * (float)hrange);
        break;
    }
}
