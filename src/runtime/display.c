#include "p8p/display.h"

int p8p_integer_viewport(int output_width, int output_height,
                         int source_width, int source_height,
                         p8p_viewport_t *out) {
    int scale_x;
    int scale_y;
    int scale;

    if (!out || output_width <= 0 || output_height <= 0 ||
        source_width <= 0 || source_height <= 0)
        return -1;

    scale_x = output_width / source_width;
    scale_y = output_height / source_height;
    scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale < 1)
        return -2;

    out->scale = scale;
    out->width = source_width * scale;
    out->height = source_height * scale;
    out->x = (output_width - out->width) / 2;
    out->y = (output_height - out->height) / 2;
    return 0;
}

int p8p_sharp_bilinear_axis(int source_size, int integer_scale,
                            int output_size, p8p_resample_point_t *out) {
    int virtual_size;

    if (!out || source_size <= 0 || integer_scale <= 0 || output_size <= 0)
        return -1;
    if (source_size > 65535 / integer_scale)
        return -1;
    virtual_size = source_size * integer_scale;

    for (int output = 0; output < output_size; ++output) {
        int64_t position =
            ((int64_t)(output * 2 + 1) * virtual_size * 32) /
            (output_size * 2) - 16;
        int virtual_first;

        if (position < 0)
            position = 0;
        if (position > (int64_t)(virtual_size - 1) * 32)
            position = (int64_t)(virtual_size - 1) * 32;
        virtual_first = (int)(position / 32);
        out[output].first = (uint16_t)(virtual_first / integer_scale);
        out[output].second = (uint16_t)(
            (virtual_first + (virtual_first + 1 < virtual_size)) /
            integer_scale);
        out[output].weight = (uint8_t)(position % 32);
    }
    return 0;
}

uint16_t p8p_rgb565_blend(uint16_t first, uint16_t second,
                          unsigned weight) {
    uint32_t inverse;
    uint32_t red_blue;
    uint32_t green;

    if (weight > 32)
        weight = 32;
    inverse = 32 - weight;
    red_blue = (((uint32_t)(first & 0xf81fu) * inverse +
                 (uint32_t)(second & 0xf81fu) * weight) >> 5) & 0xf81fu;
    green = (((uint32_t)(first & 0x07e0u) * inverse +
              (uint32_t)(second & 0x07e0u) * weight) >> 5) & 0x07e0u;
    return (uint16_t)(red_blue | green);
}
