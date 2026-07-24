#include "p8p/cart.h"

#include "miniz.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define P8P_PNG_WIDTH 160u
#define P8P_PNG_HEIGHT 205u
#define P8P_PNG_DATA_SIZE 0x8020u
#define P8P_PNG_CODE_OFFSET 0x4300u
#define P8P_PNG_CODE_CAPACITY (0x8000u - P8P_PNG_CODE_OFFSET)

static uint32_t be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static uint8_t paeth(uint8_t left, uint8_t above, uint8_t upper_left) {
    int prediction = (int)left + (int)above - (int)upper_left;
    int left_distance = prediction - (int)left;
    int above_distance = prediction - (int)above;
    int diagonal_distance = prediction - (int)upper_left;
    if (left_distance < 0) left_distance = -left_distance;
    if (above_distance < 0) above_distance = -above_distance;
    if (diagonal_distance < 0) diagonal_distance = -diagonal_distance;
    if (left_distance <= above_distance && left_distance <= diagonal_distance)
        return left;
    return above_distance <= diagonal_distance ? above : upper_left;
}

static int unfilter(const uint8_t *raw, uint8_t *pixels,
                    uint32_t width, uint32_t height, uint32_t bytes_per_pixel) {
    uint32_t row_bytes = width * bytes_per_pixel;
    uint32_t raw_stride = row_bytes + 1;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *source = raw + y * raw_stride + 1;
        uint8_t *row = pixels + y * row_bytes;
        const uint8_t *previous = y ? pixels + (y - 1) * row_bytes : NULL;
        uint8_t filter = raw[y * raw_stride];

        for (uint32_t x = 0; x < row_bytes; ++x) {
            uint8_t left = x >= bytes_per_pixel ? row[x - bytes_per_pixel] : 0;
            uint8_t above = previous ? previous[x] : 0;
            uint8_t upper_left = previous && x >= bytes_per_pixel
                ? previous[x - bytes_per_pixel] : 0;
            switch (filter) {
            case 0: row[x] = source[x]; break;
            case 1: row[x] = (uint8_t)(source[x] + left); break;
            case 2: row[x] = (uint8_t)(source[x] + above); break;
            case 3:
                row[x] = (uint8_t)(source[x] +
                    (((unsigned)left + (unsigned)above) >> 1));
                break;
            case 4:
                row[x] = (uint8_t)(source[x] + paeth(left, above, upper_left));
                break;
            default:
                return -1;
            }
        }
    }
    return 0;
}

typedef struct bit_reader {
    const uint8_t *data;
    size_t size_bits;
    size_t position;
} bit_reader_t;

static uint32_t read_bits(bit_reader_t *reader, unsigned count) {
    uint32_t value = 0;
    for (unsigned i = 0; i < count && reader->position < reader->size_bits;
         ++i, ++reader->position) {
        value |= ((reader->data[reader->position >> 3] >>
                  (reader->position & 7)) & 1u) << i;
    }
    return value;
}

static int decode_legacy(const uint8_t *code, size_t capacity,
                         char **lua_out, size_t *length_out) {
    static const char lookup[] =
        "\n 0123456789abcdefghijklmnopqrstuvwxyz!#%(){}[]<>+=/*:;.,~_";
    size_t wanted = (size_t)code[4] * 256 + code[5];
    char *output = (char *)malloc(wanted + 1);
    size_t output_length = 0;

    if (!output)
        return -1;
    for (size_t i = 8; i < capacity && output_length < wanted; ++i) {
        uint8_t current = code[i];
        if (current == 0) {
            if (++i >= capacity) break;
            output[output_length++] = (char)code[i];
        } else if (current < 0x3c) {
            output[output_length++] = lookup[current - 1];
        } else {
            size_t offset;
            size_t count;
            if (++i >= capacity) break;
            offset = (size_t)(current - 0x3c) * 16 + (code[i] & 15);
            count = (code[i] >> 4) + 2;
            if (offset == 0 || offset > output_length) {
                free(output);
                return -2;
            }
            while (count-- && output_length < wanted) {
                output[output_length] = output[output_length - offset];
                ++output_length;
            }
        }
    }
    if (output_length != wanted) {
        free(output);
        return -3;
    }
    output[output_length] = '\0';
    *lua_out = output;
    *length_out = output_length;
    return 0;
}

static uint8_t mtf_get(uint8_t state[256], unsigned index) {
    uint8_t value;
    if (index > 255)
        return 0;
    value = state[index];
    memmove(state + 1, state, index);
    state[0] = value;
    return value;
}

static int decode_pxa(const uint8_t *code, size_t capacity,
                      char **lua_out, size_t *length_out) {
    size_t wanted = (size_t)code[4] * 256 + code[5];
    size_t compressed = (size_t)code[6] * 256 + code[7];
    uint8_t state[256];
    bit_reader_t bits;
    char *output;
    size_t length = 0;

    if (compressed > capacity || compressed < 8)
        return -1;
    output = (char *)malloc(wanted + 1);
    if (!output)
        return -2;
    for (unsigned i = 0; i < 256; ++i)
        state[i] = (uint8_t)i;
    bits.data = code;
    bits.size_bits = compressed * 8;
    bits.position = 64;

    while (length < wanted && bits.position < bits.size_bits) {
        if (read_bits(&bits, 1)) {
            unsigned bit_count = 4;
            unsigned index;
            while (read_bits(&bits, 1) && bit_count < 12)
                ++bit_count;
            index = read_bits(&bits, bit_count) + (1u << bit_count) - 16u;
            uint8_t value = mtf_get(state, index);
            if (!value) break;
            output[length++] = (char)value;
        } else {
            unsigned bit_count = read_bits(&bits, 1)
                ? (read_bits(&bits, 1) ? 5u : 10u) : 15u;
            size_t offset = read_bits(&bits, bit_count) + 1u;
            if (bit_count == 10 && offset == 1) {
                uint8_t value = (uint8_t)read_bits(&bits, 8);
                while (value && length < wanted && bits.position <= bits.size_bits) {
                    output[length++] = (char)value;
                    value = (uint8_t)read_bits(&bits, 8);
                }
            } else {
                unsigned part;
                size_t count = 3;
                do {
                    part = read_bits(&bits, 3);
                    count += part;
                } while (part == 7 && bits.position < bits.size_bits);
                if (offset == 0 || offset > length) {
                    free(output);
                    return -3;
                }
                while (count-- && length < wanted) {
                    output[length] = output[length - offset];
                    ++length;
                }
            }
        }
    }
    if (length != wanted) {
        free(output);
        return -4;
    }
    output[length] = '\0';
    *lua_out = output;
    *length_out = length;
    return 0;
}

