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
#include <atomic>
#include <mutex>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H

#include <hb.h>
#include <hb-ft.h>

#include "binpack.h"
#include "utf8.h"
#include "draw.h"
#include "font.h"
#include "glyph.h"
#include "image.h"
#include "file.h"


/*
 * freetype span rasterization
 */

void span_measure::fn(int y, int count, const FT_Span* spans, void *user)
{
    span_measure *s = static_cast<span_measure*>(user);
    s->min_y = (std::min)(s->min_y, y);
    s->max_y = (std::max)(s->max_y, y);
    for (int i = 0; i < count; i++) {
        s->min_x = (std::min)(s->min_x, (int)spans[i].x);
        s->max_x = (std::max)(s->max_x, (int)spans[i].x + spans[i].len);
    }
}

void span_vector::reset(int width, int height)
{
    pixels.clear();
    pixels.resize(width * height);
    w = width;
    h = height;
}

void span_vector::fn(int y, int count, const FT_Span* spans, void *user)
{
    span_vector *s = static_cast<span_vector*>(user);
    int dy = (std::max)((std::min)(s->gy + s->oy + y, s->h - 1), 0);
    s->min_y = (std::min)(s->min_y, y);
    s->max_y = (std::max)(s->max_y, y);
    for (int i = 0; i < count; i++) {
        s->min_x = (std::min)(s->min_x, (int)spans[i].x);
        s->max_x = (std::max)(s->max_x, (int)spans[i].x + spans[i].len);
        int dx = (std::max)((std::min)(s->gx + s->ox + spans[i].x, s->w), 0);
        int dl = (std::max)((std::min)((int)spans[i].len, s->w - dx), 0);
        if (dl > 0) {
            memset(&s->pixels[dy * s->w + dx], spans[i].coverage, dl);
        }
    }
}


/*
 * font atlas
 */

font_atlas::font_atlas() :
    font_atlas(font_atlas::DEFAULT_WIDTH, font_atlas::DEFAULT_HEIGHT,
    font_atlas::DEFAULT_DEPTH) {}

font_atlas::font_atlas(size_t width, size_t height, size_t depth) :
    width(width), height(height), depth(depth),
    glyph_map(), pixels(), uv1x1(1.0f / (float)width),
    bp(bin_point((int)width, (int)height)),
    delta(bin_point(width,height),bin_point(0,0)),
    multithreading(false), mutex()
{
    init();
}

void font_atlas::init()
{
    /* reserve 0x0 - 1x1 with padding */
    bp.find_region(0, bin_point(2,2));

    /* clear bitmap */
    pixels.clear();
    pixels.resize(width * height * depth);
    switch (depth) {
        case 1:
            pixels[0] = 0xff;
            break;
        case 4:
            for (int x = 0; x < width; x++) {
                for (int y = 0; y < height; y++) {
                    *(uint32_t*)&pixels[(y * width + x) * depth] = 0xff000000;
                }
            }
            *static_cast<uint32_t*>(static_cast<void*>(&pixels[0])) = 0xffffffff;
            break;
    }
}

void font_atlas::reset(size_t width, size_t height, size_t depth)
{
    this->width = width;
    this->height = height;
    this->depth = depth;

    uv1x1 = 1.0f / (float)width;
    bp.set_bin_size(bin_point(width, height));
    delta = bin_rect(bin_point(width,height),bin_point(0,0));

    init();
}

atlas_entry font_atlas::create(font_face *face, int font_size, int glyph,
    int entry_font_size, int ox, int oy, int w, int h)
{
    atlas_entry ae;

    if (multithreading) {
        mutex.lock();
    }

    int bin_id = (int)glyph_map.size();
    auto r = bp.find_region(bin_id, bin_point(w + PADDING , h + PADDING));
    if (!r.first) {
        if (multithreading) {
            mutex.unlock();
        }
        memset(&ae, 0, sizeof(ae));
        return ae; /* atlas full */
    }

    /* track minimum update rectangle */
    expand_delta(r.second);

    /* create uv coordinates */
    float x1 = 0.5f + r.second.a.x,     y1 = 0.5f + r.second.a.y;
    float x2 = 0.5f + r.second.b.x - 1, y2 = 0.5f + r.second.b.y - 1;
    float uv[4] = { x1/width, y2/width, x2/width, y1/width };

    /* insert into glyph_map */
    auto a = r.second.a;
    auto gi = glyph_map.insert(glyph_map.end(),
        std::pair<atlas_key,atlas_entry>({face->font_id, font_size, glyph},
            {bin_id, entry_font_size, a.x, a.y, ox, oy, w, h, uv}));

    ae = gi->second;

    if (multithreading) {
        mutex.unlock();
    }

    return ae;
}

