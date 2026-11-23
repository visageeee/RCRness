/* Compare cached and full CRT output after scrolling, edits and resizes. */
#include "crt_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_frames(CrtFilterCache *cache, int width, int height, int display_width) {
    size_t bytes = (size_t)display_width * height * 9 * sizeof(uint32_t);
    uint32_t *src = malloc((size_t)width * height * sizeof *src);
    uint32_t *cached = malloc(bytes), *full = malloc(bytes), *scratch = malloc(bytes);
    if (!src || !cached || !full || !scratch) abort();

    static const int pan[] = {0, 0, 1, 2, 6, 4, 3, 2, 2};
    int ok = 1;
    for (unsigned frame = 0; frame < sizeof pan / sizeof pan[0]; frame++) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                unsigned world_x = (x + pan[frame]) % width;
                src[y * width + x] = 0xff000000u | ((world_x * 71963u + y * 4129u) & 0xffffff);
            }
        }
        /* Changes on either side of a block boundary must invalidate its halo. */
        if (width > 17 && height > 17)
            src[(15 + frame % 3) * width + 16] ^= 0x00ffffff;
        crt_filter_apply_3x(src, width, height, display_width, full, scratch);
        crt_filter_apply_cached(cache, src, width, height, display_width, cached, scratch);
        if (memcmp(full, cached, bytes)) {
            fprintf(stderr, "CRT mismatch at %dx%d -> %d, frame %u\n", width, height, display_width, frame);
            ok = 0;
            break;
        }
    }
    free(src);
    free(cached);
    free(full);
    free(scratch);
    return ok;
}

int main(void) {
    CrtFilterCache *cache = crt_filter_cache_create();
    if (!cache) return 1;
    int ok = compare_frames(cache, 426, 240, 426) &&
             compare_frames(cache, 256, 240, 320) &&
             compare_frames(cache, 33, 17, 33) &&
             compare_frames(cache, 1, 1, 1);
    crt_filter_cache_destroy(cache);
    return ok ? 0 : 1;
}
