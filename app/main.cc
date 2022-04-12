#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cassert>
#include <cmath>
#include <ctime>

#include <memory>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <set>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>

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
#include "format.h"
#include "app.h"

#include "ui9.h"
#include "cuterm.h"

using namespace std::chrono;
using vec2 = glm::vec2;

/* globals */

static std::unique_ptr<cu_term> term;

static circular_buffer frame_times;
static texture_buffer shape_tb, edge_tb, brush_tb;
static program prog_flat, prog_texture, prog_msdf, prog_canvas;
static GLuint vao, vbo, ibo;
static std::map<int,GLuint> tex_map;
static font_manager_ft manager;
static MVGCanvas canvas(&manager);

static mat4 mvp;
static GLFWwindow* window;

static const char* text_lang = "en";
static const char *mono1_regular_font_path = "fonts/NotoSansMono-Regular.ttf";
static const char *mono1_bold_font_path = "fonts/NotoSansMono-Bold.ttf";

#if defined __APPLE__
static const float stats_font_size = 12.5f;
static const float terminal_font_size = 12.5f;
static int window_width = 640, window_height = 440;
#else
static const float stats_font_size = 25.0f;
static const float terminal_font_size = 25.0f;
static int window_width = 1280, window_height = 840;
#endif

static float rs;
static ullong tl, tn;
static bool help_text = false;
static bool overlay_stats = false;
static cu_font_metric fm;
static font_face *mono1_regular, *mono1_bold;
static int framebuffer_width, framebuffer_height;
static uint cursor_color = 0x40000000;

/* canvas state */

static AContext ctx;
static draw_list batch;
static vec2 mouse_pos;
static ui9::Root root(&manager);

/* metrics */

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H

cu_font_metric get_font_metrics(font_face *face, float font_size, int codepoint)
{
    font_face_ft *fft = static_cast<font_face_ft*>(face);
    FT_Face ftface = fft->ftface;
    FT_GlyphSlot ftglyph = ftface->glyph;
    FT_Size_Metrics*  metrics = &ftface->size->metrics;
    uint32_t glyph = FT_Get_Char_Index(ftface, codepoint);

    fft->get_metrics(font_size*64.0f);

    FT_Error fterr;
    if ((fterr = FT_Load_Glyph(ftface, glyph,
        FT_LOAD_NO_BITMAP | FT_LOAD_COMPUTE_METRICS | FT_LOAD_NO_HINTING))) {
        Panic("error: FT_Load_Glyph failed: glyph=%d fterr=%d\n",
            glyph, fterr);
    }
    if (ftface->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        Panic("error: FT_Load_Glyph format is not outline: format=\n",
            ftface->glyph->format);
    }

    // divide by 64 for font_size, units and target metric
    float scale = font_size/(64.0f*64.0f);
    float leading = ceilf(font_size * 1.3f);
    float height = roundf(ftface->height * scale * 4.0f) * 0.25f;
    float ascender = ceilf(ftface->ascender * scale * 4.0f) * 0.25f;
    float descender = floorf(ftface->descender * scale * 4.0f) * 0.25f;
    float underline_position = floorf(ftface->underline_position * scale * 4.0f) * 0.25f;
    float underline_thickness = ceilf(ftface->underline_thickness * scale * 4.0f) * 0.25f;
    float advance = roundf(ftglyph->advance.x / 64.0f * 4.0f) * 0.25f;

    cu_font_metric m = {
        font_size, advance, leading, height,
        ascender, descender, underline_position, underline_thickness
    };

    return m;
}

void print_font_metrics(font_face *face, cu_font_metric m)
{
    Debug("face=%s size=%f advance=%f leading=%f\n",
        face->name.c_str(), m.size, m.advance, m.leading);
    Debug("\theight=%f ascender=%f descender=%f\n",
        m.height, m.ascender, m.descender);
    Debug("\tunderline_position=%f underline_thickness=%f\n",
        m.underline_position, m.underline_thickness);
}

