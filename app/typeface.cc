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
#include "canvas.h"
#include "color.h"
#include "logger.h"
#include "file.h"
#include "ui9.h"

#include "teletype.h"
#include "cellgrid.h"
#include "typeface.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H

const char *mono1_emoji_font_path = "fonts/NotoColorEmoji.ttf";
const char *mono1_regular_font_path = "fonts/NotoSansMono-Regular.ttf";
const char *mono1_bold_font_path = "fonts/NotoSansMono-Bold.ttf";

uint tty_typeface_lookup_glyph(font_face *face, uint codepoint)
{
    font_face_ft *fft = static_cast<font_face_ft*>(face);
    return FT_Get_Char_Index(fft->ftface, codepoint);
}

tty_font_metric tty_typeface_get_metrics(font_face *face, float font_size, int codepoint)
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

    tty_font_metric m = {
        font_size, advance, leading, height,
        ascender, descender, underline_position, underline_thickness
    };

    return m;
}

void tty_typeface_print_metrics(font_face *face, tty_font_metric m)
{
    Debug("face=%s size=%f advance=%f leading=%f\n",
        face->name.c_str(), m.size, m.advance, m.leading);
    Debug("\theight=%f ascender=%f descender=%f\n",
        m.height, m.ascender, m.descender);
    Debug("\tunderline_position=%f underline_thickness=%f\n",
        m.underline_position, m.underline_thickness);
}
