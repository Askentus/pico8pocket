#include "p8p/cart.h"
#include "p8p/display.h"
#include "p8p/platform.h"
#include "p8p/scheduler.h"
#include "p8p/system_input.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ++failures; \
    } \
} while (0)

static void write_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void test_text_cart(void) {
    static const uint8_t cart[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n";
    p8p_cart_info_t info;

    CHECK(p8p_cart_probe_memory(cart, sizeof(cart) - 1, sizeof(cart) - 1, &info) == 0);
    CHECK(info.kind == P8P_CART_TEXT);
    CHECK(info.valid == 1);
}

static void test_text_cart_with_bom(void) {
    static const uint8_t cart[] =
        "\xef\xbb\xbfpico-8 cartridge // http://www.pico-8.com\nversion 42\n";
    p8p_cart_info_t info;

    CHECK(p8p_cart_probe_memory(cart, sizeof(cart) - 1, sizeof(cart) - 1, &info) == 0);
    CHECK(info.kind == P8P_CART_TEXT);
    CHECK(info.valid == 1);
}

static void make_png_header(uint8_t png[24], uint32_t width, uint32_t height) {
    static const uint8_t prefix[16] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R'
    };
    memcpy(png, prefix, sizeof(prefix));
    write_be32(png + 16, width);
    write_be32(png + 20, height);
}

static void test_png_cart(void) {
    uint8_t png[24];
    p8p_cart_info_t info;

    make_png_header(png, 160, 205);
    CHECK(p8p_cart_probe_memory(png, sizeof(png), 12345, &info) == 0);
    CHECK(info.kind == P8P_CART_PNG);
    CHECK(info.png_width == 160);
    CHECK(info.png_height == 205);
    CHECK(info.file_size == 12345);
    CHECK(info.valid == 1);

    make_png_header(png, 128, 128);
    CHECK(p8p_cart_probe_memory(png, sizeof(png), sizeof(png), &info) == 0);
    CHECK(info.kind == P8P_CART_PNG);
    CHECK(info.valid == 0);
}

static void test_zip_and_unknown(void) {
    static const uint8_t zip[] = {'P', 'K', 3, 4, 0, 0, 0, 0};
    static const uint8_t junk[] = {1, 2, 3, 4};
    p8p_cart_info_t info;

    CHECK(p8p_cart_probe_memory(zip, sizeof(zip), sizeof(zip), &info) == 0);
    CHECK(info.kind == P8P_CART_ZIP);
    CHECK(info.valid == 1);

    CHECK(p8p_cart_probe_memory(junk, sizeof(junk), sizeof(junk), &info) == 0);
    CHECK(info.kind == P8P_CART_UNKNOWN);
    CHECK(info.valid == 0);
}

static void test_integer_scaling(void) {
    p8p_viewport_t viewport;

    CHECK(p8p_integer_viewport(1600, 1440, 128, 128, &viewport) == 0);
    CHECK(viewport.scale == 11);
    CHECK(viewport.width == 1408);
    CHECK(viewport.height == 1408);
    CHECK(viewport.x == 96);
    CHECK(viewport.y == 16);

    CHECK(p8p_integer_viewport(320, 288, 128, 128, &viewport) == 0);
    CHECK(viewport.scale == 2);
    CHECK(viewport.width == 256);
    CHECK(viewport.height == 256);
    CHECK(viewport.x == 32);
    CHECK(viewport.y == 16);

    CHECK(p8p_integer_viewport(320, 240, 128, 128, &viewport) == 0);
    CHECK(viewport.scale == 1);
    CHECK(viewport.x == 96);
    CHECK(viewport.y == 56);
    CHECK(p8p_integer_viewport(100, 100, 128, 128, &viewport) == -2);
}

static void test_sharp_bilinear_scaling(void) {
    p8p_resample_point_t axis[288];
    int has_soft_edge = 0;

    CHECK(p8p_sharp_bilinear_axis(128, 2, 288, axis) == 0);
    CHECK(axis[0].first == 0);
    CHECK(axis[0].second == 0);
    CHECK(axis[287].first == 127);
    CHECK(axis[287].second == 127);
    for (int i = 0; i < 288; ++i) {
        CHECK(axis[i].first < 128);
        CHECK(axis[i].second < 128);
        CHECK(axis[i].second >= axis[i].first);
        CHECK(axis[i].second <= axis[i].first + 1);
        CHECK(axis[i].weight < 32);
        if (axis[i].first != axis[i].second && axis[i].weight != 0)
            has_soft_edge = 1;
    }
    CHECK(has_soft_edge);
    CHECK(p8p_sharp_bilinear_axis(0, 2, 288, axis) == -1);

    CHECK(p8p_rgb565_blend(0x0000, 0xffff, 0) == 0x0000);
    CHECK(p8p_rgb565_blend(0x0000, 0xffff, 16) == 0x7bef);
    CHECK(p8p_rgb565_blend(0x0000, 0xffff, 32) == 0xffff);
}

static void test_system_input(void) {
    CHECK(p8p_system_input_update(0, P8P_PHYS_SELECT, 0) ==
          P8P_SYSTEM_INPUT_OPEN_MENU);

    CHECK(p8p_system_input_update(P8P_PHYS_SELECT | P8P_PHYS_X,
                                  P8P_PHYS_SELECT | P8P_PHYS_X,
                                  0) ==
          P8P_SYSTEM_INPUT_TOGGLE_DIAGNOSTICS);

    CHECK(p8p_system_input_update(P8P_PHYS_START,
                                  P8P_PHYS_START, 0) ==
          P8P_SYSTEM_INPUT_NONE);
    CHECK(p8p_system_input_update(P8P_PHYS_START, 0, 0) ==
          P8P_SYSTEM_INPUT_NONE);
    CHECK(p8p_system_input_update(P8P_PHYS_L | P8P_PHYS_R,
                                  P8P_PHYS_L | P8P_PHYS_R, 0) ==
          P8P_SYSTEM_INPUT_NONE);
}

static void test_render_scheduler(void) {
    /* Crossgun: already comfortably below the 60 Hz budget. */
    CHECK(p8p_render_divisor_for_load(60, 6000, 1000, 1000, 3000) == 1);
    /* Scramble: a second render slot preserves logical speed. */
    CHECK(p8p_render_divisor_for_load(60, 14000, 6000, 2000, 3000) == 2);
    /* BAS: _update dominates, so R4 would ruin visuals for little gain. */
    CHECK(p8p_render_divisor_for_load(60, 29000, 25000, 3000, 3000) == 1);
    /* UFO: rendering dominates and skipping it materially improves speed. */
    CHECK(p8p_render_divisor_for_load(60, 58000, 17000, 1000, 3000) == 4);
    /* 30 Hz carts may skip at most every other draw. */
    CHECK(p8p_render_divisor_for_load(30, 45000, 15000, 5000, 3000) == 2);
}

int main(void) {
    test_text_cart();
    test_text_cart_with_bom();
    test_png_cart();
    test_zip_and_unknown();
    test_integer_scaling();
    test_sharp_bilinear_scaling();
    test_system_input();
    test_render_scheduler();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("all pico8pocket tests passed");
    return 0;
}
