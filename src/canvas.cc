// See LICENSE for license details.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <cassert>
#include <cctype>
#include <cmath>

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <tuple>
#include <algorithm>
#include <functional>
#include <numeric>
#include <atomic>
#include <mutex>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H

#include "glm/glm.hpp"
#include "glm/gtc/matrix_inverse.hpp"

#include "binpack.h"
#include "image.h"
#include "color.h"
#include "utf8.h"
#include "draw.h"
#include "font.h"
#include "glyph.h"
#include "canvas.h"
#include "logger.h"

/*
 * gpu-accelerated shape lists
 *
 * The following shape code is derived from msdfgen/ext/import-font.cpp
 * It has been simplified and adopts a data-oriented programming approach.
 * The context output is arrays formatted suitably to download to a GPU.
 */

typedef const FT_Vector ftvec;

static AContext* uctx(void *u) { return static_cast<AContext*>(u); }
static vec2 pt(ftvec *v) { return vec2(v->x/64.f, v->y/64.f); }

static int ftMoveTo(ftvec *p, void *u) {
    uctx(u)->new_contour();
    uctx(u)->pos = pt(p);
    return 0;
}

static int ftLineTo(ftvec *p, void *u) {
    uctx(u)->new_edge(AEdge{EdgeLinear, { uctx(u)->pos, pt(p) }});
    uctx(u)->pos = pt(p);
    return 0;
}

static int ftConicTo(ftvec *c, ftvec *p, void *u) {
    uctx(u)->new_edge(AEdge{EdgeQuadratic, { uctx(u)->pos, pt(c), pt(p) }});
    uctx(u)->pos = pt(p);
    return 0;
}

static int ftCubicTo(ftvec *c1, ftvec *c2, ftvec *p, void *u) {
    uctx(u)->new_edge(AEdge{EdgeCubic, { uctx(u)->pos, pt(c1), pt(c2), pt(p) }});
    uctx(u)->pos = pt(p);
    return 0;
}

static vec2 offset(FT_Glyph_Metrics *m) {
    return vec2(floorf(m->horiBearingX/64.0f),
                floorf((m->horiBearingY-m->height)/64.0f));
}

static vec2 size(FT_Glyph_Metrics *m) {
    return vec2(ceilf(m->width/64.0f), ceilf(m->height/64.0f));
}

int AContext::add_glyph(FT_Face ftface, int sz, int dpi, int glyph)
{
    FT_Outline_Funcs ftfuncs = { ftMoveTo, ftLineTo, ftConicTo, ftCubicTo };
    FT_Glyph_Metrics *m = &ftface->glyph->metrics;
    FT_Error fterr;

    if ((fterr = FT_Set_Char_Size(ftface, 0, sz, dpi, dpi))) {
        Panic("error: FT_Set_Char_Size failed: fterr=%d\n", fterr);
    }

    if ((fterr = FT_Load_Glyph(ftface, glyph, 0))) {
        Panic("error: FT_Load_Glyph failed: fterr=%d\n", fterr);
    }

    int shape_num = new_shape(offset(m), size(m));

    if ((fterr = FT_Outline_Decompose(&ftface->glyph->outline, &ftfuncs, this))) {
        Panic("error: FT_Outline_Decompose failed: fterr=%d\n", fterr);
    }

    return shape_num;
}

int AEdge::num_points(int edge_type)
{
    switch (edge_type) {
    case EdgeLinear:                return 2;
    case EdgeQuadratic:             return 3;
    case EdgeCubic:                 return 4;
    case PrimitiveRectangle:        return 2;
    case PrimitiveCircle:           return 2;
    case PrimitiveEllipse:          return 2;
    case PrimitiveRoundedRectangle: return 2;
    }
    return 0;
}

int ABrush::num_points(int brush_type)
{
    switch (brush_type) {
    case BrushSolid:         return 0;
    case BrushAxial:         return 2;
    case BrushRadial:        return 2;
    }
    return 0;
}

int ABrush::num_colors(int brush_type)
{
    switch (brush_type) {
    case BrushSolid:         return 1;
    case BrushAxial:         return 2;
    case BrushRadial:        return 2;
    }
    return 0;
}


void AContext::clear()
{
    shapes.clear();
    contours.clear();
    edges.clear();
}

