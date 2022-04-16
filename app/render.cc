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
#include <chrono>

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
#include "terminal.h"
#include "process.h"
#include "cellgrid.h"
#include "render.h"

using namespace std::chrono;

struct cu_render_opengl : cu_render
{
	font_manager_ft *manager;
	cu_cellgrid *cg;
	cu_winsize dim;
	circular_buffer frame_times;
	texture_buffer shape_tb;
	texture_buffer edge_tb;
	texture_buffer brush_tb;
	program prog_flat;
	program prog_texture;
	program prog_msdf;
	program prog_canvas;
	GLuint vao;
	GLuint vbo;
	GLuint ibo;
	std::map<int,GLuint> tex_map;
	draw_list batch;
	mat4 mvp;
	AContext ctx;
	ui9::Root root;
	MVGCanvas canvas;
	bool overlay_stats;

	cu_render_opengl(font_manager_ft *manager, cu_cellgrid *cg);
	virtual ~cu_render_opengl();

	virtual void set_overlay(bool val);
	virtual MVGCanvas* get_canvas();
	virtual ui9::Root* get_ui9root();

	virtual cu_winsize update();
	virtual void display();
	virtual void reshape(int width, int height, float scale);
	virtual void initialize();

protected:
	program* cmd_shader_gl(int cmd_shader);
	std::vector<std::string> get_stats();
	void render_stats(draw_list &batch);
	void update_uniforms(program *prog);
};

cu_render_opengl::cu_render_opengl(font_manager_ft *manager, cu_cellgrid *cg)
: manager(manager), cg(cg), frame_times{},
  shape_tb(), edge_tb(), brush_tb(),
  prog_flat(), prog_texture(), prog_msdf(), prog_canvas(),
  vao(0), vbo(0), ibo(0),
  tex_map(), batch(), mvp{}, ctx{},
  root(manager), canvas(manager),
  overlay_stats(false) {}

cu_render_opengl::~cu_render_opengl() {}

cu_render* cu_render_new(font_manager_ft *manager, cu_cellgrid *cg)
{
	return new cu_render_opengl(manager, cg);
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

void cu_render_opengl::set_overlay(bool val) { overlay_stats = val; }
MVGCanvas* cu_render_opengl::get_canvas() { return &canvas; }
ui9::Root* cu_render_opengl::get_ui9root() { return &root; }

std::vector<std::string> cu_render_opengl::get_stats()
{
    std::vector<std::string> stats;
    stats.push_back(format("FPS: %4.1f",
        1e9 / circular_buffer_average(&frame_times)));
    return stats;
}

void cu_render_opengl::render_stats(draw_list &batch)
{
    text_shaper_hb shaper;
    text_renderer_ft renderer(manager, cg->rscale);

    uint c = 0xff000000;
    float x = cg->width - cg->margin, y = cg->margin;
    std::vector<std::string> stats = get_stats();

    float glyph_height = cg->fm.height - cg->fm.descender;
    float y_offset = floorf((cg->fm.leading - glyph_height)/2.f) + cg->fm.descender;

    for (size_t i = 0; i < stats.size(); i++) {
        std::vector<glyph_shape> shapes;
        text_segment segment(stats[i], cg->text_lang, cg->mono1_regular,
            (int)((float)cg->font_size * 64.0f), x, y + cg->fm.leading - y_offset, c);
        shaper.shape(shapes, segment);
        for (auto &shape : shapes) segment.x -= shape.x_advance/64.0f;
        renderer.render(batch, shapes, segment);
        y += (int)((float)cg->font_size * 1.3f);
    }
}

cu_winsize cu_render_opengl::update()
{
    static ullong tl, tn;

    if (!cg->term->needs_update) return dim;

    auto now = high_resolution_clock::now();
    tn = duration_cast<nanoseconds>(now.time_since_epoch()).count();
    if (tl != 0) circular_buffer_add(&frame_times, tn - tl);
    tl = tn;

    /* start frame with empty draw list */
    draw_list_clear(batch);

    /* set up scale/translate matrix */
    float s = 1.0f;
    float tx = cg->width/2.0f;
    float ty = cg->height/2.0f;
    canvas.set_transform(mat3(s,  0,  tx,
                              0,  s,  ty,
                              0,  0,  1));
    canvas.set_scale(cg->rscale);

    /* emit canvas draw list */
    color white = color(1.0f, 1.0f, 1.0f, 1.0f);
    color black = color(0.0f, 0.0f, 0.0f, 1.0f);
    float w = 2.0f, m = cg->margin/2.f + w;
    canvas.clear();
    canvas.set_fill_brush(MVGBrush{MVGBrushSolid, { }, { white }});
    canvas.set_stroke_brush(MVGBrush{MVGBrushSolid, { }, { black }});
    canvas.set_stroke_width(w);
    canvas.new_rounded_rectangle(vec2(0), vec2(tx - m, ty - m), m);
    canvas.emit(batch);

    dim = cu_cellgrid_draw(cg, batch);

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

    cg->term->needs_update = 0;

    return dim;
}

program* cu_render_opengl::cmd_shader_gl(int cmd_shader)
{
    switch (cmd_shader) {
    case shader_flat:    return &prog_flat;
    case shader_texture: return &prog_texture;
    case shader_msdf:    return &prog_msdf;
    case shader_canvas:  return &prog_canvas;
    default: return nullptr;
    }
}

void cu_render_opengl::display()
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

void cu_render_opengl::update_uniforms(program *prog)
{
    uniform_matrix_4fv(prog, "u_mvp", (const GLfloat *)&mvp[0][0]);
    uniform_1i(prog, "u_tex0", 0);
    uniform_1i(prog, "tb_shape", 0);
    uniform_1i(prog, "tb_edge", 1);
    uniform_1i(prog, "tb_brush", 2);
}

void cu_render_opengl::reshape(int width, int height, float scale)
{
    cg->width = (float)width;
    cg->height = (float)height;
    cg->rscale = 1.f/scale;

    mvp = glm::ortho(0.0f, cg->width, cg->height, 0.0f, 0.0f, 100.0f);

    glUseProgram(prog_canvas.pid);
    update_uniforms(&prog_canvas);

    glUseProgram(prog_msdf.pid);
    update_uniforms(&prog_msdf);

    glUseProgram(prog_flat.pid);
    update_uniforms(&prog_flat);

    glUseProgram(prog_texture.pid);
    update_uniforms(&prog_texture);
}

void cu_render_opengl::initialize()
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

    /* pipeline */
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
}