/* display  */

static program* cmd_shader_gl(int cmd_shader)
{
    switch (cmd_shader) {
    case shader_flat:    return &prog_flat;
    case shader_texture: return &prog_texture;
    case shader_msdf:    return &prog_msdf;
    case shader_canvas:  return &prog_canvas;
    default: return nullptr;
    }
}

static void circular_buffer_add(circular_buffer *buffer, llong new_value)
{
    llong old_value = buffer->samples[buffer->offset];

    buffer->samples[buffer->offset] = new_value;
    buffer->sum += new_value - old_value;
    buffer->count++;
    buffer->offset++;

    if (buffer->offset >= array_size(buffer->samples)) {
        buffer->offset = 0;
    }
}

static long circular_buffer_average(circular_buffer *buffer)
{
    if (buffer->count == 0) {
        return -1;
    } else if (buffer->count < array_size(buffer->samples)) {
        return buffer->sum / buffer->count;
    } else {
        return buffer->sum / array_size(buffer->samples);
    }
}

static std::vector<std::string> get_stats()
{
    std::vector<std::string> stats;
    stats.push_back(format("FPS: %4.1f",
        1e9 / circular_buffer_average(&frame_times)));
    return stats;
}

static void render_stats(draw_list &batch)
{
    text_shaper_hb shaper;
    text_renderer_ft renderer(&manager, rs);

    uint32_t c = 0xff000000;
    float x = window_width - 20.0f, y = 30.0f;
    std::vector<std::string> stats = get_stats();

    for (size_t i = 0; i < stats.size(); i++) {
        std::vector<glyph_shape> shapes;
        text_segment segment(stats[i], text_lang, mono1_regular,
            (int)((float)stats_font_size * 64.0f), x, y, c);
        shaper.shape(shapes, segment);
        for (auto &shape : shapes) segment.x -= shape.x_advance/64.0f;
        renderer.render(batch, shapes, segment);
        y += (int)((float)stats_font_size * 1.3f);
    }
}

static font_face_ft* cell_font(cu_term *term, cu_cell &cell)
{
    font_face *face;

    if (cell.flags & cu_cell_bold) {
        face = mono1_bold;
    } else {
        face = mono1_regular;
    }

    return static_cast<font_face_ft*>(face);
}

static cu_cell cell_col(cu_term *term, cu_cell &cell)
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

static void calc_visible(cu_term *term)
{
    term->vis_rows = (int)std::max(0.f, window_height - 25.0f) / fm.leading;
    term->vis_cols = (int)std::max(0.f, window_width  - 25.0f) / fm.advance;
    term->vis_lines = term->vis_rows; /* valid initially */
}