int AContext::new_shape(vec2 offset, vec2 size)
{
    int shape_num = (int)shapes.size();
    shapes.emplace_back(AShape{(float)contours.size(), 0,
        (float)edges.size(), 0, offset, size, -1, -1, 0 });
    return shape_num;
}

int AContext::new_contour()
{
    int contour_num = (int)contours.size();
    contours.emplace_back(AContour{(float)edges.size(), 0});
    shapes.back().contour_count++;
    return contour_num;
}

int AContext::new_edge(AEdge e)
{
    int edge_num = (int)edges.size();
    edges.push_back(e);
    if (shapes.back().contour_count > 0) {
        contours.back().edge_count++;
    }
    shapes.back().edge_count++;
    return edge_num;
}

int AContext::copy_shape(AShape *shape, AEdge *edges)
{
    int shape_num = new_shape(shape->offset, shape->size);
    AShape &s = shapes[shape_num];
    s.fill_brush = shape->fill_brush;
    s.stroke_brush = shape->stroke_brush;
    s.stroke_width = shape->stroke_width;
    for (int i = 0; i < shape->edge_count; i++) {
        new_edge(edges[i]);
    }
    return shape_num;
}

bool AContext::brush_equals(ABrush *b0, ABrush *b1)
{
    if (b0->type != b1->type) return false;
    int match_points = ABrush::num_points((int)b0->type);
    int match_colors = ABrush::num_colors((int)b0->type);
    for (int i = 0; i < match_points; i++) {
        if (b0->p[i] != b1->p[i]) return false;
    }
    for (int i = 0; i < match_colors; i++) {
        if (b0->c[i] != b1->c[i]) return false;
    }
    return true;
}

bool AContext::shape_equals(AShape *s0, AEdge *e0, AShape *s1, AEdge *e1)
{
    if (s0->edge_count != s1->edge_count) return false;
    if (s0->offset != s1->offset) return false;
    if (s0->size != s1->size) return false;
    if (s0->fill_brush != s1->fill_brush) return false;
    if (s0->stroke_brush != s1->stroke_brush) return false;
    if (s0->stroke_width != s1->stroke_width) return false;
    if (e0->type != e1->type) return false;
    for (int i = 0; i < s0->edge_count; i++) {
        int match_points = AEdge::num_points((int)e0[i].type);
        for (int j = 0; j < match_points; j++) {
            if (e0[i].p[j] != e1[i].p[j]) return false;
        }
    }
    return true;
}

int AContext::new_brush(ABrush b)
{
    int brush = (int)brushes.size();
    brushes.push_back(b);
    return brush;
}

int AContext::find_brush(ABrush *b)
{
    /* this is really inefficient! */
    for (size_t i = 0; i < brushes.size(); i++) {
        if (brush_equals(&brushes[i], b)) {
            return (int)i;
        }
    }
    return -1;
}

int AContext::add_brush(ABrush *b, bool dedup)
{
    int brush_num = dedup ? find_brush(b) : -1;
    if (brush_num == -1) {
        brush_num = new_brush(*b);
    }
    return brush_num;
}

bool AContext::update_brush(int brush_num, ABrush *b)
{
    bool update = !brush_equals(&brushes[brush_num], b);
    if (update) {
        brushes[brush_num] = *b;
    }
    return update;
}

int AContext::find_shape(AShape *s, AEdge *e)
{
    /* this is really inefficient! */
    for (size_t i = 0; i < shapes.size(); i++) {
        int j = (int)shapes[i].edge_offset;
        if (shape_equals(&shapes[i], &edges[j], s, e)) {
            return (int)i;
        }
    }
    return -1;
}

int AContext::add_shape(AShape *s, AEdge *e, bool dedup)
{
    int shape_num = dedup ? find_shape(s, e) : -1;
    if (shape_num == -1) {
        shape_num = copy_shape(s, e);
    }
    return shape_num;
}

bool AContext::update_shape(int shape_num, AShape *s, AEdge *e)
{
    AShape *os = &shapes[shape_num];
    AEdge *oe = &edges[(int)shapes[shape_num].edge_offset];

    if (shape_equals(os, oe, s, e)) {
        return false;
    }

    s->contour_offset = os->contour_offset;
    s->contour_count  = os->contour_count;
    s->edge_offset    = os->edge_offset;

    shapes[shape_num] = s[0];
    for (int i = 0; i < s->edge_count; i++) {
        edges[i] = e[i];
    }

    return true;
}