static int cart_from_pico_data(const uint8_t *pico_data, p8p_cart_t *out) {
    p8p_cart_t cart;
    const uint8_t *code = pico_data + P8P_PNG_CODE_OFFSET;
    size_t length = 0;

    memset(&cart, 0, sizeof(cart));
    memcpy(cart.rom, pico_data, sizeof(cart.rom));
    if (code[0] == 0 && code[1] == 'p' && code[2] == 'x' && code[3] == 'a') {
        if (decode_pxa(code, P8P_PNG_CODE_CAPACITY, &cart.lua, &cart.lua_size) != 0)
            return -1;
    } else if (code[0] == ':' && code[1] == 'c' && code[2] == ':' && code[3] == 0) {
        if (decode_legacy(code, P8P_PNG_CODE_CAPACITY, &cart.lua,
                          &cart.lua_size) != 0)
            return -2;
    } else {
        while (length < P8P_PNG_CODE_CAPACITY && code[length])
            ++length;
        cart.lua = (char *)malloc(length + 1);
        if (!cart.lua)
            return -3;
        memcpy(cart.lua, code, length);
        cart.lua[length] = '\0';
        cart.lua_size = length;
    }
    if (!cart.lua_size) {
        p8p_cart_destroy(&cart);
        return -4;
    }
    *out = cart;
    return 0;
}

int p8p_cart_load_png_memory(const uint8_t *data, size_t size,
                             p8p_cart_t *out) {
    static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t color_type;
    uint8_t interlace;
    uint32_t bytes_per_pixel;
    size_t total_idat = 0;
    uint8_t *compressed = NULL;
    uint8_t *raw = NULL;
    uint8_t *pixels = NULL;
    uint8_t *pico_data = NULL;
    size_t compressed_position = 0;
    int result = -1;

    if (!data || !out || size < 33 || memcmp(data, signature, 8) != 0)
        return -1;
    if (be32(data + 8) != 13 || memcmp(data + 12, "IHDR", 4) != 0)
        return -2;
    width = be32(data + 16);
    height = be32(data + 20);
    bit_depth = data[24];
    color_type = data[25];
    interlace = data[28];
    if (width != P8P_PNG_WIDTH || height != P8P_PNG_HEIGHT ||
        bit_depth != 8 || (color_type != 2 && color_type != 6) || interlace != 0)
        return -3;
    bytes_per_pixel = color_type == 6 ? 4 : 3;

    for (size_t position = 8; position + 12 <= size; ) {
        uint32_t chunk_length = be32(data + position);
        if ((size_t)chunk_length > size - position - 12)
            return -4;
        if (memcmp(data + position + 4, "IDAT", 4) == 0)
            total_idat += chunk_length;
        position += 12 + chunk_length;
    }
    if (!total_idat)
        return -5;
    compressed = (uint8_t *)malloc(total_idat);
    raw = (uint8_t *)malloc((width * bytes_per_pixel + 1) * height);
    pixels = (uint8_t *)malloc(width * height * bytes_per_pixel);
    pico_data = (uint8_t *)malloc(P8P_PNG_DATA_SIZE);
    if (!compressed || !raw || !pixels || !pico_data) {
        result = -6;
        goto cleanup;
    }
    for (size_t position = 8; position + 12 <= size; ) {
        uint32_t chunk_length = be32(data + position);
        if (memcmp(data + position + 4, "IDAT", 4) == 0) {
            memcpy(compressed + compressed_position, data + position + 8,
                   chunk_length);
            compressed_position += chunk_length;
        }
        position += 12 + chunk_length;
    }
    {
        mz_ulong raw_length = (mz_ulong)((width * bytes_per_pixel + 1) * height);
        if (mz_uncompress(raw, &raw_length, compressed, (mz_ulong)total_idat) != MZ_OK ||
            raw_length != (mz_ulong)((width * bytes_per_pixel + 1) * height)) {
            result = -7;
            goto cleanup;
        }
    }
    if (unfilter(raw, pixels, width, height, bytes_per_pixel) != 0) {
        result = -8;
        goto cleanup;
    }
    for (size_t i = 0; i < P8P_PNG_DATA_SIZE; ++i) {
        const uint8_t *pixel = pixels + i * bytes_per_pixel;
        uint8_t alpha = bytes_per_pixel == 4 ? pixel[3] : 255;
        pico_data[i] = (uint8_t)(((alpha & 3) << 6) | ((pixel[0] & 3) << 4) |
                                 ((pixel[1] & 3) << 2) | (pixel[2] & 3));
    }
    result = cart_from_pico_data(pico_data, out);

cleanup:
    free(compressed);
    free(raw);
    free(pixels);
    free(pico_data);
    return result;
}
