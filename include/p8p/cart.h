#ifndef P8P_CART_H
#define P8P_CART_H

#include <stddef.h>
#include <stdint.h>

typedef enum p8p_cart_kind {
    P8P_CART_UNKNOWN = 0,
    P8P_CART_TEXT,
    P8P_CART_PNG,
    P8P_CART_ZIP
} p8p_cart_kind_t;

typedef struct p8p_cart_info {
    p8p_cart_kind_t kind;
    uint64_t file_size;
    uint32_t png_width;
    uint32_t png_height;
    int valid;
} p8p_cart_info_t;

#define P8P_CART_ROM_SIZE 0x4300u

typedef struct p8p_cart {
    uint8_t rom[P8P_CART_ROM_SIZE];
    char *lua;
    size_t lua_size;
} p8p_cart_t;

int p8p_cart_probe_memory(const uint8_t *data, size_t size,
                          uint64_t file_size, p8p_cart_info_t *out);
int p8p_cart_probe_file(const char *path, p8p_cart_info_t *out);
const char *p8p_cart_kind_name(p8p_cart_kind_t kind);
int p8p_cart_load_text_memory(const uint8_t *data, size_t size,
                              p8p_cart_t *out);
int p8p_cart_load_text_file(const char *path, p8p_cart_t *out);
int p8p_cart_load_png_memory(const uint8_t *data, size_t size,
                             p8p_cart_t *out);
int p8p_cart_load_file(const char *path, p8p_cart_t *out);
void p8p_cart_destroy(p8p_cart_t *cart);

#endif
