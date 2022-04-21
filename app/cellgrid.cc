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
#include "app.h"

#include "terminal.h"
#include "cellgrid.h"
#include "typeface.h"

cu_cellgrid* cu_cellgrid_new(font_manager_ft *manager, cu_term *term, bool test_mode)
{
    cu_cellgrid *cg = new cu_cellgrid{};

    cg->term = term;
    cg->manager = manager;

    if (test_mode) {
        cg->width = 1200;
        cg->height = 800;
        cg->font_size = 25.0f;
        cg->margin = 0.0f;
    } else {
        #if defined __APPLE__
        cg->width = 630;
        cg->height = 440;
        cg->font_size = 12.5f;
        cg->margin = 15.0f;
        #else
        cg->width = 1260;
        cg->height = 860;
        cg->font_size = 25.0f;
        cg->margin = 30.0f;
        #endif
    }
    cg->cursor_color = 0x40000000;
    cg->text_lang = "en";
    cg->rscale = 1.0f;
    cg->flags = cu_cellgrid_background;

    cu_typeface_init(cg);

    return cg;
}

static font_face_ft* cell_font(cu_cellgrid *cg, cu_cell &cell)
{
    font_face *face;

    if (cell.codepoint >= 0x1f000 && cell.codepoint <= 0x1ffff) {
        face = cg->mono1_emoji;
    }
    else if (cell.flags & cu_cell_bold) {
        face = cg->mono1_bold;
    }
    else {
        face = cg->mono1_regular;
    }

    return static_cast<font_face_ft*>(face);
}

static cu_cell cell_col(cu_cellgrid *cg, cu_cell &cell)
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

cu_winsize cu_cellgrid_visible(cu_cellgrid *cg)
{
    /* vis_lines == vis_rows initially */
    int vis_rows = (int)std::max(0.f, cg->height - cg->margin*2.f) / cg->fm.leading;
    int vis_cols = (int)std::max(0.f, cg->width  - cg->margin*2.f) / cg->fm.advance;
    return cu_winsize { vis_rows, vis_rows, vis_cols };
}

static cu_winsize draw_loop(cu_cellgrid *cg, int rows, int cols,
    std::function<void(cu_line &line,size_t k,size_t l,size_t o,size_t i)> linepre_cb,
    std::function<void(cu_cell &cell,size_t k,size_t l,size_t o,size_t i)> cell_cb,
    std::function<void(cu_line &line,size_t k,size_t l,size_t o,size_t i)> linepost_cb)
{
    int wrapline_count = 0;
    bool linewrap = (cg->term->flags & cu_flag_DECAWM) > 0;
    size_t linecount = cg->term->lines.size();
    for (size_t k = linecount - 1, l = 0; k < linecount && l < rows; k--) {
        font_face_ft *face, *lface = nullptr;
        cu_line line = cg->term->lines[k];
        line.unpack();
        size_t cellcount = line.cells.size();
        size_t wraplines = cellcount == 0 ? 1
            : linewrap ? (cellcount + cols - 1) / cols : 1;
        for (size_t j = wraplines - 1; j < wraplines && l < rows; j--) {
            if (j != 0) wrapline_count++;
            size_t o = j * cols;
            size_t limit = std::min(o + cols, cellcount);
            linepre_cb(line, k, l, o, o);
            for (size_t i = o; i < limit; i++) {
                cu_cell &cell = line.cells[i];
                cell_cb(cell, k, l, o, i);
            }
            linepost_cb(line, k, l, o, limit);
            l++;
        }
    }
    return cu_winsize{
        rows - wrapline_count, rows, cols,
        (int)cg->width, (int)cg->height
    };
}