static cu_dim render_loop(cu_term *term, int rows, int cols,
    std::function<void(cu_line &line,size_t k,size_t l,size_t o)> linepre_cb,
    std::function<void(cu_cell &cell,size_t k,size_t l,size_t i)> cell_cb,
    std::function<void(cu_line &line,size_t k,size_t l,size_t o)> linepost_cb)
{
    int wrapline_count = 0;
    bool linewrap = (term->flags & cu_flag_DECAWM) > 0;
    size_t linecount = term->lines.size();
    for (size_t k = linecount - 1, l = 0; k < linecount && l < rows; k--) {
        font_face_ft *face, *lface = nullptr;
        cu_line line = term->lines[k];
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
    return cu_dim{ rows - wrapline_count, rows, cols };
}

static void render_terminal(cu_term *term, draw_list &batch)
{
    text_renderer_ft renderer(&manager, rs);
    std::vector<glyph_shape> shapes;

    int rows = (int)std::max(0.f, window_height - 25.0f) / fm.leading;
    int cols = (int)std::max(0.f, window_width  - 25.0f) / fm.advance;
    int font_size = (int)(fm.size * 64.0f);
    int advance_x = (int)(fm.advance * 64.0f);
    float glyph_height = fm.height - fm.descender;
    float y_offset = floorf((fm.leading - glyph_height)/2.f) + fm.descender;
    int clrow = term->cur_row, clcol = term->cur_col;
    float ox = 15.0f, oy = window_height - 15.0f;
    size_t lo;
    uint bg, lbg;
    font_face_ft *face, *lface = nullptr;

    auto render_block = [&](int row, int col, int h, int w, uint c) {
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

    auto render_text = [&](float x, float y, font_face_ft *face) {
        text_segment segment("", text_lang, face, font_size, x, y-y_offset, 0);
        renderer.render(batch, shapes, segment);
        shapes.clear();
    };

    /* render background colors */

    cu_dim rmd = render_loop(term, rows, cols,
        [&] (auto line, auto k, auto l, auto o) {
            lo = 0;
            bg = lbg = 0;
        },
        [&] (auto cell, auto k, auto l, auto o) {
            bg = cell_col(term, cell).bg;
            if (o-lo > 0 && bg != lbg) {
                render_block(l, lo, 1, o-lo, lbg);
                lo = o;
            }
            lbg = bg;
        },
        [&] (auto line, auto k, auto l, auto o) {
            if (o-lo > 0) {
                render_block(l, lo, 1, o-lo, lbg);
            }
        }
    );

    /* render text */

    render_loop(term, rows, cols,
        [&] (auto line, auto k, auto l, auto o) {
            lo = 0;
            face = lface = nullptr;
        },
        [&] (auto cell, auto k, auto l, auto o) {
            face = cell_font(term, cell);
            if (o-lo > 0 && face != lface) {
                render_text(ox + lo * fm.advance, oy - l * fm.leading, lface);
                lo = o;
            }
            lface = face;
            uint glyph = FT_Get_Char_Index(face->ftface, cell.codepoint);
            shapes.push_back({
                glyph, (unsigned)o, 0, 0, advance_x, 0, cell_col(term, cell).fg
            });
        },
        [&] (auto line, auto k, auto l, auto o) {
            if (o-lo > 0) {
                render_text(ox + lo * fm.advance, oy - l * fm.leading, lface);
            }
        }
    );

    /* render cursor */

    if ((term->flags & cu_flag_DECTCEM) > 0) {
        render_loop(term, rows, cols,
            [&] (auto line, auto k, auto l, auto o) {
                if (clrow == k && clcol >= o && clcol < o + cols)
                {
                    render_block(l, term->cur_col - o, 1, 1, cursor_color);
                }
            },
            [&] (auto cell, auto k, auto l, auto o) {},
            [&] (auto line, auto k, auto l, auto o) {}
        );
    }

    term->vis_rows = rmd.vis_rows;
    term->vis_cols = rmd.vis_cols;
    term->vis_lines = rmd.vis_lines;
}

static const color white = color(1.0f, 1.0f, 1.0f, 1.0f);
static const color black = color(0.0f, 0.0f, 0.0f, 1.0f);

static void update()
{
    if (!term->needs_update) return;

    auto now = high_resolution_clock::now();
    tn = duration_cast<nanoseconds>(now.time_since_epoch()).count();
    if (tl != 0) circular_buffer_add(&frame_times, tn - tl);
    tl = tn;

    /* start frame with empty draw list */
    draw_list_clear(batch);

    /* set up scale/translate matrix */
    float s = 1.0f;
    float ds = sqrtf((float)(framebuffer_width * framebuffer_height) /
                     (float)(window_width * window_height));
    float tx = window_width/2.0f;
    float ty = window_height/2.0f;
    canvas.set_transform(mat3(s,  0,  tx,
                              0,  s,  ty,
                              0,  0,  1));
    canvas.set_scale(ds);
    rs = 1.f/ds;

    /* emit canvas draw list */
    canvas.clear();
    canvas.set_fill_brush(MVGBrush{MVGBrushSolid, { }, { white }});
    canvas.set_stroke_brush(MVGBrush{MVGBrushSolid, { }, { black }});
    canvas.set_stroke_width(1);
    canvas.new_rounded_rectangle(vec2(0), vec2(window_width/2.0f-10.0f,window_height/2.0f-10.0f), 10.0f);
    canvas.emit(batch);

    render_terminal(term.get(), batch);

    /* render stats text */
    if (overlay_stats) {
        render_stats(batch);
    }

    /* synchronize canvas texture buffers */
    buffer_texture_create(shape_tb, canvas.ctx->shapes, GL_TEXTURE0, GL_R32F);
    buffer_texture_create(edge_tb, canvas.ctx->edges, GL_TEXTURE1, GL_R32F);
    buffer_texture_create(brush_tb, canvas.ctx->brushes, GL_TEXTURE2, GL_R32F);

    /* update vertex and index buffers arrays (idempotent) */
    vertex_buffer_create("vbo", &vbo, GL_ARRAY_BUFFER, batch.vertices);
    vertex_buffer_create("ibo", &ibo, GL_ELEMENT_ARRAY_BUFFER, batch.indices);

    term->needs_update = 0;
}

static void display()
{
    /* okay, lets send commands to the GPU */
    glClearColor(0.9f, 0.9f, 0.9f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* draw list batch with tbo_iid canvas texture buffer special case */
    for (auto img : batch.images) {
        auto ti = tex_map.find(img.iid);
        if (ti == tex_map.end()) {
            tex_map[img.iid] = image_create_texture(img);
        } else {
            image_update_texture(tex_map[img.iid], img);
        }
    }
    for (auto cmd : batch.cmds) {
        glUseProgram(cmd_shader_gl(cmd.shader)->pid);
        if (cmd.iid == tbo_iid) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_BUFFER, shape_tb.tex);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_BUFFER, edge_tb.tex);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_BUFFER, brush_tb.tex);
        } else {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex_map[cmd.iid]);
        }
        glBindVertexArray(vao);
        glDrawElements(cmd_mode_gl(cmd.mode), cmd.count, GL_UNSIGNED_INT,
            (void*)(cmd.offset * sizeof(uint)));
    }
}

