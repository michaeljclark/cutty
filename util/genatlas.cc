#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <climits>
#include <cassert>
#include <cstring>
#include <cctype>
#include <cmath>

#include <map>
#include <vector>
#include <memory>
#include <string>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>

#include "binpack.h"
#include "image.h"
#include "draw.h"
#include "font.h"
#include "glyph.h"
#include "logger.h"
#include "file.h"
#include "utf8.h"
#include "worker.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H

#include "core/arithmetics.hpp"
#include "core/Vector2.h"
#include "core/Scanline.h"
#include "core/Shape.h"
#include "core/BitmapRef.hpp"
#include "core/Bitmap.h"
#include "core/pixel-conversion.hpp"
#include "core/edge-coloring.h"
#include "core/render-sdf.h"
#include "core/rasterization.h"
#include "core/estimate-sdf-error.h"
#include "core/save-bmp.h"
#include "core/save-tiff.h"
#include "core/shape-description.h"
#include "ext/save-png.h"
#include "ext/import-svg.h"
#include "ext/import-font.h"
#include "msdfgen.h"

using namespace std::chrono;

typedef unsigned uint;

static const char *font_path = nullptr;
static const char *output_path = nullptr;
static const char *scan_path = nullptr;
static const int dpi = 72;
static unsigned glyph_limit = 0x7f; /* 0x10ffff; */
static double range = 8;
static int glyph = 0;
static int font_size = 128;
static bool help_text = false;
static bool quiet = false;
static bool verbose = false;
static bool multithread = false;
static bool batch_render = true;
static font_manager_ft manager;

static std::string ae_dim_str(atlas_entry *ae)
{
    std::string s;
    if (ae->ox >= 0) s+= "+";
    s += std::to_string(ae->ox);
    if (ae->oy >= 0) s+= "+";
    s += std::to_string(ae->oy);
    s += " ";
    s += std::to_string(ae->x);
    s += "x";
    s += std::to_string(ae->y);
    return s;
}

struct FtContext {
    msdfgen::Shape *shape;
    msdfgen::Point2 position;
    msdfgen::Contour *contour;
};

static msdfgen::Point2 ftPoint2(const FT_Vector &vector) {
    return msdfgen::Point2(vector.x/64., vector.y/64.);
}

static int ftMoveTo(const FT_Vector *to, void *user) {
    FtContext *context = static_cast<FtContext *>(user);
    context->contour = &context->shape->addContour();
    context->position = ftPoint2(*to);
    return 0;
}

static int ftLineTo(const FT_Vector *to, void *user) {
    FtContext *context = static_cast<FtContext *>(user);
    context->contour->addEdge(new msdfgen::LinearSegment(context->position,
        ftPoint2(*to)));
    context->position = ftPoint2(*to);
    return 0;
}

static int ftConicTo(const FT_Vector *control, const FT_Vector *to, void *user) {
    FtContext *context = static_cast<FtContext *>(user);
    context->contour->addEdge(new msdfgen::QuadraticSegment(context->position,
        ftPoint2(*control), ftPoint2(*to)));
    context->position = ftPoint2(*to);
    return 0;
}

static int ftCubicTo(const FT_Vector *control1, const FT_Vector *control2,
        const FT_Vector *to, void *user) {
    FtContext *context = static_cast<FtContext *>(user);
    context->contour->addEdge(new msdfgen::CubicSegment(context->position,
        ftPoint2(*control1), ftPoint2(*control2), ftPoint2(*to)));
    context->position = ftPoint2(*to);
    return 0;
}

