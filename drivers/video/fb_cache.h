#ifndef FB_CACHE_H
#define FB_CACHE_H

#include <stdint.h>

int fb_cache_enable_write_combining(uintptr_t fb_addr, uint32_t fb_bytes);

#endif