static void update_uniforms(program *prog)
{
    uniform_matrix_4fv(prog, "u_mvp", (const GLfloat *)&mvp[0][0]);
    uniform_1i(prog, "u_tex0", 0);
    uniform_1i(prog, "tb_shape", 0);
    uniform_1i(prog, "tb_edge", 1);
    uniform_1i(prog, "tb_brush", 2);
}

static void reshape()
{
    glfwGetWindowSize(window, &window_width, &window_height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);

    mvp = glm::ortho(0.0f, (float)window_width, (float)window_height,
        0.0f, 0.0f, 100.0f);

    glViewport(0, 0, framebuffer_width, framebuffer_height);

    glUseProgram(prog_canvas.pid);
    update_uniforms(&prog_canvas);

    glUseProgram(prog_msdf.pid);
    update_uniforms(&prog_msdf);

    glUseProgram(prog_flat.pid);
    update_uniforms(&prog_flat);

    glUseProgram(prog_texture.pid);
    update_uniforms(&prog_texture);
}

/* keyboard callback */

static void keyboard(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    cuterm_keyboard(term.get(), key, scancode, action, mods);
}

/* mouse callbacks */

static void scroll(GLFWwindow* window, double xoffset, double yoffset)
{
    //
}

static char q;
static char b;

static bool mouse_button_ui9(int button, int action, int mods, vec3 pos)
{
    switch(button) {
    case GLFW_MOUSE_BUTTON_LEFT:  b = ui9::left_button;  break;
    case GLFW_MOUSE_BUTTON_RIGHT: b = ui9::right_button; break;
    }
    switch(action) {
    case GLFW_PRESS:   q = ui9::pressed;  break;
    case GLFW_RELEASE: q = ui9::released; break;
    }
    vec3 v = canvas.get_inverse_transform() * pos;
    ui9::MouseEvent evt{{ui9::mouse, q}, b, v};
    return root.dispatch(&evt.header);
}