static atlas_entry generateMSDF(font_face *face, font_atlas *atlas,
    int size, int dpi, int glyph)
{
    msdfgen::Shape shape;
    msdfgen::Vector2 translate, scale = { 1, 1 };
    FT_Face ftface;
    FT_GlyphSlot ftglyph;
    FT_Error error;
    FT_Outline_Funcs ftFunctions;
    FtContext context = { &shape };
    atlas_entry ae;

    int char_height = size;
    int horz_resolution = dpi;
    bool overlapSupport = true;
    bool scanlinePass = true;
    double angleThreshold = 3;
    double edgeThreshold = 1.001;
    double glyphAdvance = 0;
    long long coloringSeed = 0;
    msdfgen::FillRule fillRule = msdfgen::FILL_NONZERO;

    ftface = static_cast<font_face_ft*>(face)->ftface;
    error = FT_Set_Char_Size(ftface, 0, char_height, horz_resolution,
        horz_resolution);
    if (error) {
        memset(&ae, 0, sizeof(ae));
        return ae;
    }
    error = FT_Load_Glyph(ftface, glyph, FT_LOAD_NO_HINTING);
    if (error) {
        memset(&ae, 0, sizeof(ae));
        return ae;
    }

    ftFunctions.move_to = ftMoveTo;
    ftFunctions.line_to = ftLineTo;
    ftFunctions.conic_to = ftConicTo;
    ftFunctions.cubic_to = ftCubicTo;
    ftFunctions.shift = 0;
    ftFunctions.delta = 0;

    error = FT_Outline_Decompose(&ftface->glyph->outline, &ftFunctions, &context);
    if (error) {
        memset(&ae, 0, sizeof(ae));
        return ae;
    }

    /* font dimensions */
    ftglyph = ftface->glyph;
    int ox = (int)floorf((float)ftglyph->metrics.horiBearingX / 64.0f) - 1;
    int oy = (int)floorf((float)(ftglyph->metrics.horiBearingY -
        ftglyph->metrics.height) / 64.0f) - 1;
    int w = (int)ceilf(ftglyph->metrics.width / 64.0f) + 2;
    int h = (int)ceilf(ftglyph->metrics.height / 64.0f) + 2;
    //w = rupeven(w), h = rupeven(h); /* render_block needs even rows and cols */
    translate.x = -ox;
    translate.y = -oy;

    msdfgen::Bitmap<float, 3> msdf(w, h);
    msdfgen::edgeColoringSimple(shape, angleThreshold, coloringSeed);
    msdfgen::generateMSDF(msdf, shape, range, scale, translate,
        scanlinePass ? 0 : edgeThreshold, overlapSupport);
    msdfgen::distanceSignCorrection(msdf, shape, scale, translate, fillRule);
    if (edgeThreshold > 0) {
        msdfgen::msdfErrorCorrection(msdf, edgeThreshold/(scale*range));
    }

    ae = atlas->create(face, 0, glyph, char_height, ox, oy, w, h);
    if (ae.w == w && ae.h == h) {
        for (int x = 0; x < w; x++) {
            for (int y = 0; y < h; y++) {
                int r = msdfgen::pixelFloatToByte(msdf(x,y)[0]);
                int g = msdfgen::pixelFloatToByte(msdf(x,y)[1]);
                int b = msdfgen::pixelFloatToByte(msdf(x,y)[2]);
                size_t dst = ((ae.y + y) * atlas->width + ae.x + x) * 4;
                uint32_t color = r | g << 8 | b << 16 | 0xff000000;
                *(uint32_t*)&atlas->pixels[dst] = color;
            }
        }
    }

    return ae;
}

/*
 * ftrender -render text using freetype2, and display metrics
 *
 * e.g.  ./build/bin/ftrender --font fonts/Roboto-Bold.ttf \
 *           --size 32 --text 'ABCDabcd1234' --block
 */

static void print_help(int argc, char **argv)
{
    fprintf(stderr,
        "\n"
        "Usage: %s [options] --font <path>\n"
        "\n"
        "Options:\n"
        "  -h, --help             display this help text\n"
        "  -q, --quiet            supress all output messages\n"
        "  -v, --verbose          include per glyph output messages\n"
        "  -m, --multithreaded    process multiple fonts in parallel\n"
        "  -d, --display          display glyphs (ANSI console)\n"
        "  -c, --clear            send clear before glyph (ANSI console)\n"
        "  -r, --range <float>    signed distance range (default %f)\n"
        "  -s, --size <pixels>    font size (default %d)\n"
        "  -l, --limit <integer>  render atlas glyph limit (default %d)\n"
        "  -o, --output <path>    output path (defaults to <path>.atlas)\n"
        "  -f, --font <ttf-file>  font path (mandatory)\n"
        "  -a, --scan <font-dir>  convert all fonts in directory\n",
        argv[0], range, font_size, glyph_limit);
}

static bool check_param(bool cond, const char *param)
{
    if (cond) {
        printf("error: %s requires parameter\n", param);
    }
    return (help_text = cond);
}

static bool match_opt(const char *arg, const char *opt, const char *longopt)
{
    return strcmp(arg, opt) == 0 || strcmp(arg, longopt) == 0;
}

static void parse_options(int argc, char **argv)
{
    int i = 1;
    while (i < argc) {
        if (match_opt(argv[i], "-h", "--help")) {
            help_text = true;
            i++;
        }
        else if (match_opt(argv[i], "-o","--output")) {
            if (check_param(++i == argc, "--output")) break;
            output_path = argv[i++];
        }
        else if (match_opt(argv[i], "-f","--font")) {
            if (check_param(++i == argc, "--font")) break;
            font_path = argv[i++];
        }
        else if (match_opt(argv[i], "-a","--scan")) {
            if (check_param(++i == argc, "--scan")) break;
            scan_path = argv[i++];
        }
        else if (match_opt(argv[i], "-s", "--size")) {
            if (check_param(++i == argc, "--size")) break;
            font_size = atoi(argv[i++]);
        }
        else if (match_opt(argv[i], "-r", "--range")) {
            if (check_param(++i == argc, "--range")) break;
            range = (float)atof(argv[i++]);
        }
        else if (match_opt(argv[i], "-l", "--limit")) {
            if (check_param(++i == argc, "--limit")) break;
            glyph_limit = atoi(argv[i++]);
        }
        else if (match_opt(argv[i], "-q", "--quiet")) {
            quiet = true;
            i++;
        }
        else if (match_opt(argv[i], "-v", "--verbose")) {
            verbose = true;
            i++;
        }
        else if (match_opt(argv[i], "-m", "--multithreaded")) {
            multithread = true;
            i++;
        }
        else {
            fprintf(stderr, "error: unknown option: %s\n", argv[i]);
            help_text = true;
            break;
        }
    }

    if (font_path == nullptr && scan_path == nullptr) {
        fprintf(stderr, "error: need to specify --font <path>\n");
        help_text = true;
    }

    if (help_text) {
        print_help(argc, argv);
        exit(1);
    }
}