/*
 * draw list utility
 */

static vec2 transform(vec2 pos, mat3 matrix)
{
    vec3 v = vec3(pos, 1.0f) * matrix;
    return vec2(v.x / v.z, v.y / v.z);
}

static void rect(draw_list &b, uint iid, vec2 A, vec2 B, float Z,
    vec2 UV0, vec2 UV1, uint c, float s, mat3 m = mat3(1))
{
    uint o = static_cast<uint>(b.vertices.size());

    auto v1 = transform(A,m);
    auto v2 = transform(B,m);

    uint o0 = draw_list_vertex(b, {{v1.x, v1.y, Z}, {UV0.x, UV0.y}, c, s});
    uint o1 = draw_list_vertex(b, {{v2.x, v1.y, Z}, {UV1.x, UV0.y}, c, s});
    uint o2 = draw_list_vertex(b, {{v2.x, v2.y, Z}, {UV1.x, UV1.y}, c, s});
    uint o3 = draw_list_vertex(b, {{v1.x, v2.y, Z}, {UV0.x, UV1.y}, c, s});

    draw_list_indices(b, iid, mode_triangles, shader_canvas,
        {o0, o3, o1, o1, o3, o2});
}

static void rect(draw_list &batch, vec2 A, vec2 B, float Z,
    AContext &ctx, uint shape_num, uint color, mat3 m = mat3(1))
{
    AShape &shape = ctx.shapes[shape_num];
    auto &size = shape.size;
    auto &offset = shape.offset;
    auto t = mat3(size.x,       0,        offset.x ,
                       0,      -size.y,   size.y + offset.y ,
                       0,       0,        1);
    auto UV0 = vec3(0,0,1) * t;
    auto UV1 = vec3(1,1,1) * t;
    rect(batch, tbo_iid, A, B, Z, UV0, UV1, color, (float)shape_num, m);
}


/*
 * text renderer
 */

void text_renderer_canvas::render(draw_list &batch,
        std::vector<glyph_shape> &shapes,
        text_segment &segment, mat3 matrix)
{
    FT_Face ftface = static_cast<font_face_ft*>(segment.face)->ftface;
    int font_size = segment.font_size;
    int dpi = font_manager::dpi;
    float x_offset = 0;

    for (auto &s : shapes) {
        auto gi = glyph_map.find(s.glyph);
        if (gi != glyph_map.end()) continue;
        glyph_map[s.glyph] = ctx.add_glyph(ftface, glyph_size, dpi, s.glyph);
    }

    for (auto &s : shapes) {
        /* lookup shape num */
        int shape_num = glyph_map[s.glyph];
        AShape &shape = ctx.shapes[shape_num];

        /* figure out glyph dimensions */
        float s_scale = (float)font_size / (float)glyph_size;
        vec2 s_size = shape.size * s_scale;
        vec2 s_offset = shape.offset * s_scale;
        float y_offset = (font_size / 64.0f) - s_size.y;
        vec2 p1 = vec2(segment.x, segment.y) + vec2(x_offset, y_offset) +
            vec2(s_offset.x,-s_offset.y);
        vec2 p2 = p1 + vec2(s_size.x,s_size.y);

        /* emit geometry and advance */
        rect(batch, p1, p2, 0, ctx, shape_num, segment.color, matrix);
        x_offset += s.x_advance/64.0f + segment.tracking;
    }
}


/*
 * Canvas objects
 */

/* Drawable */

float Drawable::get_z() { return z; }
vec2 Drawable::get_position() { return pos; }
bool Drawable::is_visible() { return visible; }

void Drawable::set_z(float z) { this->z = z; }
void Drawable::set_position(vec2 pos) { this->pos = pos; }
void Drawable::set_visible(bool v) { visible = v; }

Brush Drawable::get_fill_brush() { return fill_brush; }
Brush Drawable::get_stroke_brush() { return stroke_brush; }
float Drawable::get_stroke_width() { return stroke_width; }

void Drawable::set_fill_brush(Brush brush) { fill_brush = brush; }
void Drawable::set_stroke_brush(Brush brush) { stroke_brush = brush; }
void Drawable::set_stroke_width(float width) { stroke_width = width; }

/* Edges */

void Edges::clear()
{
    edges.clear();
    contours.clear();
}