bin_rect font_atlas::get_delta()
{
    /*
     * return the dela rectangle, and reset it to its smallest value.
     *
     * This interface is called to get the smalled possible update
     * rectangle to use with APIs such as glTexSubImage2D.
     */
    bin_rect r = delta;
    delta = bin_rect(bin_point(width,height),bin_point(0,0));
    return r;
}

void font_atlas::expand_delta(bin_rect b)
{
    /*
     * expand the delta rectangle.
     *
     * This interface is called after find_region with each newly
     * allocated region, to keep track of the minimum update rectangle.
     */
    delta.a.x = (std::min)(delta.a.x,b.a.x);
    delta.a.y = (std::min)(delta.a.y,b.a.y);
    delta.b.x = (std::max)(delta.b.x,b.b.x);
    delta.b.y = (std::max)(delta.b.y,b.b.y);
}

atlas_entry font_atlas::resize(font_face *face, int font_size, int glyph,
    atlas_entry *tmpl)
{
    float scale = (float)font_size / tmpl->font_size;
    auto gi = glyph_map.insert(glyph_map.end(),
        std::pair<atlas_key,atlas_entry>(
            { face->font_id, font_size, glyph},
            { tmpl->bin_id, font_size, tmpl->x, tmpl->y,
              (short)roundf((float)tmpl->ox * scale),
              (short)roundf((float)tmpl->oy * scale),
              (short)roundf((float)tmpl->w * scale),
              (short)roundf((float)tmpl->h * scale),
              tmpl->uv }));
    return gi->second;
}

atlas_entry font_atlas::lookup(font_face *face, int font_size, int glyph,
    glyph_renderer *renderer)
{
    atlas_entry ae;

    /*
     * lookup atlas to see if the glyph is in the atlas
     */
    auto gi = glyph_map.find({face->font_id, font_size, glyph});
    if (gi != glyph_map.end()) {
        return gi->second;
    }

    /*
     * lookup index with font size 0 which is used for variable
     * sized entries created by signed distance field renderers.
     */
    gi = glyph_map.find({face->font_id, 0, glyph});
    if (gi != glyph_map.end()) {
        return resize(face, font_size, glyph, &gi->second);
    }

    /*
     * rendering a glyph may create a variable size entry, so
     * we check that we got the font size that we requested.
     */
    ae = renderer->render(this, static_cast<font_face_ft*>(face),
        font_size, glyph);
    if (ae.font_size != font_size) {
        return resize(face, font_size, glyph, &ae);
    } else {
        return ae;
    }
}

std::string font_atlas::get_path(font_face *face, file_type type)
{
    switch (type) {
    case csv_file: return face->path + ".atlas.csv";
    case png_file: return face->path + ".atlas.png";
    case ttf_file:
    default: return face->path;
    }
}

#define FLOAT32 "%.9g"

void font_atlas::save_map(font_manager *manager, FILE *out)
{
    std::vector<atlas_key> v;
    for(auto i = glyph_map.begin(); i != glyph_map.end(); i++) {
        v.push_back(i->first);
    }
    sort(v.begin(), v.end(), [&](const atlas_key &a, const atlas_key &b)
        -> bool { return glyph_map[a].bin_id < glyph_map[b].bin_id; });
    for (auto &k : v) {
        auto i = glyph_map.find(k);
        const atlas_key &key = i->first;
        const atlas_entry &ent = i->second;
        fprintf(out, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
            ent.bin_id, key.font_id(), key.glyph(),
            ent.font_size, ent.x, ent.y, ent.ox, ent.oy, ent.w, ent.h);
    }
}

void font_atlas::load_map(font_manager *manager, FILE *in)
{
    const int num_fields = 10;
    int ret;
    do {
        int bin_id, font_id, glyph;
        atlas_entry ent;
        ret = fscanf(in, "%d,%d,%d,%d,%hd,%hd,%hd,%hd,%hd,%hd\n",
            &bin_id, &font_id, &glyph, &ent.font_size,
            &ent.x, &ent.y, &ent.ox, &ent.oy, &ent.w, &ent.h);
        if (ret == num_fields) {
            font_face *face = manager->findFontById(font_id);
            create(face, /*size*/0, glyph, ent.font_size, ent.ox, ent.oy, ent.w, ent.h);
        }
    } while (ret == num_fields);
}

void font_atlas::save(font_manager *manager, font_face *face)
{
    std::string img_path = get_path(face, png_file);
    std::string csv_path = get_path(face, csv_file);
    FILE *fcsv = fopen(csv_path.c_str(), "w");
    if (fcsv == nullptr) {
        fprintf(stderr, "error: fopen: %s: %s\n",
            csv_path.c_str(), strerror(errno));
        exit(1);
    }
    save_map(manager, fcsv);
    fclose(fcsv);
    image_ptr img = image::createBitmap(width, height,
        depth == 4 ? pixel_format_rgba : pixel_format_alpha, &pixels[0]);
    image::saveToFile(img_path, img);
}

