"""Pre-build patch: neutralise LVGL 9's Cortex-M Helium assembly on Xtensa.

LVGL 9.2+ ships `lv_blend_helium.S` / `lv_blend_neon.S` which are gated in C
but not in the assembler — they still try to pull in <stdint.h> which is
invalid in an assembly context on Xtensa. The result is a flood of "unknown
opcode 'typedef'" errors. We replace both files with an empty stub the first
time they appear in `.pio/libdeps/`.
"""
Import("env")
import os

def patch_lvgl(*_):
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV/lvgl")
    if not os.path.isdir(libdeps_dir):
        return

    targets = [
        os.path.join(libdeps_dir, "src", "draw", "sw", "blend", "helium", "lv_blend_helium.S"),
        os.path.join(libdeps_dir, "src", "draw", "sw", "blend", "neon",    "lv_blend_neon.S"),
        # Older layout (LVGL may ship without src/)
        os.path.join(libdeps_dir, "draw", "sw", "blend", "helium", "lv_blend_helium.S"),
        os.path.join(libdeps_dir, "draw", "sw", "blend", "neon",    "lv_blend_neon.S"),
    ]
    for t in targets:
        if os.path.isfile(t):
            with open(t, "w") as f:
                f.write("/* Neutralised by patch_lvgl.py — not applicable on Xtensa */\n")
            print(f"[patch_lvgl] cleared {os.path.relpath(t, libdeps_dir)}")

patch_lvgl()