static void mouse_button(GLFWwindow* window, int button, int action, int mods)
{
    if (mouse_button_ui9(button, action, mods, vec3(mouse_pos, 1))) {
        return;
    }
}

static bool mouse_motion_ui9(vec3 pos)
{
    vec3 v = canvas.get_inverse_transform() * pos;
    ui9::MouseEvent evt{{ui9::mouse, ui9::motion}, b, v};
    return root.dispatch(&evt.header);
}

static void cursor_position(GLFWwindow* window, double xpos, double ypos)
{
    mouse_pos = vec2(xpos, ypos);

    if (mouse_motion_ui9(vec3(mouse_pos, 1))) {
        return;
    }
}

/* OpenGL initialization */

static void initialize()
{
    GLuint flat_fsh, texture_fsh, msdf_fsh, canvas_fsh, vsh;

    std::vector<std::string> attrs = {
        "a_pos", "a_uv0", "a_color", "a_shape", "a_gamma"
    };

    /* shader program */
    vsh = compile_shader(GL_VERTEX_SHADER, "shaders/simple.vsh");
    flat_fsh = compile_shader(GL_FRAGMENT_SHADER, "shaders/flat.fsh");
    texture_fsh = compile_shader(GL_FRAGMENT_SHADER, "shaders/texture.fsh");
    msdf_fsh = compile_shader(GL_FRAGMENT_SHADER, "shaders/msdf.fsh");
    canvas_fsh = compile_shader(GL_FRAGMENT_SHADER, "shaders/canvas.fsh");
    link_program(&prog_flat, vsh, flat_fsh, attrs);
    link_program(&prog_texture, vsh, texture_fsh, attrs);
    link_program(&prog_msdf, vsh, msdf_fsh, attrs);
    link_program(&prog_canvas, vsh, canvas_fsh, attrs);
    glDeleteShader(vsh);
    glDeleteShader(texture_fsh);
    glDeleteShader(msdf_fsh);
    glDeleteShader(canvas_fsh);

    /* create vertex and index buffers arrays */
    vertex_buffer_create("vbo", &vbo, GL_ARRAY_BUFFER, batch.vertices);
    vertex_buffer_create("ibo", &ibo, GL_ELEMENT_ARRAY_BUFFER, batch.indices);

    /* configure vertex array object */
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    program *p = &prog_canvas; /* use any program to get attribute locations */
    vertex_array_pointer(p, "a_pos", 3, GL_FLOAT, 0, &draw_vertex::pos);
    vertex_array_pointer(p, "a_uv0", 2, GL_FLOAT, 0, &draw_vertex::uv);
    vertex_array_pointer(p, "a_color", 4, GL_UNSIGNED_BYTE, 1, &draw_vertex::color);
    vertex_array_pointer(p, "a_shape", 1, GL_FLOAT, 0, &draw_vertex::shape);
    vertex_array_1f(p, "a_gamma", 1.0f);
    glBindVertexArray(0);

    /*
     * we need to scan font directory for caching to work, as it uses
     * font ids assigned during scanning. this also means that if the
     * font directory has changed, then cached font ids will be wrong
     */
    if (manager.msdf_enabled) {
        manager.scanFontDir("fonts");
    }

    /* fetch our sans font */
    mono1_regular = manager.findFontByPath(mono1_regular_font_path);
    mono1_bold = manager.findFontByPath(mono1_bold_font_path);

    /* measure font */
    fm = get_font_metrics(mono1_regular, terminal_font_size, 'M');
    print_font_metrics(mono1_regular, fm);

    /* pipeline */
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
}

/* GLFW GUI entry point */

