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
#include "color.h"
#include "logger.h"
#include "app.h"

#include "terminal.h"
#include "cellgrid.h"
#include "typeface.h"

cu_cellgrid* cu_cellgrid_new(font_manager_ft *manager, cu_term *term)
{
    cu_cellgrid *cg = new cu_cellgrid{};

    cg->term = term;
    cg->manager = manager;
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
    cg->cursor_color = 0x40000000;
    cg->text_lang = "en";
    cg->rscale = 1.0f;

    cu_typeface_init(cg);

    return cg;
}

static font_face_ft* cell_font(cu_cellgrid *cg, cu_cell &cell)
{
    font_face *face;

    if (cell.flags & cu_cell_bold) {
        face = cg->mono1_bold;
    } else {
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
    std::function<void(cu_line &line,size_t k,size_t l,size_t o)> linepre_cb,
    std::function<void(cu_cell &cell,size_t k,size_t l,size_t i)> cell_cb,
    std::function<void(cu_line &line,size_t k,size_t l,size_t o)> linepost_cb)
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
            linepre_cb(line, k, l, o);
            for (size_t i = o; i < limit; i++) {
                cu_cell &cell = line.cells[i];
                cell_cb(cell, k, l, i - o);
            }
            linepost_cb(line, k, l, limit - o);
            l++;
        }
    }
    return cu_winsize{
        rows - wrapline_count, rows, cols,
        (int)cg->width, (int)cg->height
    };
}

cu_winsize cu_cellgrid_draw(cu_cellgrid *cg, draw_list &batch)
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

    auto render_block = [&](int row, int col, int h, int w, uint c) {
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

    auto render_text = [&](float x, float y, font_face_ft *face) {
        text_segment segment("", cg->text_lang, face, font_size, x, y-y_offset, 0);
        renderer.render(batch, shapes, segment);
        shapes.clear();
    };

    /* render background */

    cu_winsize dim = draw_loop(cg, rows, cols,
        [&] (auto line, auto k, auto l, auto o) {},
        [&] (auto cell, auto k, auto l, auto o) {
            uint bg = cell_col(cg, cell).bg;
            render_block(l, o, 1, 1, bg);
        },
        [&] (auto line, auto k, auto l, auto o) {}
    );

    /* render text */

    draw_loop(cg, rows, cols,
        [&] (auto line, auto k, auto l, auto o) {},
        [&] (auto cell, auto k, auto l, auto o) {
            font_face_ft *face = cell_font(cg, cell);
            uint glyph = cu_typeface_lookup_glyph(face, cell.codepoint);
            shapes.push_back({
                glyph, (unsigned)o, 0, 0, advance_x, 0, cell_col(cg, cell).fg
            });
            render_text(ox + o * cg->fm.advance, oy - l * cg->fm.leading, face);
        },
        [&] (auto line, auto k, auto l, auto o) {}
    );

    /* render cursor */

    if ((cg->term->flags & cu_flag_DECTCEM) > 0) {
        draw_loop(cg, rows, cols,
            [&] (auto line, auto k, auto l, auto o) {
                if (clrow == k && clcol >= o && clcol < o + cols) {
                    render_block(l, clcol - o, 1, 1, cg->cursor_color);
                }
            },
            [&] (auto cell, auto k, auto l, auto o) {},
            [&] (auto line, auto k, auto l, auto o) {}
        );
    }

    return dim;
}
