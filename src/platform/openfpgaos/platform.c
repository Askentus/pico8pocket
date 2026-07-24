#include "p8p/platform.h"
#include "p8p/display.h"
#include "p8p/runtime.h"

#include "of.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define P8P_SAFE_VIDEO_WIDTH 320
#define P8P_SAFE_VIDEO_HEIGHT 288
#define P8P_FULL_SOFT_SIZE 288
#define P8P_SCALE_FULL_SOFT 3

static uint8_t logical_framebuffer[P8P_SCREEN_WIDTH * P8P_SCREEN_HEIGHT];
static of_video_mode_t active_mode;
static uint16_t palette_rgb565[256];
static p8p_resample_point_t full_soft_axis[P8P_FULL_SOFT_SIZE];
static uint16_t full_soft_rows[2][P8P_FULL_SOFT_SIZE];
static int audio_ring_capacity;
static int16_t audio_buffer[1024 * 2];
static unsigned requested_scale;
static unsigned audio_volume = 100;
static int audio_paused;
static int audio_gain_q15 = 32768;

static const uint32_t pico8_palette[32] = {
    0x000000, 0x1d2b53, 0x7e2553, 0x008751,
    0xab5236, 0x5f574f, 0xc2c3c7, 0xfff1e8,
    0xff004d, 0xffa300, 0xffec27, 0x00e436,
    0x29adff, 0x83769c, 0xff77a8, 0xffccaa,
    0x291814, 0x111d35, 0x422136, 0x125359,
    0x742f29, 0x49333b, 0xa28879, 0xf3ef7d,
    0xbe1250, 0xff6c24, 0xa8e72e, 0x00b543,
    0x065ab5, 0x754665, 0xff6e59, 0xff9d81
};

static uint32_t dim_rgb_15(uint32_t color) {
    uint32_t red = (((color >> 16) & 0xffu) * 15u + 50u) / 100u;
    uint32_t green = (((color >> 8) & 0xffu) * 15u + 50u) / 100u;
    uint32_t blue = ((color & 0xffu) * 15u + 50u) / 100u;
    return (red << 16) | (green << 8) | blue;
}

static uint16_t rgb888_to_rgb565(uint32_t color) {
    return (uint16_t)((((color >> 16) & 0xf8u) << 8) |
                      (((color >> 8) & 0xfcu) << 3) |
                      ((color & 0xffu) >> 3));
}

static void install_palette(void) {
    memset(palette_rgb565, 0, sizeof(palette_rgb565));
    of_video_palette_bulk(pico8_palette, 16);
    for (int i = 0; i < 16; ++i) {
        uint32_t dimmed = dim_rgb_15(pico8_palette[i]);
        uint32_t extended_dimmed = dim_rgb_15(pico8_palette[16 + i]);

        palette_rgb565[i] = rgb888_to_rgb565(pico8_palette[i]);
        palette_rgb565[16 + i] = rgb888_to_rgb565(dimmed);
        palette_rgb565[32 + i] = rgb888_to_rgb565(extended_dimmed);
        palette_rgb565[128 + i] = rgb888_to_rgb565(pico8_palette[16 + i]);
        of_video_palette((uint8_t)(16 + i), dimmed);
        of_video_palette((uint8_t)(32 + i), extended_dimmed);
        of_video_palette((uint8_t)(128 + i), pico8_palette[16 + i]);
    }
}

static int set_video_color_mode(uint8_t color_mode) {
    of_video_mode_t requested;
    of_video_mode_t normalized;

    memset(&requested, 0, sizeof(requested));
    requested.width = P8P_SAFE_VIDEO_WIDTH;
    requested.height = P8P_SAFE_VIDEO_HEIGHT;
    requested.stride = color_mode == OF_VIDEO_MODE_RGB565 ?
                       P8P_SAFE_VIDEO_WIDTH * 2 : P8P_SAFE_VIDEO_WIDTH;
    requested.color_mode = color_mode;
    if (of_video_check_mode(&requested, &normalized) != 0 ||
        normalized.width != P8P_SAFE_VIDEO_WIDTH ||
        normalized.height != P8P_SAFE_VIDEO_HEIGHT ||
        normalized.color_mode != color_mode ||
        of_video_set_mode(&normalized) != 0)
        return -1;

    of_video_get_mode(&active_mode);
    if (active_mode.width != P8P_SAFE_VIDEO_WIDTH ||
        active_mode.height != P8P_SAFE_VIDEO_HEIGHT ||
        active_mode.color_mode != color_mode)
        return -1;
    if (color_mode == OF_VIDEO_MODE_8BIT)
        install_palette();
    return 0;
}

