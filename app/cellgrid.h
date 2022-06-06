#pragma once

enum tty_cellgrid_flag
{
    tty_cellgrid_focused = (1 << 0),
    tty_cellgrid_background = (1 << 1),
    tty_cellgrid_scrollbars = (1 << 2),
    tty_cellgrid_timestamps = (1 << 3),
    tty_cellgrid_linenumbers = (1 << 4)
};

enum tty_cellgrid_face
{
    tty_cellgrid_face_emoji,
    tty_cellgrid_face_regular,
    tty_cellgrid_face_bold,
    tty_cellgrid_face_condensed_regular,
    tty_cellgrid_face_condensed_bold
};

struct tty_style
{
    float width;
    float height;
    float margin;
    float font_size;
    float rscale;
    uint background_color;
    uint cursor_color;
    uint select_focus_color;
    uint select_nofocus_color;
    template <typename... Args> constexpr auto tuple() {
        return std::tie(width, height, margin, font_size, rscale,
            background_color, cursor_color, select_focus_color, select_nofocus_color);
    }
};

inline bool operator==(tty_style &a, tty_style&b) { return a.tuple() == b.tuple(); }
inline bool operator!=(tty_style &a, tty_style&b) { return a.tuple() != b.tuple(); }

struct tty_cellgrid
{
    virtual ~tty_cellgrid() = default;

    virtual void draw(draw_list &batch) = 0;

    virtual bool has_flag(uint f) = 0;
    virtual void set_flag(uint f, bool val) = 0;
    virtual const char* get_lang() = 0;
    virtual tty_style get_style() = 0;
    virtual void set_style(tty_style s) = 0;
    virtual tty_winsize get_winsize() = 0;
    virtual llong get_scroll_row_limit() = 0;
    virtual llong get_scroll_row() = 0;
    virtual void set_scroll_row(llong row) = 0;
    virtual tty_font_metric get_font_metric() = 0;
    virtual font_face* get_font_face(tty_cellgrid_face face) = 0;
    virtual font_manager_ft* get_manager() = 0;
    virtual tty_teletype* get_teletype() = 0;
    virtual MVGCanvas* get_canvas() = 0;
    virtual ui9::Root* get_root() = 0;
    virtual bool mouse_event(ui9::MouseEvent *me) = 0;
};

tty_cellgrid* tty_cellgrid_new(font_manager_ft *manager, tty_teletype *term,
                             bool test_mode = false);