void font_atlas::load(font_manager *manager, font_face *face)
{
    std::string img_path = get_path(face, png_file);
    std::string csv_path = get_path(face, csv_file);
    if (!file::fileExists(img_path) || !file::fileExists(csv_path)) {
        return;
    }
    FILE *fcsv = fopen(csv_path.c_str(), "r");
    if (fcsv == nullptr) {
        fprintf(stderr, "error: fopen: %s: %s\n",
            csv_path.c_str(), strerror(errno));
        exit(1);
    }
    load_map(manager, fcsv);
    fclose(fcsv);
    image_ptr img = image::createFromFile(img_path);
    reset(img->getWidth(), img->getHeight(), img->getBytesPerPixel());
    for (size_t y = 0; y < width; y++) {
        uint8_t *src = img->getData() + y * width * depth;
        uint8_t *dest = pixels.data() + y * width * depth;
        memcpy(dest, src, width * depth);
    }
}


/*
 * text shaper (FreeType)
 */

void text_shaper_ft::shape(std::vector<glyph_shape> &shapes, text_segment *segment)
{
    font_face_ft *face = static_cast<font_face_ft*>(segment->face);
    FT_Face ftface = face->ftface;
    FT_GlyphSlot ftglyph = ftface->glyph;
    FT_Error fterr;

    /* we need to set up our font metrics */
    face->get_metrics(segment->font_size);

    /* shape text with FreeType (does not apply glyph-glyph kerning) */
    const char* text = segment->text.c_str();
    size_t text_len = segment->text.size();
    for (size_t i = 0; i < text_len; i += utf8_codelen(text + i)) {

        uint32_t codepoint = utf8_to_utf32(text + i);
        uint32_t glyph = FT_Get_Char_Index(ftface, codepoint);

        if ((fterr = FT_Load_Glyph(ftface, glyph, 0))) {
            fprintf(stderr, "error: FT_Load_Glyph failed: glyph=%d fterr=%d\n",
                glyph, fterr);
            return;
        }
        if (ftface->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
            fprintf(stderr, "error: FT_Load_Glyph format is not outline\n");
            return;
        }

        shapes.push_back({
            glyph, (unsigned)i,
            (int)ftglyph->metrics.horiBearingX,
            0,
            (int)ftglyph->advance.x,
            (int)ftglyph->advance.y
        });
    }
}

/*
 * text shaper (HarfBuff)
 */

void text_shaper_hb::shape(std::vector<glyph_shape> &shapes, text_segment *segment)
{
    font_face_ft *face = static_cast<font_face_ft*>(segment->face);
    FT_Face ftface = face->ftface;

    hb_font_t *hbfont;
    hb_language_t hblang;
    hb_glyph_info_t *glyph_info;
    hb_glyph_position_t *glyph_pos;
    unsigned glyph_count;

    /* we need to set up our font metrics */
    face->get_metrics(segment->font_size);

    /* get text to render */
    const char* text = segment->text.c_str();
    size_t text_len = segment->text.size();

    /* create text buffers */
    hbfont  = hb_ft_font_create(ftface, NULL);
    hblang = hb_language_from_string(segment->language.c_str(),
        (int)segment->language.size());

    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hblang);
    hb_buffer_add_utf8(buf, text, (int)text_len, 0, (int)text_len);

    /* shape text with HarfBuzz */
    hb_shape(hbfont, buf, NULL, 0);
    glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
    glyph_pos = hb_buffer_get_glyph_positions(buf, &glyph_count);

    for (size_t i = 0; i < glyph_count; i++) {
        shapes.push_back({
            glyph_info[i].codepoint, glyph_info[i].cluster,
            glyph_pos[i].x_offset, glyph_pos[i].y_offset,
            glyph_pos[i].x_advance, glyph_pos[i].y_advance
        });
    }

    hb_buffer_destroy(buf);
}

/*
 * glyph renderer
 */

