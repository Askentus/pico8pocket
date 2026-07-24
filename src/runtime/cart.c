#include "p8p/cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t png_signature[8] = {
    0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
};

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static int has_text_header(const uint8_t *data, size_t size) {
    static const char header[] = "pico-8 cartridge // http://www.pico-8.com";
    size_t offset = 0;

    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
        offset = 3;

    return size - offset >= sizeof(header) - 1 &&
           memcmp(data + offset, header, sizeof(header) - 1) == 0;
}

int p8p_cart_probe_memory(const uint8_t *data, size_t size,
                          uint64_t file_size, p8p_cart_info_t *out) {
    p8p_cart_info_t info;

    if (!data || !out)
        return -1;

    memset(&info, 0, sizeof(info));
    info.file_size = file_size;

    if (has_text_header(data, size)) {
        info.kind = P8P_CART_TEXT;
        info.valid = 1;
    } else if (size >= 24 && memcmp(data, png_signature, sizeof(png_signature)) == 0 &&
               memcmp(data + 12, "IHDR", 4) == 0) {
        info.kind = P8P_CART_PNG;
        info.png_width = read_be32(data + 16);
        info.png_height = read_be32(data + 20);
        info.valid = info.png_width == 160 && info.png_height == 205;
    } else if (size >= 4 && data[0] == 'P' && data[1] == 'K' &&
               ((data[2] == 3 && data[3] == 4) ||
                (data[2] == 5 && data[3] == 6) ||
                (data[2] == 7 && data[3] == 8))) {
        info.kind = P8P_CART_ZIP;
        info.valid = 1;
    }

    *out = info;
    return 0;
}

int p8p_cart_probe_file(const char *path, p8p_cart_info_t *out) {
    uint8_t prefix[128];
    long length;
    size_t read_count;
    FILE *file;

    if (!path || !out)
        return -1;

    file = fopen(path, "rb");
    if (!file)
        return -2;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -3;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -3;
    }

    read_count = fread(prefix, 1, sizeof(prefix), file);
    fclose(file);
    return p8p_cart_probe_memory(prefix, read_count, (uint64_t)length, out);
}

const char *p8p_cart_kind_name(p8p_cart_kind_t kind) {
    switch (kind) {
    case P8P_CART_TEXT: return "p8";
    case P8P_CART_PNG: return "p8.png";
    case P8P_CART_ZIP: return "zip";
    default: return "unknown";
    }
}

typedef enum p8p_section {
    P8P_SECTION_NONE = 0,
    P8P_SECTION_LUA,
    P8P_SECTION_GFX,
    P8P_SECTION_GFF,
    P8P_SECTION_MAP,
    P8P_SECTION_SFX,
    P8P_SECTION_MUSIC
} p8p_section_t;