void Edges::new_contour()
{
    contours.emplace_back(AContour{(float)edges.size(), 0});
}

void Edges::new_edge(AEdge e)
{
    edges.push_back(e);
    if (contours.size() > 0) {
        contours.back().edge_count++;
    }
}

void Edges::set_offset(vec2 v) { offset = v; }
void Edges::set_size(vec2 v) { size = v; }

vec2 Edges::get_offset() { return offset; }
vec2 Edges::get_size() { return size; }

/* Patch */

Patch* Patch::new_contour() {
    Edges::new_contour();
    return this;
}

Patch* Patch::new_line(vec2 p1, vec2 p2) {
    Edges::new_edge(AEdge{EdgeLinear, { p1, p2 }});
    return this;
}

Patch* Patch::new_quadratic_curve(vec2 p1, vec2 c1, vec2 p2) {
    Edges::new_edge(AEdge{EdgeQuadratic, { p1, c1, p2 }});
    return this;
}

/* Path */

Path* Path::new_contour() {
    Edges::new_contour();
    return this;
}

Path* Path::new_line(vec2 p1, vec2 p2) {
    Edges::new_edge(AEdge{EdgeLinear, { p1, p2 }});
    return this;
}

Path* Path::new_quadratic_curve(vec2 p1, vec2 c1, vec2 p2) {
    Edges::new_edge(AEdge{EdgeQuadratic, { p1, c1, p2 }});
    return this;
}

/* TextStyle */

float TextStyle::get_size() { return size; }
font_face* TextStyle::get_face() { return face; }
text_halign TextStyle::get_halign() { return halign; }
text_valign TextStyle::get_valign() { return valign; }
std::string TextStyle::get_lang() { return lang; }
Brush TextStyle::get_fill_brush() { return fill_brush; }
Brush TextStyle::get_stroke_brush() { return stroke_brush; }
float TextStyle::get_stroke_width() { return stroke_width; }

void TextStyle::set_size(float size) { this->size = size; }
void TextStyle::set_face(font_face *face) { this->face = face; }
void TextStyle::set_halign(text_halign halign) { this->halign = halign; }
void TextStyle::set_valign(text_valign valign) { this->valign = valign; }
void TextStyle::set_lang(std::string lang) { this->lang = lang; }
void TextStyle::set_fill_brush(Brush fill_brush) { this->fill_brush = fill_brush; }
void TextStyle::set_stroke_brush(Brush stroke_brush) { this->stroke_brush = stroke_brush; }
void TextStyle::set_stroke_width(float stroke_width) { this->stroke_width = stroke_width; }

/* Text */

float Text::get_size() { return size; }
font_face* Text::get_face() { return face; }
text_halign Text::get_halign() { return halign; }
text_valign Text::get_valign() { return valign; }
std::string Text::get_text() { return text; }
std::string Text::get_lang() { return lang; }
Text::render_as Text::get_render_mode() { return mode;}

void Text::set_size(float size) { shapes.clear(); this->size = size; }
void Text::set_face(font_face *face) { shapes.clear(); this->face = face; }
void Text::set_halign(text_halign halign) { this->halign = halign; }
void Text::set_valign(text_valign valign) { this->valign = valign; }
void Text::set_lang(std::string lang) { shapes.clear(); this->lang = lang; }
void Text::set_render_mode(Text::render_as mode) { this->mode = mode; }

void Text::set_text(std::string text)
{
    if (this->text == text) {
        return;
    }
    shapes.clear();
    this->text = text;
}

TextStyle Text::get_text_style() {
    return TextStyle{size, face, halign, valign, lang, fill_brush, stroke_brush, stroke_width};
}

void Text::set_text_style(TextStyle style) {
    shapes.clear();
    size = style.size;
    face = style.face;
    halign = style.halign;
    valign = style.valign;
    lang = style.lang;
    fill_brush = style.fill_brush;
    stroke_brush = style.stroke_brush;
    stroke_width = style.stroke_width;
}

text_segment& Text::get_text_segment() {
    int font_size = (int)(size * 64.0f);
    segment = text_segment(text, lang, face, font_size, 0, 0, 0xffffffff);
    return segment;
}

std::vector<glyph_shape>& Text::get_glyph_shapes() {
    if (shapes.size() == 0) {
        canvas->text_shaper.shape(shapes, get_text_segment());
    }
    return shapes;
}

