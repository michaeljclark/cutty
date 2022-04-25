#pragma once

enum cu_cellgrid_flag
{
    cu_cellgrid_background = (1 << 0)
};

struct cu_cellgrid
{
    cu_font_metric fm;
    uint cursor_color;
    uint background_color;
    const char* text_lang;
    font_face *mono1_emoji;
    font_face *mono1_regular;
    font_face *mono1_bold;
    float width;
    float height;
    float margin;
    float font_size;
    float rscale;
    int flags;
    ssize_t vdelta;

    virtual ~cu_cellgrid() = default;

    virtual cu_winsize visible() = 0;
    virtual cu_winsize draw(draw_list &batch) = 0;

    virtual font_manager_ft* get_manager() = 0;
    virtual cu_term* get_terminal() = 0;
    virtual MVGCanvas* get_canvas() = 0;
    virtual ui9::Root* get_root() = 0;
};

cu_cellgrid* cu_cellgrid_new(font_manager_ft *manager, cu_term *term,
                             bool test_mode = false);
