/**
 * LVGL 9 configuration for SpoolHard Console.
 *
 * Only non-default settings are called out; everything else uses the built-in
 * defaults from lv_conf_internal.h. See:
 *   https://docs.lvgl.io/master/intro/add-library/configuration.html
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*=====================
 * Color settings
 *=====================*/
#define LV_COLOR_DEPTH     16
// LVGL 9 ignores LV_COLOR_16_SWAP; it always emits RGB565 in host byte order.
// The byte swap for the panel happens in display.cpp's flush_cb via
// pushPixels(..., swap=true). Keeping this 0 to avoid confusion.
#define LV_COLOR_16_SWAP   0

/*=========================
 * Memory settings
 *=========================*/
/* Let LVGL allocate from PSRAM via a custom allocator registered at boot. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB
#define LV_MEM_SIZE             (128 * 1024)  /* 128 kB internal pool */

/*====================
 * HAL settings
 *====================*/
#define LV_TICK_CUSTOM              1
#define LV_TICK_CUSTOM_INCLUDE      "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DISP_DEF_REFR_PERIOD     16
#define LV_INDEV_DEF_READ_PERIOD    16

/*====================
 * Operating system
 *====================*/
#define LV_USE_OS               LV_OS_FREERTOS

/*=================
 * Feature flags
 *=================*/
#define LV_USE_LOG              1
#define LV_LOG_LEVEL            LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF           1

#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

#define LV_USE_PERF_MONITOR     0
#define LV_USE_MEM_MONITOR      0

/*=================
 * Themes
 *=================*/
#define LV_USE_THEME_DEFAULT    1
#define LV_THEME_DEFAULT_DARK   1

/*=================
 * Widgets
 *=================*/
/* Leave widgets at defaults (all enabled). Disable if binary-size becomes an issue. */

/*=================
 * Fonts
 *=================*/
/* We ship our own Montserrat builds (console/firmware/src/fonts/) with
   extra general-punctuation glyphs baked in — see spoolhard_fonts.h and
   console/scripts/build_fonts.sh. LVGL's defaults are disabled to avoid
   duplicating ~350 KB of font bitmaps in flash. */
#define LV_FONT_MONTSERRAT_14   0
#define LV_FONT_MONTSERRAT_16   0
#define LV_FONT_MONTSERRAT_18   0
#define LV_FONT_MONTSERRAT_22   0
#define LV_FONT_MONTSERRAT_28   0
#define LV_FONT_MONTSERRAT_36   0

/* Declare our custom fonts in every translation unit that includes lvgl.h,
   so LVGL internals (themes, default label font) can reference them too. */
#define LV_FONT_CUSTOM_DECLARE \
    LV_FONT_DECLARE(spoolhard_mont_14) \
    LV_FONT_DECLARE(spoolhard_mont_16) \
    LV_FONT_DECLARE(spoolhard_mont_18) \
    LV_FONT_DECLARE(spoolhard_mont_22) \
    LV_FONT_DECLARE(spoolhard_mont_28) \
    LV_FONT_DECLARE(spoolhard_mont_36)

/* Theme + default label font point at our 14px build instead of LVGL's. */
#define LV_FONT_DEFAULT &spoolhard_mont_14

/*=====================
 * File system
 *=====================*/
#define LV_USE_FS_LITTLEFS      0   /* we access LittleFS directly, not via LVGL */

#endif /* LV_CONF_H */