vec2 Text::get_text_size() {
    std::vector<glyph_shape> &shapes = get_glyph_shapes();
    float text_width = std::accumulate(shapes.begin(), shapes.end(), 0.0f,
        [](float t, glyph_shape& s) { return t + s.x_advance/64.0f; });
    return vec2(text_width,size);
}

/*
 * Primitive subclasses are single edge shapes with no contours
 */

/* Circle */

vec2 Circle::get_origin() { return origin; }
void Circle::set_origin(vec2 v) { origin = v; }
float Circle::get_radius() { return radius; }
void Circle::set_radius(float v) { radius = v; }

/* Ellipse */

vec2 Ellipse::get_origin() { return origin; }
void Ellipse::set_origin(vec2 v) { origin = v; }
vec2 Ellipse::get_halfsize() { return half_size; }
void Ellipse::set_halfsize(vec2 v) { half_size = v; }

/* Rectangle */

vec2 Rectangle::get_origin() { return origin; }
void Rectangle::set_origin(vec2 v) { origin = v; }
vec2 Rectangle::get_halfsize() { return half_size; }
void Rectangle::set_halfsize(vec2 v) { half_size = v; }
float Rectangle::get_radius() { return radius; }
void Rectangle::set_radius(float v) { radius = v; }

/*
 * Canvas API
 */

Canvas::Canvas(font_manager* manager) :
    objects(), glyph_map(), ctx(std::make_unique<AContext>()),
    text_renderer_c(*ctx, glyph_map), text_renderer_r(manager), manager(manager),
    fill_brush{BrushSolid, {vec2(0)}, {color(0,0,0,1)}},
    stroke_brush{BrushSolid, {vec2(0)}, {color(0,0,0,1)}},
    stroke_width(0.0f), scale(1.0f) {}

void Canvas::set_transform(mat3 m)
{
    transform = m;
    transform_inv = glm::inverseTranspose(m);
}

mat3 Canvas::get_transform()
{
    return transform;
}

mat3 Canvas::get_inverse_transform()
{
    return transform_inv;
}

void Canvas::set_render_mode(Text::render_as mode)
{
    text_mode = mode;
}

Text::render_as Canvas::get_render_mode()
{
    return text_mode;
}

void Canvas::set_scale(float scale)
{
    if (scale != this->scale) {
        float factor = scale / this->scale;
        this->scale = scale;
        /* update strokes on existing shapes */
        for (auto &shape : ctx->shapes) {
            shape.stroke_width = shape.stroke_width * factor;
        }
    }
}

float Canvas::get_scale()
{
    return scale;
}

int Canvas::get_brush_num(Brush p)
{
    if (p.brush_type == BrushNone) {
        return -1;
    } else {
        color *c = p.colors;
        vec2 *P = p.points;
        vec4 C[4] = {
            { c[0].r, c[0].g, c[0].b, c[0].a },
            { c[1].r, c[1].g, c[1].b, c[1].a },
            { c[2].r, c[2].g, c[2].b, c[2].a },
            { c[3].r, c[3].g, c[3].b, c[3].a },
        };
        ABrush tmpl{(float)(int)p.brush_type,
            { P[0], P[1], P[2], P[3] }, { C[0], C[1], C[2], C[3] } };
        return ctx->add_brush(&tmpl);
    }
}

Brush Canvas::get_brush(int brush_num) {
    if (brush_num == -1) {
        return Brush{BrushNone, { vec2(0,0)}, { color(0,0,0,1) } };
    } else {
        ABrush &b = ctx->brushes[brush_num];
        return Brush{(BrushType)(int)b.type,
            { b.p[0], b.p[1], b.p[2], b.p[3]},
            { color(b.c[0].r, b.c[0].g, b.c[0].b, b.c[0].a),
              color(b.c[1].r, b.c[1].g, b.c[1].b, b.c[1].a),
              color(b.c[2].r, b.c[2].g, b.c[2].b, b.c[2].a),
              color(b.c[3].r, b.c[3].g, b.c[3].b, b.c[3].a) }
        };
    }
}

Brush Canvas::get_fill_brush() { return fill_brush; }
Brush Canvas::get_stroke_brush() { return stroke_brush; }
float Canvas::get_stroke_width() { return stroke_width; }

