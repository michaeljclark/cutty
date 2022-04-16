#pragma once

typedef unsigned int uint;

void cu_typeface_init(cu_cellgrid *cg);
uint cu_typeface_lookup_glyph(font_face *face, uint codepoint);
cu_font_metric cu_typeface_get_metrics(font_face *face, float font_size, int codepoint);
void cu_typeface_print_metrics(font_face *face, cu_font_metric m);
