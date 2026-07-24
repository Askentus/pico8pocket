#ifndef P8P_DISPLAY_H
#define P8P_DISPLAY_H

#include <stdint.h>

typedef struct p8p_viewport {
    int scale;
    int width;
    int height;
    int x;
    int y;
} p8p_viewport_t;

typedef struct p8p_resample_point {
    uint16_t first;
    uint16_t second;
    uint8_t weight;
} p8p_resample_point_t;

int p8p_integer_viewport(int output_width, int output_height,
                         int source_width, int source_height,
                         p8p_viewport_t *out);
int p8p_sharp_bilinear_axis(int source_size, int integer_scale,
                            int output_size, p8p_resample_point_t *out);
uint16_t p8p_rgb565_blend(uint16_t first, uint16_t second,
                          unsigned weight);

#endif
