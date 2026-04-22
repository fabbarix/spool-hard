"""Pre-build patch: clean up two upstream-library issues that bite us.

1. LVGL 9.2+ ships `lv_blend_helium.S` / `lv_blend_neon.S` — Cortex-M
   intrinsics gated in C but not in the assembler — and they pull in
   <stdint.h>, which on Xtensa produces a flood of "unknown opcode
   'typedef'" errors. Stub both files.

2. LovyanGFX 1.3.x ships a vendored copy of LVGL's lv_font headers
   (`lgfx/v1/lv_font/font_fmt_txt.h` etc.) intended as a fallback when
   LVGL isn't installed. We DO have LVGL installed, so the duplicate
   enum + typedef declarations collide. platformio.ini pins LovyanGFX
   to 1.2.19 (which doesn't have these); this script strips them
   defensively in case a future pin bump re-introduces the conflict.

Both fix the build-side cleanup at the same point — pre-action, before
scons sees the headers.
"""
Import("env")
import os

def _stub(path, label, banner):
    if os.path.isfile(path):
        with open(path, "w") as f:
            f.write(f"/* Neutralised by patch_lvgl.py — {banner} */\n")
        print(f"[patch_lvgl] cleared {label}")

def patch_lvgl(*_):
    # ── 1. LVGL Helium/Neon assembly stubs ──
    lvgl_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV/lvgl")
    if os.path.isdir(lvgl_dir):
        for rel in (
            "src/draw/sw/blend/helium/lv_blend_helium.S",
            "src/draw/sw/blend/neon/lv_blend_neon.S",
            # Older layout (LVGL may ship without src/)
            "draw/sw/blend/helium/lv_blend_helium.S",
            "draw/sw/blend/neon/lv_blend_neon.S",
        ):
            _stub(os.path.join(lvgl_dir, rel), f"lvgl/{rel}",
                  "not applicable on Xtensa")

    # ── 2. LovyanGFX vendored LVGL adapter ──
    # When present, its lv_font/*.h declares the same enums/typedefs as
    # the real LVGL, breaking any TU that includes both <lvgl.h> and
    # <LovyanGFX.hpp>. We always have real LVGL — the vendored copy is
    # never the right thing here.
    lgfx_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV/LovyanGFX")
    lgfx_lvfont = os.path.join(lgfx_dir, "src", "lgfx", "v1", "lv_font")
    if os.path.isdir(lgfx_lvfont):
        for f in os.listdir(lgfx_lvfont):
            if f.endswith(".h") or f.endswith(".hpp"):
                _stub(os.path.join(lgfx_lvfont, f),
                      f"LovyanGFX/src/lgfx/v1/lv_font/{f}",
                      "duplicate of LVGL upstream — use real lvgl instead")

patch_lvgl()