void Canvas::set_fill_brush(Brush brush) { fill_brush = brush; }
void Canvas::set_stroke_brush(Brush brush) { stroke_brush = brush; }
void Canvas::set_stroke_width(float width) { stroke_width = width; }

size_t Canvas::num_drawables() { return objects.size(); }

Drawable* Canvas::get_drawable(size_t offset) {
    return objects[offset].get();
}

void Canvas::clear()
{
    glyph_map.clear();
    objects.clear();
    ctx->clear();
    fill_brush = Brush{BrushSolid, {vec2(0)}, {color(0,0,0,1)}};
    stroke_brush = Brush{BrushSolid, {vec2(0)}, {color(0,0,0,1)}};
    stroke_width = 0;
}

Patch* Canvas::new_patch(vec2 offset, vec2 size)
{
    auto o = new Patch{this, 1, drawable_patch, (int)objects.size(),
        -1, vec2(0), 0.0f, fill_brush, stroke_brush, stroke_width };
    o->set_offset(offset);
    o->set_size(size);
    objects.push_back(std::unique_ptr<Drawable>(o));
    return o;
}

Path* Canvas::new_path(vec2 offset, vec2 size)
{
    auto o = new Path{this, 1, drawable_path, (int)objects.size(),
        -1, vec2(0), 0.0f, fill_brush, stroke_brush, stroke_width };
    o->set_offset(offset);
    o->set_size(size);
    objects.push_back(std::unique_ptr<Drawable>(o));
    return o;
}

Text* Canvas::new_text()
{
    auto o = new Text{this, 1, drawable_text, (int)objects.size(),
        -1, vec2(0), 0.0f, fill_brush, stroke_brush, stroke_width };
    o->set_render_mode(text_mode);
    objects.push_back(std::unique_ptr<Drawable>(o));
    return o;
}

Text* Canvas::new_text(TextStyle text_style)
{
    auto o = new Text{this, 1, drawable_text, (int)objects.size(),
        -1, vec2(0), 0.0f, fill_brush, stroke_brush, stroke_width };
    o->set_text_style(text_style);
    o->set_render_mode(text_mode);
    objects.push_back(std::unique_ptr<Drawable>(o));
    return o;
}

Circle* Canvas::new_circle(vec2 pos, float radius)
{
    auto o = new Circle{this, 1, drawable_circle, (int)objects.size(),
        -1, pos, 0.0f, fill_brush, stroke_brush, stroke_width };
    o->set_radius(radius);
    objects.push_back(std::unique_ptr<Drawable>(o));
    return o;
}

Ellipse* Canvas::new_ellipse(vec2 pos, vec2 half_size)
{
    auto o = new Ellipse{this, 1, drawable_ellipse, (int)objects.size(),
        -1, pos, 0.0f, fill_brush, stroke_brush, stroke_width };
    o->set_halfsize(half_size);
    objects.push_back(std::unique_ptr<Drawable>(o));
    return o;
}

Rectangle* Canvas::new_rectangle(vec2 pos, vec2 half_size)
{
    auto o = new Rectangle{this, 1, drawable_rectangle, (int)objects.size(),
        -1, pos, 0.0f, fill_brush, stroke_brush, stroke_width };
    o->set_halfsize(half_size);
    objects.push_back(std::unique_ptr<Drawable>(o));
    return o;
}

Rectangle* Canvas::new_rounded_rectangle(vec2 pos, vec2 half_size, float radius)
{
    auto o = new Rectangle{this, 1, drawable_rectangle, (int)objects.size(),
        -1, pos, 0.0f, fill_brush, stroke_brush, stroke_width };
    o->set_halfsize(half_size);
    o->set_radius(radius);
    objects.push_back(std::unique_ptr<Drawable>(o));
    return o;
}

