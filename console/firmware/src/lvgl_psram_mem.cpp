// LVGL custom memory backend (LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM).
//
// The widget tree, styles, and event lists are thousands of small
// allocations that previously went through plain malloc into internal
// DRAM — the scarce heap on this board. They're touched at render
// cadence, not pixel cadence, so PSRAM's latency is fine (the draw
// buffers, which ARE bandwidth-critical, live in display.cpp and are
// allocated separately). Internal-DRAM fallback keeps the UI alive if
// PSRAM is ever exhausted.
#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "lv_conf.h must set LV_USE_STDLIB_MALLOC to LV_STDLIB_CUSTOM"
#endif

#include <esp_heap_caps.h>

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;   // pools are a BUILTIN-backend concept
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    LV_UNUSED(pool);
}

void* lv_malloc_core(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    return p;
}

void* lv_realloc_core(void* p, size_t new_size) {
    void* np = heap_caps_realloc(p, new_size,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!np) np = heap_caps_realloc(p, new_size, MALLOC_CAP_DEFAULT);
    return np;
}

void lv_free_core(void* p) {
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) {
    LV_UNUSED(mon_p);   // heap-caps stats are served by /api/heap instead
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}
