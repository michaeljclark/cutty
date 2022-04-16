#pragma once

struct cu_cellgrid
{
    cu_term *term;
    font_manager_ft *manager;
    cu_font_metric fm;
    uint cursor_color;
    const char* text_lang;
    font_face *mono1_emoji;
    font_face *mono1_regular;
    font_face *mono1_bold;
    float width;
    float height;
    float margin;
    float font_size;
    float rscale;
};

cu_cellgrid* cu_cellgrid_new(font_manager_ft *manager, cu_term *term);

void cu_cellgrid_init(cu_cellgrid *cg, cu_term *term, font_manager_ft *manager);
cu_winsize cu_cellgrid_visible(cu_cellgrid *cg);
cu_winsize cu_cellgrid_draw(cu_cellgrid *cg, draw_list &batch);
