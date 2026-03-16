#include "esp_heap_caps.h"
#include "lvgl.h"

extern "C" {

void lv_mem_init(void)
{
    /* Nothing to init — using ESP-IDF heap allocator */
}

void lv_mem_deinit(void)
{
    /* Nothing to deinit */
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    lv_memzero(mon_p, sizeof(*mon_p));
    mon_p->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->free_size = info.total_free_bytes;
    mon_p->used_pct = mon_p->total_size
        ? (100 * info.total_allocated_bytes / mon_p->total_size)
        : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

} // extern "C"
