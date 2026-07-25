#include "p8p/platform.h"
#include "p8p/display.h"
#include "p8p/runtime.h"

#include "of.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define P8P_SAFE_VIDEO_WIDTH 320
#define P8P_SAFE_VIDEO_HEIGHT 288

static uint8_t logical_framebuffer[P8P_SCREEN_WIDTH * P8P_SCREEN_HEIGHT];
static of_video_mode_t active_mode;
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

static void install_palette(void) {
    of_video_palette_bulk(pico8_palette, 16);
    for (int i = 0; i < 16; ++i) {
        uint32_t dimmed = dim_rgb_15(pico8_palette[i]);
        uint32_t extended_dimmed = dim_rgb_15(pico8_palette[16 + i]);

        of_video_palette((uint8_t)(16 + i), dimmed);
        of_video_palette((uint8_t)(32 + i), extended_dimmed);
        of_video_palette((uint8_t)(128 + i), pico8_palette[16 + i]);
    }
}

static int set_video_mode(void) {
    of_video_mode_t requested;
    of_video_mode_t normalized;

    memset(&requested, 0, sizeof(requested));
    requested.width = P8P_SAFE_VIDEO_WIDTH;
    requested.height = P8P_SAFE_VIDEO_HEIGHT;
    requested.stride = P8P_SAFE_VIDEO_WIDTH;
    requested.color_mode = OF_VIDEO_MODE_8BIT;
    if (of_video_check_mode(&requested, &normalized) != 0 ||
        normalized.width != P8P_SAFE_VIDEO_WIDTH ||
        normalized.height != P8P_SAFE_VIDEO_HEIGHT ||
        normalized.color_mode != OF_VIDEO_MODE_8BIT ||
        of_video_set_mode(&normalized) != 0)
        return -1;

    of_video_get_mode(&active_mode);
    if (active_mode.width != P8P_SAFE_VIDEO_WIDTH ||
        active_mode.height != P8P_SAFE_VIDEO_HEIGHT ||
        active_mode.color_mode != OF_VIDEO_MODE_8BIT)
        return -1;
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
    if (set_video_mode() != 0)
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

void p8p_platform_present(const uint8_t *framebuffer, const uint8_t *palette) {
    uint8_t *dest = of_video_surface();
    p8p_viewport_t viewport;

    if (!dest)
        return;
    if (!framebuffer)
        framebuffer = logical_framebuffer;

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
        if (!audio_paused && audio_gain_q15 == 32768 &&
            audio_volume == 100) {
            /* Normal gameplay already has unity gain.  The old loop still
             * multiplied and divided every stereo sample, wasting a visible
             * part of BAS Escape's 6-8 ms audio budget. */
        } else if (!audio_paused && audio_gain_q15 == 32768) {
            int gain = 32768 * (int)audio_volume / 100;
            for (int frame = 0; frame < count; ++frame) {
                int16_t sample = (int16_t)(
                    ((int32_t)audio_buffer[frame * 2] * gain) >> 15);
                audio_buffer[frame * 2] = sample;
                audio_buffer[frame * 2 + 1] = sample;
            }
        } else {
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
                int16_t sample = (int16_t)(
                    ((int32_t)audio_buffer[frame * 2] * gain) >> 15);
                audio_buffer[frame * 2] = sample;
                audio_buffer[frame * 2 + 1] = sample;
            }
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
    if (scale > 2)
        scale = 2;
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