cu_winsize cu_cellgrid_draw(cu_cellgrid *cg, draw_list &batch, MVGCanvas &canvas)
{
    text_renderer_ft renderer(cg->manager, cg->rscale);
    std::vector<glyph_shape> shapes;

    int rows = (int)std::max(0.f, cg->height - cg->margin*2.f) / cg->fm.leading;
    int cols = (int)std::max(0.f, cg->width  - cg->margin*2.f) / cg->fm.advance;
    int font_size = (int)(cg->fm.size * 64.0f);
    int advance_x = (int)(cg->fm.advance * 64.0f);
    float glyph_height = cg->fm.height - cg->fm.descender;
    float y_offset = floorf((cg->fm.leading - glyph_height)/2.f) + cg->fm.descender;
    int clrow = cg->term->cur_row, clcol = cg->term->cur_col;
    float ox = cg->margin, oy = cg->height - cg->margin;

    auto render_block = [&](int row, int col, int h, int w, uint c)
    {
        float u1 = 0.f, v1 = 0.f, u2 = 0.f, v2 = 0.f;
        float x2 = ox + col * cg->fm.advance, x1 = x2 + (cg->fm.advance * w);
        float y1 = oy - row * cg->fm.leading, y2 = y1 - (cg->fm.leading * h);
        uint o0 = draw_list_vertex(batch, {{x1, y1, 0}, {u1, v1}, c});
        uint o1 = draw_list_vertex(batch, {{x2, y1, 0}, {u2, v1}, c});
        uint o2 = draw_list_vertex(batch, {{x2, y2, 0}, {u2, v2}, c});
        uint o3 = draw_list_vertex(batch, {{x1, y2, 0}, {u1, v2}, c});
        draw_list_indices(batch, image_none, mode_triangles,
            shader_flat, {o0, o3, o1, o1, o3, o2});
    };

    auto render_underline = [&](int row, int col, int w, uint fg)
    {
        float lw = cg->fm.advance * w, sw = 2.0f;
        float x1 = ox + col * cg->fm.advance, x2 = x1 + lw;
        float y = oy - row * cg->fm.leading - (y_offset + cg->fm.underline_position - sw);
        canvas.set_stroke_width(sw);
        canvas.set_fill_brush(MVGBrush{MVGBrushNone, { }, { }});
        canvas.set_stroke_brush(MVGBrush{MVGBrushSolid, { }, { color(fg) }});
        MVGPath *p1 = canvas.new_path({0.0f,0.0f},{lw,sw});
        p1->pos = { (x1+x2)*0.5f, y };
        p1->new_line({0.0f,0.0f}, {lw,0.0f});
    };

    auto render_text = [&](float x, float y, font_face_ft *face)
    {
        text_segment segment("", cg->text_lang, face, font_size, x, y-y_offset, 0);
        renderer.render(batch, shapes, segment);
        shapes.clear();
    };

    /* emit background */
    if ((cg->flags & cu_cellgrid_background) > 0) {
        color white = color(1.0f, 1.0f, 1.0f, 1.0f);
        color black = color(0.0f, 0.0f, 0.0f, 1.0f);
        float w = 2.0f, m = cg->margin/2.f + w;
        float tx = cg->width/2.0f;
        float ty = cg->height/2.0f;
        canvas.clear();
        canvas.set_fill_brush(MVGBrush{MVGBrushSolid, { }, { white }});
        canvas.set_stroke_brush(MVGBrush{MVGBrushSolid, { }, { black }});
        canvas.set_stroke_width(w);
        canvas.new_rounded_rectangle(vec2(tx, ty), vec2(tx - m, ty - m), m);
        canvas.emit(batch);
    }

    /* render background */

    cu_winsize dim = draw_loop(cg, rows, cols,
        [&] (auto line, auto k, auto l, auto o, auto i) {},
        [&] (auto cell, auto k, auto l, auto o, auto i) {
            uint bg = cell_col(cg, cell).bg;
            render_block(l, i-o, 1, 1, bg);
        },
        [&] (auto line, auto k, auto l, auto o, auto i) {}
    );

    /* render text */

    draw_loop(cg, rows, cols,
        [&] (auto line, auto k, auto l, auto o, auto i) {},
        [&] (auto cell, auto k, auto l, auto o, auto i) {
            font_face_ft *face = cell_font(cg, cell);
            uint glyph = cu_typeface_lookup_glyph(face, cell.codepoint);
            shapes.push_back({
                glyph, (unsigned)o, 0, 0, advance_x, 0, cell_col(cg, cell).fg
            });
            render_text(ox + (i-o) * cg->fm.advance, oy - l * cg->fm.leading, face);
        },
        [&] (auto line, auto k, auto l, auto o, auto i) {}
    );

    /* render underline */

    uint lounder;
    bool under, lunder;
    uint fg, lfg;

    draw_loop(cg, rows, cols,
        [&] (auto line, auto k, auto l, auto o, auto i) {
            lounder = 0;
            lunder = under = false;
            lfg = fg = 0;
        },
        [&] (auto cell, auto k, auto l, auto o, auto i) {
            under = (cell.flags & cu_cell_underline) > 0;
            fg = cell_col(cg, cell).fg;
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

    if ((cg->term->flags & cu_flag_DECTCEM) > 0) {
        draw_loop(cg, rows, cols,
            [&] (auto line, auto k, auto l, auto o, auto i) {
                if (clrow == k && clcol >= o && clcol < o + cols) {
                    render_block(l, clcol - o, 1, 1, cg->cursor_color);
                }
            },
            [&] (auto cell, auto k, auto l, auto o, auto i) {},
            [&] (auto line, auto k, auto l, auto o, auto i) {}
        );
    }

    return dim;
}
