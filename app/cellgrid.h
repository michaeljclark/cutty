#pragma once

enum tty_cellgrid_flag
{
    tty_cellgrid_background = (1 << 0)
};

struct tty_cellgrid
{
    tty_fontmetric fm;
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
    llong scroll_row;
    llong scroll_col;

    virtual ~tty_cellgrid() = default;

    virtual tty_winsize get_winsize() = 0;
    virtual void draw(draw_list &batch) = 0;

    virtual font_manager_ft* get_manager() = 0;
    virtual tty_teletype* get_teletype() = 0;
    virtual MVGCanvas* get_canvas() = 0;
    virtual ui9::Root* get_root() = 0;
};

tty_cellgrid* tty_cellgrid_new(font_manager_ft *manager, tty_teletype *term,
                             bool test_mode = false);