static std::vector<std::pair<uint,uint>> allCodepointGlyphPairs(FT_Face ftface)
{
    std::vector<std::pair<uint,uint>> l;
    unsigned glyph, codepoint = FT_Get_First_Char(ftface, &glyph);
    do {
        l.push_back(std::pair<uint,uint>(codepoint,glyph));
        codepoint = FT_Get_Next_Char(ftface, codepoint, &glyph);
    } while (glyph);
    return l;
}

uint64_t process_one_file(font_face *face, const char *output_path)
{
    font_atlas atlas(font_atlas::DEFAULT_WIDTH, font_atlas::DEFAULT_HEIGHT,
        font_atlas::MSDF_DEPTH);

    const auto t1 = high_resolution_clock::now();

    /*
     * load font
     */

    FT_Face ftface = static_cast<font_face_ft*>(face)->ftface;

    /*
     * select single or multiple glyphs
     */
    std::vector<std::pair<uint,uint>> allGlyphs;
    if (glyph) {
        allGlyphs.push_back({ glyph, FT_Get_Char_Index(ftface, glyph)});
    } else {
        allGlyphs = allCodepointGlyphPairs(ftface);
    }

    /*
     * loop through chosen glyphs and create atlas
     */
    size_t area = 0, count = 0;
    for (auto pair : allGlyphs) {
        uint codepoint = pair.first, glyph = pair.second;
        atlas_entry ae;

        if (codepoint >= glyph_limit) continue;

        ae = generateMSDF(face, &atlas, font_size * 64, dpi, glyph);
        if (ae.bin_id < 0) {
            if (verbose) {
                printf("ATLAS FULL (codepoint: %u, glyph: %u)\n",
                    codepoint, glyph);
            }
            break;
        }

        if (verbose) {
            printf("[%zu/%zu] %20s (codepoint: %u, glyph: %u)\n",
                count, allGlyphs.size(), ae_dim_str(&ae).c_str(),
                codepoint, glyph);
        }

        area += ae.w * ae.h;
        count++;
    }

    if (batch_render) {
        atlas.save(&manager, face);
    }

    /*
     * atlas statistics
     */
    if (verbose) {
        printf("---\n");
        printf("font-path        : %s\n", font_path);
        printf("total-glyphs     : %zu\n", allGlyphs.size());
        printf("glyphs-processed : %zu\n", count);
        printf("total-area       : %zu (%d squared)\n", area, (int)sqrtf(area));
        printf("utilization      : %5.3f%%\n",
            100.0f*(float)area / (float)(atlas.width * atlas.height));
    }

    const auto t2 = high_resolution_clock::now();

    return duration_cast<nanoseconds>(t2 - t1).count();
}

static std::vector<std::string> sortList(std::vector<std::string> l)
{
    std::sort(l.begin(), l.end());
    return l;
}

static std::vector<std::string> endsWith(std::vector<std::string> l,
    std::string ext)
{
    std::vector<std::string> list;
    for (auto &p : l) {
        size_t i = p.find(ext);
        if (i == p.size() - ext.size()) {
            list.push_back(p);
        }
    }
    return list;
}

struct font_job
{
    font_face *face;
    std::string path;
};

struct font_worker : pool_worker<font_job>
{
    virtual void operator()(font_job &item) {
        uint64_t d = process_one_file(item.face, item.path.c_str());
        if (verbose) {
            printf("processing time  : %5.3f seconds\n---\n", (float)d/ 1e9f);
        } else if (!quiet) {
            printf("%-40s (%5.3f seconds)\n", item.path.c_str(), (float)d/ 1e9f);
        }
    }
};

int main(int argc, char **argv)
{
    parse_options(argc, argv);

    /* gather files */
    std::vector<font_job> jobs;
    if (scan_path) {
        for (auto &path : sortList(endsWith(file::list(scan_path), ".ttf"))) {
            font_face *face = manager.findFontByPath(path);
            jobs.push_back(font_job{face, path});
        }
    } else {
        font_face *face = manager.findFontByPath(font_path);
        jobs.push_back(font_job{face, output_path ? output_path : font_path});
    }

    /* process them */
    if (multithread) {
        const size_t num_threads = std::thread::hardware_concurrency();
        pool_executor<font_job,font_worker> pool(num_threads, jobs.size(), [](){
            return new font_worker();
        });
        for (auto &path : jobs) {
            pool.enqueue(path);
        }
        pool.run();
    } else {
        for (auto &path : jobs) {
            uint64_t d = process_one_file(path.face, path.path.c_str());
            if (verbose) {
                printf("processing time  : %5.3f seconds\n---\n", (float)d/ 1e9f);
            } else if (!quiet) {
                printf("%-40s (%5.3f seconds)\n", path.face->name.c_str(), (float)d/ 1e9f);
            }
        }
    }
}
