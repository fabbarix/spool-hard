#pragma once
#include <lvgl.h>

// SpoolHard's own Montserrat fonts, regenerated via
// `console/scripts/build_fonts.sh`. They replace LVGL's built-in
// `lv_font_montserrat_*` (disabled in lv_conf.h) so we can bake in extra
// general-punctuation glyphs — em-dash, en-dash, ellipsis — plus a curated
// FontAwesome subset. Adding a new glyph is a one-liner in the script.
extern const lv_font_t spoolhard_mont_14;
extern const lv_font_t spoolhard_mont_16;
extern const lv_font_t spoolhard_mont_18;
extern const lv_font_t spoolhard_mont_22;
extern const lv_font_t spoolhard_mont_28;
extern const lv_font_t spoolhard_mont_36;
