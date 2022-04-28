#pragma once

typedef unsigned int uint;

void tty_typeface_init(tty_cellgrid *cg);
uint tty_typeface_lookup_glyph(font_face *face, uint codepoint);
tty_fontmetric tty_typeface_get_metrics(font_face *face, float font_size, int codepoint);
void tty_typeface_print_metrics(font_face *face, tty_fontmetric m);