void Canvas::emit(draw_list &batch)
{
    glyph_map.clear();
    ctx->clear();

    for (auto &o : objects) {
        if (!o->visible) continue;
        switch (o->drawable_type) {
        case drawable_patch: {
            auto shape = static_cast<Patch*>(o.get());
            int fill_brush_num = get_brush_num(shape->fill_brush);
            int stroke_brush_num = get_brush_num(shape->stroke_brush);

            int shape_num = shape->ll_shape_num = ctx->new_shape(shape->offset, shape->size);
            ctx->shapes[shape_num].fill_brush = (float)fill_brush_num;
            ctx->shapes[shape_num].stroke_brush = (float)stroke_brush_num;
            ctx->shapes[shape_num].stroke_width = shape->stroke_width * scale;
            ctx->shapes[shape_num].stroke_mode = 0.0f; /* interior */
            ctx->shapes[shape_num].contour_offset = (float)ctx->contours.size();
            ctx->shapes[shape_num].contour_count = (float)shape->contours.size();
            for (size_t i = 0; i < shape->contours.size(); i++) {
                ctx->contours.push_back(shape->contours[i]);
                ctx->contours.back().edge_offset += ctx->edges.size();
            }
            ctx->shapes[shape_num].edge_offset = (float)ctx->edges.size();
            ctx->shapes[shape_num].edge_count = (float)shape->edges.size();
            for (size_t i = 0; i < shape->edges.size(); i++) {
                ctx->edges.push_back(shape->edges[i]);
            }

            AShape &llshape = ctx->shapes[shape->ll_shape_num];
            vec2 pos = shape->get_position() + llshape.offset;
            vec2 halfSize = llshape.size / 2.0f;
            float padding = ceil(llshape.stroke_width/2.0f);
            Brush fill_brush = get_brush((int)llshape.fill_brush);
            uint32_t c = 0xffffffff;
            rect(batch, tbo_iid,
                pos - halfSize - padding, pos + halfSize + padding,
                shape->get_z(), -vec2(padding), halfSize * 2.0f + padding, c,
                (float)o->ll_shape_num, transform);
            break;
        }
        case drawable_path: {
            auto shape = static_cast<Path*>(o.get());
            int fill_brush_num = get_brush_num(shape->fill_brush);
            int stroke_brush_num = get_brush_num(shape->stroke_brush);

            int shape_num = shape->ll_shape_num = ctx->new_shape(shape->offset, shape->size);
            ctx->shapes[shape_num].fill_brush = (float)fill_brush_num;
            ctx->shapes[shape_num].stroke_brush = (float)stroke_brush_num;
            ctx->shapes[shape_num].stroke_width = shape->stroke_width * scale;
            ctx->shapes[shape_num].stroke_mode = 1.0f; /* no interior */
            ctx->shapes[shape_num].contour_offset = (float)ctx->contours.size();
            ctx->shapes[shape_num].contour_count = (float)shape->contours.size();
            for (size_t i = 0; i < shape->contours.size(); i++) {
                ctx->contours.push_back(shape->contours[i]);
                ctx->contours.back().edge_offset += ctx->edges.size();
            }
            ctx->shapes[shape_num].edge_offset = (float)ctx->edges.size();
            ctx->shapes[shape_num].edge_count = (float)shape->edges.size();
            for (size_t i = 0; i < shape->edges.size(); i++) {
                ctx->edges.push_back(shape->edges[i]);
            }

            AShape &llshape = ctx->shapes[shape->ll_shape_num];
            vec2 pos = shape->get_position() + llshape.offset;
            vec2 halfSize = llshape.size / 2.0f;
            float padding = ceil(llshape.stroke_width/2.0f);
            Brush fill_brush = get_brush((int)llshape.fill_brush);
            uint32_t c = 0xffffffff;
            rect(batch, tbo_iid,
                pos - halfSize - padding, pos + halfSize + padding,
                shape->get_z(), -vec2(padding), halfSize * 2.0f + padding, c,
                (float)o->ll_shape_num, transform);
            break;
        }
        case drawable_text: {
            auto shape = static_cast<Text*>(o.get());
            size_t s = glyph_map.size();
            vec2 pos = shape->get_position();
            text_segment &segment = shape->get_text_segment();
            std::vector<glyph_shape> &shapes = shape->get_glyph_shapes();
            vec2 text_size = shape->get_text_size();
            switch (shape->halign) {
            case text_halign_left:   segment.x = pos.x; break;
            case text_halign_center: segment.x = pos.x - text_size.x/2.0f; break;
            case text_halign_right:  segment.x = pos.x - text_size.x; break;
            }
            switch (shape->valign) {
            case text_valign_top:    segment.y = pos.y - text_size.y; break;
            case text_valign_center: segment.y = pos.y - text_size.y/2.0f; break;
            case text_valign_bottom: segment.y = pos.y; break;
            }
            if (shape->get_render_mode() == Text::render_as_text &&
                shape->get_stroke_width() == 0 &&
                shape->get_fill_brush().brush_type == BrushSolid)
            {
                /* todo - use fast text renderer (currently disabled) */
                segment.baseline_shift = -text_size.y;
                segment.color = shape->get_fill_brush().colors[0].rgba32();
                text_renderer_r.render(batch, shapes, segment, transform);
            } else {
                /* todo - brushes are stored on each glyph shape */
                segment.color = shape->get_fill_brush().colors[0].rgba32();
                text_renderer_c.render(batch, shapes, segment, transform);
            }
            break;
        }
        case drawable_circle: {
            auto shape = static_cast<Circle*>(o.get());
            int fill_brush_num = get_brush_num(shape->fill_brush);
            int stroke_brush_num = get_brush_num(shape->stroke_brush);

            AShape s{0, 0, 0, 1, vec2(0), vec2(shape->radius * 2.0f),
                (float)fill_brush_num, (float)stroke_brush_num, shape->stroke_width * scale };
            AEdge e{PrimitiveCircle,{vec2(shape->radius), vec2(shape->radius)}};
            shape->ll_shape_num = ctx->add_shape(&s, &e, false);

            AShape &llshape = ctx->shapes[shape->ll_shape_num];
            vec2 pos = shape->get_position();
            float radius = shape->get_radius();
            float padding = ceil(llshape.stroke_width/2.0f);
            Brush fill_brush = get_brush((int)llshape.fill_brush);
            uint32_t c = 0xffffffff;
            rect(batch, tbo_iid,
                pos - radius - padding, pos + radius + padding,
                shape->get_z(), -vec2(padding), vec2(radius) * 2.0f + padding, c,
                (float)o->ll_shape_num, transform);
            break;
        }
        case drawable_ellipse: {
            auto shape = static_cast<Ellipse*>(o.get());
            int fill_brush_num = get_brush_num(shape->fill_brush);
            int stroke_brush_num = get_brush_num(shape->stroke_brush);

            AShape s{0, 0, 0, 1, vec2(0), shape->half_size * 2.0f,
                (float)fill_brush_num, (float)stroke_brush_num, shape->stroke_width * scale };
            AEdge e{PrimitiveEllipse,{shape->half_size, shape->half_size}};
            shape->ll_shape_num = ctx->add_shape(&s, &e, false);

            AShape &llshape = ctx->shapes[shape->ll_shape_num];
            vec2 pos = shape->get_position();
            vec2 halfSize = shape->get_halfsize();
            float padding = ceil(llshape.stroke_width/2.0f);
            Brush fill_brush = get_brush((int)llshape.fill_brush);
            uint32_t c = 0xffffffff;
            rect(batch, tbo_iid,
                pos - halfSize - padding, pos + halfSize + padding,
                shape->get_z(), -vec2(padding), halfSize * 2.0f + padding, c,
                (float)o->ll_shape_num, transform);
            break;
        }
        case drawable_rectangle: {
            auto shape = static_cast<Rectangle*>(o.get());
            int fill_brush_num = get_brush_num(shape->fill_brush);
            int stroke_brush_num = get_brush_num(shape->stroke_brush);

            AShape s{0, 0, 0, 1, vec2(0), shape->half_size * 2.0f,
                (float)fill_brush_num, (float)stroke_brush_num, shape->stroke_width * scale };
            AEdge e = (shape->radius > 0.0f) ?
                AEdge{PrimitiveRoundedRectangle,{shape->half_size, shape->half_size, vec2(shape->radius)}} :
                AEdge{PrimitiveRectangle,{shape->half_size, shape->half_size}};
            shape->ll_shape_num = ctx->add_shape(&s, &e, false);

            AShape &llshape = ctx->shapes[shape->ll_shape_num];
            vec2 pos = shape->get_position();
            vec2 halfSize = shape->get_halfsize();
            float padding = ceil(llshape.stroke_width/2.0f);
            Brush fill_brush = get_brush((int)llshape.fill_brush);
            uint32_t c = 0xffffffff;
            rect(batch, tbo_iid,
                pos - halfSize - padding, pos + halfSize + padding,
                shape->get_z(), -vec2(padding), halfSize * 2.0f + padding, c,
                (float)o->ll_shape_num, transform);
            break;
        }
        }
    }
}