int p8p_platform_init(void) {
    of_video_init();
    /*
     * The current openfpgaOS Pocket bitstream has a proven 320x288 scanout
     * path.  Requesting 128x128 changes the software framebuffer geometry,
     * but does not produce a matching APF scanout timing on hardware; Pocket
     * then magnifies only part of the image.  Scale PICO-8 2x into 320x288
     * instead, matching the known-good pocket-8/openfpgaOS contract.
     */
    if (set_video_color_mode(OF_VIDEO_MODE_8BIT) != 0)
        return -1;
    if (p8p_sharp_bilinear_axis(P8P_SCREEN_WIDTH, 2,
                                P8P_FULL_SOFT_SIZE,
                                full_soft_axis) != 0)
        return -1;

    memset(logical_framebuffer, 0, sizeof(logical_framebuffer));
    /* This clears all three retained scanout pages, including the borders. */
    of_video_clear(0);
    of_audio_init();
    audio_ring_capacity = of_audio_free();
    return 0;
}

uint8_t *p8p_platform_framebuffer(void) {
    return logical_framebuffer;
}

static uint16_t framebuffer_rgb565(const uint8_t *framebuffer,
                                   const uint8_t *palette,
                                   int offset) {
    uint8_t color = framebuffer[offset];
    uint8_t index = palette ? palette[color & 15] : color;
    return palette_rgb565[index];
}

static void scale_full_soft_row(uint16_t *output,
                                const uint8_t *framebuffer,
                                const uint8_t *palette,
                                int source_y) {
    uint16_t source_colors[P8P_SCREEN_WIDTH];
    const uint8_t *source = framebuffer + source_y * P8P_SCREEN_WIDTH;

    for (int source_x = 0; source_x < P8P_SCREEN_WIDTH; ++source_x)
        source_colors[source_x] =
            framebuffer_rgb565(source, palette, source_x);
    for (int output_x = 0; output_x < P8P_FULL_SOFT_SIZE; ++output_x) {
        const p8p_resample_point_t *point = &full_soft_axis[output_x];
        uint16_t first = source_colors[point->first];
        output[output_x] = point->first == point->second ? first :
            p8p_rgb565_blend(first, source_colors[point->second],
                             point->weight);
    }
}

static int full_soft_row_slot(int source_y, int protected_slot,
                              int cached_source[2],
                              const uint8_t *framebuffer,
                              const uint8_t *palette) {
    int slot;

    if (cached_source[0] == source_y)
        return 0;
    if (cached_source[1] == source_y)
        return 1;
    slot = protected_slot == 0 ? 1 : 0;
    scale_full_soft_row(full_soft_rows[slot], framebuffer, palette, source_y);
    cached_source[slot] = source_y;
    return slot;
}

static void present_full_soft(uint8_t *dest, const uint8_t *framebuffer,
                              const uint8_t *palette) {
    int cached_source[2] = {-1, -1};
    int output_x = ((int)active_mode.width - P8P_FULL_SOFT_SIZE) / 2;

    for (int output_y = 0; output_y < P8P_FULL_SOFT_SIZE; ++output_y) {
        const p8p_resample_point_t *point = &full_soft_axis[output_y];
        int first_slot = full_soft_row_slot(point->first, -1, cached_source,
                                            framebuffer, palette);
        int second_slot = point->first == point->second ? first_slot :
            full_soft_row_slot(point->second, first_slot, cached_source,
                               framebuffer, palette);
        uint16_t *row = (uint16_t *)(dest +
            (uint32_t)output_y * active_mode.stride + output_x * 2);

        if (first_slot == second_slot) {
            memcpy(row, full_soft_rows[first_slot],
                   P8P_FULL_SOFT_SIZE * sizeof(*row));
        } else {
            for (int x = 0; x < P8P_FULL_SOFT_SIZE; ++x)
                row[x] = p8p_rgb565_blend(full_soft_rows[first_slot][x],
                                          full_soft_rows[second_slot][x],
                                          point->weight);
        }
    }
}