atlas_entry glyph_renderer_ft::render(font_atlas *atlas, font_face_ft *face,
    int font_size, int glyph)
{
    FT_Library ftlib;
    FT_Face ftface;
    FT_Error fterr;
    FT_GlyphSlot ftglyph;
    FT_Raster_Params rp;
    int ox, oy, w, h;
    atlas_entry ae;

    /* freetype library and glyph pointers */
    ftface = face->ftface;
    ftglyph = ftface->glyph;
    ftlib = ftglyph->library;

    /* we need to set up our font metrics */
    face->get_metrics(font_size);

    /* load glyph */
    if ((fterr = FT_Load_Glyph(ftface, glyph, 0))) {
        fprintf(stderr, "error: FT_Load_Glyph failed: glyph=%d fterr=%d\n",
            glyph, fterr);
        return atlas_entry(-1);
    }
    if (ftface->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        fprintf(stderr, "error: FT_Load_Glyph format is not outline\n");
        return atlas_entry(-1);
    }

    /* set up render parameters */
    rp.target = 0;
    rp.flags = FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_AA;
    rp.user = &span;
    rp.black_spans = 0;
    rp.bit_set = 0;
    rp.bit_test = 0;
    rp.gray_spans = span_vector::fn;

    /* font dimensions */
    ox = (int)floorf((float)ftglyph->metrics.horiBearingX / 64.0f) - 1;
    oy = (int)floorf((float)(ftglyph->metrics.horiBearingY -
        ftglyph->metrics.height) / 64.0f) - 1;
    w = (int)ceilf(ftglyph->metrics.width / 64.0f) + 2;
    h = (int)ceilf(ftglyph->metrics.height / 64.0f) + 2;

    /* set up span vector dimensions */
    span.gx = 0;
    span.gy = 0;
    span.ox = -ox;
    span.oy = -oy;
    span.min_x = INT_MAX;
    span.min_y = INT_MAX;
    span.max_x = INT_MIN;
    span.max_y = INT_MIN;
    span.reset(w, h);

    /* rasterize glyph */
    if ((fterr = FT_Outline_Render(ftlib, &ftface->glyph->outline, &rp))) {
        printf("error: FT_Outline_Render failed: fterr=%d\n", fterr);
        return atlas_entry(-1);
    }

    if (span.min_x == INT_MAX && span.min_y == INT_MAX) {
        /* create atlas entry for white space glyph with zero dimensions */
        ae = atlas->create(face, font_size, glyph, font_size, 0, 0, 0, 0);
    } else {
        /* create atlas entry for glyph using dimensions from span */
        ae = atlas->create(face, font_size, glyph, font_size, ox, oy, w, h);
        /* copy pixels from span to atlas */
        if (ae.bin_id >= 0) {
            for (int i = 0; i < span.h; i++) {
                size_t src = i * span.w;
                size_t dst = (ae.y + i) * atlas->width + ae.x;
                memcpy(&atlas->pixels[dst], &span.pixels[src], span.w);
            }
        }
    }

    return ae;
}

/*
 * text renderer
 */

void text_renderer_ft::render(draw_list &batch,
    std::vector<glyph_shape> &shapes,
    text_segment *segment)
{
    font_face_ft *face = static_cast<font_face_ft*>(segment->face);
    int font_size = segment->font_size;
    int baseline_shift = segment->baseline_shift;
    int tracking = segment->tracking;
    font_atlas *atlas = manager->getFontAtlas(face);
    glyph_renderer *renderer = manager->getGlyphRenderer(face);

    /* lookup glyphs in font atlas, creating them if they don't exist */
    float dx = 0, dy = 0;
    for (auto shape : shapes) {
        atlas_entry ae = atlas->lookup(face, font_size, shape.glyph, renderer);
        if (ae.bin_id < 0) continue;
        /* create polygons in vertex array */
        int x1 = segment->x + ae.ox + (int)roundf(dx + shape.x_offset/64.0f);
        int x2 = x1 + ae.w;
        int y1 = segment->y - ae.oy + (int)roundf(dy + shape.y_offset/64.0f) -
            ae.h - baseline_shift;
        int y2 = y1 + ae.h;
        if (ae.w > 0 && ae.h > 0) {
            float x1p = 0.5f + x1, x2p = 0.5f + x2;
            float y1p = 0.5f + y1, y2p = 0.5f + y2;
            float u1 = ae.uv[0], v1 = ae.uv[1];
            float u2 = ae.uv[2], v2 = ae.uv[3];
            uint o = (int)batch.vertices.size();
            uint c = segment->color;
            uint o0 = draw_list_vertex(batch, {{x1p, y1p, 0}, {u1, v1}, c});
            uint o1 = draw_list_vertex(batch, {{x2p, y1p, 0}, {u2, v1}, c});
            uint o2 = draw_list_vertex(batch, {{x2p, y2p, 0}, {u2, v2}, c});
            uint o3 = draw_list_vertex(batch, {{x1p, y2p, 0}, {u1, v2}, c});
            draw_list_add(batch, image_none, mode_triangles, shader_simple,
                {o0, o3, o1, o1, o3, o2});
        }
        /* advance */
        dx += shape.x_advance/64.0f + tracking;
        dy += shape.y_advance/64.0f;
    }
}