static void framebuffer_size(GLFWwindow* window, int w, int h)
{
    term->needs_update++;
    reshape();
    update();
    display();
    glfwSwapBuffers(window);
}

static void window_refresh(GLFWwindow* window)
{
    display();
    glfwSwapBuffers(window);
}

static void window_pos(GLFWwindow* window, int x, int y)
{
    //printf("%s: x=%d y=%d\n", __func__, x, y);
}

static void window_size(GLFWwindow* window, int w, int h)
{
    //printf("%s: w=%d h=%d\n", __func__, w, h);
}

static void cuterm_main(int argc, char **argv)
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, CTX_OPENGL_MAJOR);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, CTX_OPENGL_MINOR);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);

    term = std::unique_ptr<cu_term>(new cu_term{});
    cuterm_init(term.get());

    window = glfwCreateWindow(window_width, window_height, argv[0], NULL, NULL);
    glfwMakeContextCurrent(window);
    gladLoadGL();
    glfwSwapInterval(1);
    glfwSetScrollCallback(window, scroll);
    glfwSetKeyCallback(window, keyboard);
    glfwSetMouseButtonCallback(window, mouse_button);
    glfwSetCursorPosCallback(window, cursor_position);
    glfwSetFramebufferSizeCallback(window, framebuffer_size);
    glfwSetWindowRefreshCallback(window, window_refresh);
    glfwSetWindowPosCallback(window, window_pos);
    glfwSetWindowSizeCallback(window, window_size);
    glfwGetWindowSize(window, &window_width, &window_height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);

    initialize();
    reshape();

    calc_visible(term.get());
    cuterm_reset(term.get());

    if (cuterm_fork(term.get(), term->vis_cols, term->vis_rows) < 0) {
        Panic("cuterm_fork: failed");
    }

    while (!glfwWindowShouldClose(window)) {
        update();
        display();
        glfwSwapBuffers(window);
        cuterm_winsize(term.get());
        glfwPollEvents();
        do if (cuterm_io(term.get()) < 0) {
            glfwSetWindowShouldClose(window, 1);
        }
        while (cuterm_process(term.get()) > 0);
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    cuterm_close(term.get());
}

/* help text */

void print_help(int argc, char **argv)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -h, --help                command line help\n"
        "  -t, --trace               log trace messages\n"
        "  -d, --debug               log debug messages\n"
        "  -y, --overlay-stats       show statistics overlay\n"
        "  -m, --enable-msdf         enable MSDF font rendering\n",
        argv[0]);
}

/* option parsing */

bool check_param(bool cond, const char *param)
{
    if (cond) {
        printf("error: %s requires parameter\n", param);
    }
    return (help_text = cond);
}

bool match_opt(const char *arg, const char *opt, const char *longopt)
{
    return strcmp(arg, opt) == 0 || strcmp(arg, longopt) == 0;
}

void parse_options(int argc, char **argv)
{
    int i = 1;
    while (i < argc) {
        if (match_opt(argv[i], "-h", "--help")) {
            help_text = true;
            i++;
        } else if (match_opt(argv[i], "-t", "--trace")) {
            logger::set_level(logger::L::Ltrace);
            i++;
        } else if (match_opt(argv[i], "-d", "--debug")) {
            logger::set_level(logger::L::Ldebug);
            i++;
        } else if (match_opt(argv[i], "-y", "--overlay-stats")) {
            overlay_stats = true;
            i++;
        } else if (match_opt(argv[i], "-m", "--enable-msdf")) {
            manager.msdf_enabled = true;
            manager.msdf_autoload = true;
            i++;
        } else {
            fprintf(stderr, "error: unknown option: %s\n", argv[i]);
            help_text = true;
            break;
        }
    }

    if (help_text) {
        print_help(argc, argv);
        exit(1);
    }

}

/* entry point */

static int app_main(int argc, char** argv)
{
    parse_options(argc, argv);
    cuterm_main(argc, argv);

    return 0;
}

declare_main(app_main)