void p8p_platform_present(const uint8_t *framebuffer, const uint8_t *palette) {
    uint8_t *dest = of_video_surface();
    p8p_viewport_t viewport;

    if (!dest)
        return;
    if (!framebuffer)
        framebuffer = logical_framebuffer;

    if (requested_scale == P8P_SCALE_FULL_SOFT &&
        active_mode.color_mode == OF_VIDEO_MODE_RGB565) {
        present_full_soft(dest, framebuffer, palette);
        of_video_flip();
        return;
    }

    if (p8p_integer_viewport(active_mode.width, active_mode.height,
                             P8P_SCREEN_WIDTH, P8P_SCREEN_HEIGHT,
                             &viewport) == 0) {
        if (requested_scale && requested_scale < (unsigned)viewport.scale) {
            viewport.scale = (int)requested_scale;
            viewport.width = P8P_SCREEN_WIDTH * viewport.scale;
            viewport.height = P8P_SCREEN_HEIGHT * viewport.scale;
            viewport.x = ((int)active_mode.width - viewport.width) / 2;
            viewport.y = ((int)active_mode.height - viewport.height) / 2;
        }
        /* The known-good Pocket mode always gives us a 2x viewport. */
        if (viewport.scale == 2) {
            for (int source_y = 0; source_y < P8P_SCREEN_HEIGHT; ++source_y) {
                uint8_t *row0 = dest +
                    (uint32_t)(viewport.y + source_y * 2) * active_mode.stride +
                    viewport.x;
                uint16_t *pairs = (uint16_t *)row0;
                const uint8_t *source = framebuffer +
                    (uint32_t)source_y * P8P_SCREEN_WIDTH;
                for (int source_x = 0; source_x < P8P_SCREEN_WIDTH; ++source_x) {
                    uint16_t color = palette ?
                        palette[source[source_x] & 15] : source[source_x];
                    pairs[source_x] = (uint16_t)(color | (color << 8));
                }
                memcpy(row0 + active_mode.stride, row0,
                       P8P_SCREEN_WIDTH * 2);
            }
        } else {
            for (int source_y = 0; source_y < P8P_SCREEN_HEIGHT; ++source_y) {
                for (int repeat_y = 0; repeat_y < viewport.scale; ++repeat_y) {
                    uint8_t *row = dest +
                        (uint32_t)(viewport.y + source_y * viewport.scale + repeat_y) *
                        active_mode.stride + viewport.x;
                    const uint8_t *source = framebuffer +
                        (uint32_t)source_y * P8P_SCREEN_WIDTH;
                    for (int source_x = 0; source_x < P8P_SCREEN_WIDTH; ++source_x) {
                        uint8_t color = palette ?
                            palette[source[source_x] & 15] : source[source_x];
                        for (int repeat_x = 0; repeat_x < viewport.scale; ++repeat_x)
                            *row++ = color;
                    }
                }
            }
        }
    }

    of_video_flip();
}

void p8p_platform_audio_pump(p8p_runtime_t *runtime) {
    const int target_queued_frames = 2048;
    int free_frames;
    int queued_frames;

    if (!runtime || audio_ring_capacity <= 0)
        return;
    free_frames = of_audio_free();
    if (free_frames <= 0)
        return;
    queued_frames = audio_ring_capacity - free_frames;
    if (queued_frames < 0)
        queued_frames = 0;

    while (queued_frames < target_queued_frames && free_frames > 0) {
        int count = target_queued_frames - queued_frames;
        int written;
        if (count > 1024) count = 1024;
        if (count > free_frames) count = free_frames;
        if (!audio_paused || audio_gain_q15 > 0)
            p8p_runtime_audio_render(runtime, audio_buffer, (size_t)count);
        else
            memset(audio_buffer, 0, (size_t)count * 2 * sizeof(*audio_buffer));
        for (int frame = 0; frame < count; ++frame) {
            int target = audio_paused ? 0 : 32768;
            if (audio_gain_q15 < target) {
                audio_gain_q15 += 32;
                if (audio_gain_q15 > target) audio_gain_q15 = target;
            } else if (audio_gain_q15 > target) {
                audio_gain_q15 -= 32;
                if (audio_gain_q15 < target) audio_gain_q15 = target;
            }
            int gain = audio_gain_q15 * (int)audio_volume / 100;
            audio_buffer[frame * 2] =
                (int16_t)(((int32_t)audio_buffer[frame * 2] * gain) >> 15);
            audio_buffer[frame * 2 + 1] =
                (int16_t)(((int32_t)audio_buffer[frame * 2 + 1] * gain) >> 15);
        }
        written = of_audio_write(audio_buffer, count);
        if (written <= 0)
            break;
        queued_frames += written;
        free_frames -= written;
    }
}

