#include "mbedtls_psram_alloc.h"

#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

static void* _psram_calloc(size_t n, size_t size) {
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_DEFAULT);
    return p;
}

static void _psram_free(void* p) {
    heap_caps_free(p);
}

void mbedtls_install_psram_alloc() {
    static bool installed = false;
    if (installed) return;
    mbedtls_platform_set_calloc_free(_psram_calloc, _psram_free);
    installed = true;
}
