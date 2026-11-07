#pragma once

#include <esp_heap_caps.h>

#define OVERRIDE_SPEEX_ALLOC

static inline void* speex_alloc(int size) {
    return heap_caps_calloc(size, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