void p8p_platform_audio_set_paused(int paused) {
    audio_paused = paused != 0;
}

void p8p_platform_audio_set_volume(unsigned percent) {
    audio_volume = percent > 100 ? 100 : percent;
}

unsigned p8p_platform_set_scale(unsigned scale) {
    uint8_t wanted_color_mode;

    if (scale > P8P_SCALE_FULL_SOFT)
        scale = 2;
    wanted_color_mode = scale == P8P_SCALE_FULL_SOFT ?
                        OF_VIDEO_MODE_RGB565 : OF_VIDEO_MODE_8BIT;
    if (active_mode.color_mode != wanted_color_mode &&
        set_video_color_mode(wanted_color_mode) != 0) {
        scale = active_mode.color_mode == OF_VIDEO_MODE_RGB565 ?
                P8P_SCALE_FULL_SOFT : 2;
    }
    if (requested_scale != scale) {
        requested_scale = scale;
        of_video_clear(0);
    }
    return requested_scale;
}

uint8_t p8p_platform_dim_color(uint8_t color) {
    if (color < 16)
        return (uint8_t)(16 + color);
    if (color >= 128 && color < 144)
        return (uint8_t)(32 + color - 128);
    return (uint8_t)(16 + (color & 15));
}

void p8p_platform_poll_input(p8p_physical_input_t *input) {
    if (!input)
        return;
    memset(input, 0, sizeof(*input));
    of_input_poll_p0();
#define MAP_BUTTON(of_button, p8p_button) do { \
    if (of_btn(of_button)) input->held |= p8p_button; \
    if (of_btn_pressed(of_button)) input->pressed |= p8p_button; \
    if (of_btn_released(of_button)) input->released |= p8p_button; \
} while (0)
    MAP_BUTTON(OF_BTN_LEFT, P8P_PHYS_LEFT);
    MAP_BUTTON(OF_BTN_RIGHT, P8P_PHYS_RIGHT);
    MAP_BUTTON(OF_BTN_UP, P8P_PHYS_UP);
    MAP_BUTTON(OF_BTN_DOWN, P8P_PHYS_DOWN);
    MAP_BUTTON(OF_BTN_A, P8P_PHYS_A);
    MAP_BUTTON(OF_BTN_B, P8P_PHYS_B);
    MAP_BUTTON(OF_BTN_X, P8P_PHYS_X);
    MAP_BUTTON(OF_BTN_Y, P8P_PHYS_Y);
    MAP_BUTTON(OF_BTN_L1, P8P_PHYS_L);
    MAP_BUTTON(OF_BTN_R1, P8P_PHYS_R);
    MAP_BUTTON(OF_BTN_START, P8P_PHYS_START);
    MAP_BUTTON(OF_BTN_SELECT, P8P_PHYS_SELECT);
#undef MAP_BUTTON
}

uint16_t p8p_platform_poll_physical(void) {
    p8p_physical_input_t input;
    p8p_platform_poll_input(&input);
    return input.held;
}

uint8_t p8p_platform_poll_buttons(void) {
    uint16_t physical = p8p_platform_poll_physical();
    uint8_t result = 0;

    if (physical & P8P_PHYS_LEFT) result |= P8P_BTN_LEFT;
    if (physical & P8P_PHYS_RIGHT) result |= P8P_BTN_RIGHT;
    if (physical & P8P_PHYS_UP) result |= P8P_BTN_UP;
    if (physical & P8P_PHYS_DOWN) result |= P8P_BTN_DOWN;

    /* Fake-08/libretro convention: bottom B is PICO-8 O, right A is X. */
    if (physical & (P8P_PHYS_B | P8P_PHYS_Y)) result |= P8P_BTN_O;
    if (physical & (P8P_PHYS_A | P8P_PHYS_X)) result |= P8P_BTN_X;
    if (physical & P8P_PHYS_START) result |= P8P_BTN_PAUSE;
    if (physical & P8P_PHYS_SELECT) result |= P8P_BTN_MENU;
    return result;
}

void p8p_platform_wait_frame(void) {
    of_video_wait_flip();
}

uint32_t p8p_platform_time_us(void) {
    return of_time_us();
}

void p8p_platform_sleep_us(uint32_t microseconds) {
    if (microseconds)
        usleep(microseconds);
}

void p8p_platform_exit(void) {
    of_exit();
}