static int hex_nibble(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_byte(const uint8_t *p) {
    int high = hex_nibble(p[0]);
    int low = hex_nibble(p[1]);
    return high < 0 || low < 0 ? -1 : (high << 4) | low;
}

static p8p_section_t section_from_line(const uint8_t *line, size_t length) {
    struct section_name {
        const char *name;
        p8p_section_t section;
    };
    static const struct section_name names[] = {
        {"__lua__", P8P_SECTION_LUA},
        {"__gfx__", P8P_SECTION_GFX},
        {"__gff__", P8P_SECTION_GFF},
        {"__map__", P8P_SECTION_MAP},
        {"__sfx__", P8P_SECTION_SFX},
        {"__music__", P8P_SECTION_MUSIC}
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        size_t name_length = strlen(names[i].name);
        if (length == name_length && memcmp(line, names[i].name, length) == 0)
            return names[i].section;
    }
    return P8P_SECTION_NONE;
}

static void parse_hex_stream(const uint8_t *line, size_t length,
                             uint8_t *destination, size_t capacity,
                             size_t *position, int swap_nibbles) {
    int first = -1;

    for (size_t i = 0; i < length && *position < capacity; ++i) {
        int nibble = hex_nibble(line[i]);
        if (nibble < 0)
            continue;
        if (first < 0) {
            first = nibble;
        } else {
            destination[(*position)++] = (uint8_t)(swap_nibbles
                ? first | (nibble << 4)
                : (first << 4) | nibble);
            first = -1;
        }
    }
}

static void parse_music_line(const uint8_t *line, size_t length,
                             uint8_t *rom, size_t index) {
    int flags;
    int channels[4];

    if (index >= 64 || length < 11)
        return;
    flags = hex_byte(line);
    for (int i = 0; i < 4; ++i)
        channels[i] = hex_byte(line + 3 + i * 2);
    if (flags < 0 || channels[0] < 0 || channels[1] < 0 ||
        channels[2] < 0 || channels[3] < 0)
        return;

    rom[0x3100 + index * 4 + 0] = (uint8_t)(channels[0] | ((flags & 1) << 7));
    rom[0x3100 + index * 4 + 1] = (uint8_t)(channels[1] | ((flags & 2) << 6));
    rom[0x3100 + index * 4 + 2] = (uint8_t)(channels[2] | ((flags & 4) << 5));
    rom[0x3100 + index * 4 + 3] = (uint8_t)(channels[3] | ((flags & 8) << 4));
}

static void parse_sfx_line(const uint8_t *line, size_t length,
                           uint8_t *rom, size_t index) {
    uint8_t *sfx;

    if (index >= 64 || length < 168)
        return;
    sfx = rom + 0x3200 + index * 68;
    for (int note = 0; note < 32; ++note) {
        size_t offset = 8 + (size_t)note * 5;
        int key = hex_byte(line + offset);
        int waveform = hex_nibble(line[offset + 2]);
        int volume = hex_nibble(line[offset + 3]);
        int effect = hex_nibble(line[offset + 4]);
        if (key < 0 || waveform < 0 || volume < 0 || effect < 0)
            continue;
        sfx[note * 2] = (uint8_t)((key & 0x3f) | ((waveform & 3) << 6));
        sfx[note * 2 + 1] = (uint8_t)(((waveform >> 2) & 1) |
            ((volume & 7) << 1) | ((effect & 7) << 4) |
            ((waveform > 7 ? 1 : 0) << 7));
    }
    sfx[64] = (uint8_t)hex_byte(line);
    sfx[65] = (uint8_t)hex_byte(line + 2);
    sfx[66] = (uint8_t)hex_byte(line + 4);
    sfx[67] = (uint8_t)hex_byte(line + 6);
}

int p8p_cart_load_text_memory(const uint8_t *data, size_t size,
                              p8p_cart_t *out) {
    p8p_cart_t cart;
    p8p_section_t section = P8P_SECTION_NONE;
    size_t cursor = 0;
    size_t lua_capacity;
    size_t gfx_position = 0;
    size_t gff_position = 0;
    size_t map_position = 0;
    size_t music_line = 0;
    size_t sfx_line = 0;

    if (!data || !out || !has_text_header(data, size))
        return -1;
    memset(&cart, 0, sizeof(cart));
    for (size_t i = 0; i < 64; ++i)
        cart.rom[0x3200 + i * 68 + 65] = 16;
    lua_capacity = size + 1;
    cart.lua = (char *)malloc(lua_capacity);
    if (!cart.lua)
        return -2;
    cart.lua[0] = '\0';

    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
        cursor = 3;

    while (cursor < size) {
        size_t line_start = cursor;
        size_t line_length;
        p8p_section_t next_section;

        while (cursor < size && data[cursor] != '\n')
            ++cursor;
        line_length = cursor - line_start;
        if (line_length && data[line_start + line_length - 1] == '\r')
            --line_length;
        if (cursor < size)
            ++cursor;

        next_section = section_from_line(data + line_start, line_length);
        if (next_section != P8P_SECTION_NONE) {
            section = next_section;
            continue;
        }

        switch (section) {
        case P8P_SECTION_LUA:
            if (cart.lua_size + line_length + 2 <= lua_capacity) {
                memcpy(cart.lua + cart.lua_size, data + line_start, line_length);
                cart.lua_size += line_length;
                cart.lua[cart.lua_size++] = '\n';
                cart.lua[cart.lua_size] = '\0';
            }
            break;
        case P8P_SECTION_GFX:
            parse_hex_stream(data + line_start, line_length, cart.rom,
                             0x2000, &gfx_position, 1);
            break;
        case P8P_SECTION_GFF:
            parse_hex_stream(data + line_start, line_length, cart.rom + 0x3000,
                             0x100, &gff_position, 0);
            break;
        case P8P_SECTION_MAP:
            parse_hex_stream(data + line_start, line_length, cart.rom + 0x2000,
                             0x1000, &map_position, 0);
            break;
        case P8P_SECTION_MUSIC:
            parse_music_line(data + line_start, line_length, cart.rom, music_line++);
            break;
        case P8P_SECTION_SFX:
            parse_sfx_line(data + line_start, line_length, cart.rom, sfx_line++);
            break;
        default:
            break;
        }
    }

    if (cart.lua_size == 0) {
        free(cart.lua);
        return -3;
    }
    *out = cart;
    return 0;
}

int p8p_cart_load_text_file(const char *path, p8p_cart_t *out) {
    FILE *file;
    uint8_t *data;
    long length;
    int result;

    if (!path || !out)
        return -1;
    file = fopen(path, "rb");
    if (!file)
        return -2;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -3;
    }
    data = (uint8_t *)malloc((size_t)length);
    if (!data) {
        fclose(file);
        return -4;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return -5;
    }
    fclose(file);
    result = p8p_cart_load_text_memory(data, (size_t)length, out);
    free(data);
    return result;
}

int p8p_cart_load_file(const char *path, p8p_cart_t *out) {
    FILE *file;
    uint8_t *data;
    long length;
    p8p_cart_info_t info;
    int result;

    if (!path || !out)
        return -1;
    file = fopen(path, "rb");
    if (!file)
        return -2;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -3;
    }
    data = (uint8_t *)malloc((size_t)length);
    if (!data) {
        fclose(file);
        return -4;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return -5;
    }
    fclose(file);
    if (p8p_cart_probe_memory(data, (size_t)length, (uint64_t)length, &info) != 0) {
        free(data);
        return -6;
    }
    if (info.kind == P8P_CART_TEXT)
        result = p8p_cart_load_text_memory(data, (size_t)length, out);
    else if (info.kind == P8P_CART_PNG)
        result = p8p_cart_load_png_memory(data, (size_t)length, out);
    else
        result = -7;
    free(data);
    return result;
}

void p8p_cart_destroy(p8p_cart_t *cart) {
    if (!cart)
        return;
    free(cart->lua);
    memset(cart, 0, sizeof(*cart));
}
