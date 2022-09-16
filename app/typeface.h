#pragma once

typedef unsigned int uint;

extern const char *mono1_emoji_font_path;
extern const char *mono1_regular_font_path;
extern const char *mono1_bold_font_path;
extern const char *mono1_condensed_regular_font_path;
extern const char *mono1_condensed_bold_font_path;

font_face* tty_typeface_get_font(font_manager_ft *manager, tty_cellgrid_face face);
uint tty_typeface_lookup_glyph(font_face *face, uint codepoint);
tty_font_metric tty_typeface_get_metrics(font_face *face, float font_size, int codepoint);
void tty_typeface_print_metrics(font_face *face, tty_font_metric m);
