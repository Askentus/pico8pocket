#include "p8p/cart.h"
#include "p8p/menu.h"
#include "p8p/platform.h"
#include "p8p/runtime.h"
#include "p8p/settings.h"
#include "p8p/state_store.h"
#include "p8p/system_input.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int runtime_cartdata_load(void *userdata, const char *id,
                                 uint8_t *data, size_t size) {
    (void)userdata;
    return p8p_cartdata_load(id, data, size);
}

static int runtime_cartdata_save(void *userdata, const char *id,
                                 const uint8_t *data, size_t size) {
    (void)userdata;
    return p8p_cartdata_save(id, data, size);
}

static void fill_rect(uint8_t *fb, int x, int y, int width, int height,
                      uint8_t color) {
    for (int py = 0; py < height; ++py) {
        int dy = y + py;
        if ((unsigned)dy >= P8P_SCREEN_HEIGHT)
            continue;
        for (int px = 0; px < width; ++px) {
            int dx = x + px;
            if ((unsigned)dx < P8P_SCREEN_WIDTH)
                fb[dy * P8P_SCREEN_WIDTH + dx] = color;
        }
    }
}

static void draw_number(uint8_t *fb, int y, uint8_t color, unsigned value) {
    static const uint16_t digits[10] = {
        0x7b6f, 0x2c97, 0x62a7, 0x628e, 0x5bc9,
        0x798e, 0x39aa, 0x7292, 0x2aaa, 0x2ace
    };
    int values[2];

    if (value > 99) value = 99;
    values[0] = (int)(value / 10);
    values[1] = (int)(value % 10);
    fill_rect(fb, 0, y, 13, 7, 0);
    fill_rect(fb, 1, y + 1, 1, 5, color);
    for (int digit = 0; digit < 2; ++digit) {
        uint16_t bits = digits[values[digit]];
        for (int row = 0; row < 5; ++row)
            for (int column = 0; column < 3; ++column)
                if (bits & (1u << (14 - row * 3 - column)))
                    fb[(y + row + 1) * P8P_SCREEN_WIDTH +
                       4 + digit * 4 + column] = color;
    }
}

static void draw_profile(uint8_t *fb, unsigned fps, unsigned runtime_us,
                         unsigned audio_us, unsigned present_us) {
    draw_number(fb, 0, 7, fps);
    draw_number(fb, 7, 11, (runtime_us + 500) / 1000);
    draw_number(fb, 14, 9, (audio_us + 500) / 1000);
    draw_number(fb, 21, 12, (present_us + 500) / 1000);
}

static uint16_t error_glyph(unsigned char character) {
#define GLYPH(r0, r1, r2, r3, r4) \
    ((uint16_t)((r0) << 12) | (uint16_t)((r1) << 9) | \
     (uint16_t)((r2) << 6) | (uint16_t)((r3) << 3) | (uint16_t)(r4))
    if (character >= 'a' && character <= 'z')
        character = (unsigned char)(character - ('a' - 'A'));
    switch (character) {
    case '0': return GLYPH(2, 5, 5, 5, 2);
    case '1': return GLYPH(2, 6, 2, 2, 7);
    case '2': return GLYPH(6, 1, 2, 4, 7);
    case '3': return GLYPH(6, 1, 2, 1, 6);
    case '4': return GLYPH(5, 5, 7, 1, 1);
    case '5': return GLYPH(7, 4, 6, 1, 6);
    case '6': return GLYPH(3, 4, 6, 5, 2);
    case '7': return GLYPH(7, 1, 2, 2, 2);
    case '8': return GLYPH(2, 5, 2, 5, 2);
    case '9': return GLYPH(2, 5, 3, 1, 6);
    case 'A': return GLYPH(2, 5, 7, 5, 5);
    case 'B': return GLYPH(6, 5, 6, 5, 6);
    case 'C': return GLYPH(3, 4, 4, 4, 3);
    case 'D': return GLYPH(6, 5, 5, 5, 6);
    case 'E': return GLYPH(7, 4, 6, 4, 7);
    case 'F': return GLYPH(7, 4, 6, 4, 4);
    case 'G': return GLYPH(3, 4, 5, 5, 3);
    case 'H': return GLYPH(5, 5, 7, 5, 5);
    case 'I': return GLYPH(7, 2, 2, 2, 7);
    case 'J': return GLYPH(1, 1, 1, 5, 2);
    case 'K': return GLYPH(5, 5, 6, 5, 5);
    case 'L': return GLYPH(4, 4, 4, 4, 7);
    case 'M': return GLYPH(5, 7, 7, 5, 5);
    case 'N': return GLYPH(5, 7, 7, 7, 5);
    case 'O': return GLYPH(2, 5, 5, 5, 2);
    case 'P': return GLYPH(6, 5, 6, 4, 4);
    case 'Q': return GLYPH(2, 5, 5, 3, 1);
    case 'R': return GLYPH(6, 5, 6, 5, 5);
    case 'S': return GLYPH(3, 4, 2, 1, 6);
    case 'T': return GLYPH(7, 2, 2, 2, 2);
    case 'U': return GLYPH(5, 5, 5, 5, 7);
    case 'V': return GLYPH(5, 5, 5, 5, 2);
    case 'W': return GLYPH(5, 5, 7, 7, 5);
    case 'X': return GLYPH(5, 5, 2, 5, 5);
    case 'Y': return GLYPH(5, 5, 2, 2, 2);
    case 'Z': return GLYPH(7, 1, 2, 4, 7);
    case '!': return GLYPH(2, 2, 2, 0, 2);
    case '?': return GLYPH(6, 1, 2, 0, 2);
    case '.': return GLYPH(0, 0, 0, 0, 2);
    case ':': return GLYPH(0, 2, 0, 2, 0);
    case '-': return GLYPH(0, 0, 7, 0, 0);
    case '/': return GLYPH(1, 1, 2, 4, 4);
    case '_': return GLYPH(0, 0, 0, 0, 7);
    default: return 0;
    }
#undef GLYPH
}

static void draw_error_text(uint8_t *fb, int x, int y, const char *value,
                            uint8_t color) {
    while (value && *value && x <= P8P_SCREEN_WIDTH - 3) {
        uint16_t bits = error_glyph((unsigned char)*value++);
        for (int row = 0; row < 5; ++row)
            for (int column = 0; column < 3; ++column)
                if (bits & (1u << (14 - row * 3 - column)))
                    fb[(y + row) * P8P_SCREEN_WIDTH + x + column] = color;
        x += 4;
    }
}

static void draw_runtime_error_frame(uint8_t *fb, const char *message) {
    char line[31];
    int line_length = 0;
    int y = 15;

    memset(fb, 0, P8P_SCREEN_WIDTH * P8P_SCREEN_HEIGHT);
    fill_rect(fb, 0, 0, 128, 3, 8);
    fill_rect(fb, 0, 125, 128, 3, 8);
    fill_rect(fb, 0, 0, 3, 128, 8);
    fill_rect(fb, 125, 0, 3, 128, 8);
    draw_error_text(fb, 6, 6, "RUNTIME ERROR", 8);

    while (message && *message && y <= 108) {
        unsigned char c = (unsigned char)*message++;
        if (c == '\n' || line_length == 30) {
            line[line_length] = '\0';
            draw_error_text(fb, 4, y, line, 7);
            y += 7;
            line_length = 0;
            if (c == '\n')
                continue;
        }
        line[line_length++] = c >= 32 && c < 127 ? (char)c : '?';
    }
    if (line_length && y <= 108) {
        line[line_length] = '\0';
        draw_error_text(fb, 4, y, line, 7);
    }
    draw_error_text(fb, 40, 117, "SELECT MENU", 6);
}

static uint32_t smooth_time(uint32_t average, uint32_t sample) {
    return average ? (average * 7u + sample) / 8u : sample;
}

static int adaptive_render_divisor(uint32_t full_frame_us,
                                   uint32_t update_only_us,
                                   uint32_t audio_us,
                                   uint32_t present_us) {
    const uint32_t budget_us = 15000u;
    uint32_t update_us = update_only_us ? update_only_us : full_frame_us / 4u;
    uint32_t draw_us = full_frame_us > update_us ?
                       full_frame_us - update_us : 0;

    for (int divisor = 1; divisor <= 4; ++divisor) {
        uint32_t estimated_us = update_us + audio_us +
            (draw_us + (uint32_t)divisor - 1u) / (uint32_t)divisor +
            (present_us + (uint32_t)divisor - 1u) / (uint32_t)divisor;
        if (estimated_us <= budget_us)
            return divisor;
    }
    return 4;
}

typedef struct input_latch {
    p8p_physical_input_t pending;
    uint16_t previous_held;
    uint32_t last_poll_us;
    p8p_runtime_t *runtime;
    const p8p_control_profile_t *controls;
    uint16_t suppressed;
} input_latch_t;

static void input_latch_poll(input_latch_t *latch, int force) {
    p8p_physical_input_t sample;
    uint32_t now_us;

    if (!latch)
        return;
    now_us = p8p_platform_time_us();
    if (!force && now_us - latch->last_poll_us < 4000u)
        return;
    p8p_platform_poll_input(&sample);
    sample.pressed |= sample.held & (uint16_t)~latch->previous_held;
    sample.released |= latch->previous_held & (uint16_t)~sample.held;
    latch->pending.held = sample.held;
    latch->pending.pressed |= sample.pressed;
    latch->pending.released |= sample.released;
    latch->previous_held = sample.held;
    latch->last_poll_us = now_us;
}

static void input_latch_service(void *userdata) {
    input_latch_t *latch = (input_latch_t *)userdata;
    input_latch_poll(latch, 0);
    if (latch && latch->runtime && latch->controls) {
        uint16_t physical = latch->pending.held &
                            (uint16_t)~latch->suppressed;
        if (physical & P8P_PHYS_SELECT)
            physical &= (uint16_t)~P8P_PHYS_X;
        p8p_runtime_set_live_buttons(
            latch->runtime, p8p_controls_map(latch->controls, physical));
    }
}

static void input_latch_take(input_latch_t *latch,
                             p8p_physical_input_t *input) {
    input_latch_poll(latch, 1);
    *input = latch->pending;
    latch->pending.pressed = 0;
    latch->pending.released = 0;
}

static uint8_t status_color(const p8p_cart_info_t *cart) {
    if (!cart->valid)
        return 8;
    switch (cart->kind) {
    case P8P_CART_TEXT: return 11;
    case P8P_CART_PNG: return 12;
    case P8P_CART_ZIP: return 9;
    default: return 8;
    }
}

static void draw_bringup_frame(uint8_t *fb, const p8p_cart_info_t *cart,
                               int cursor_x, int cursor_y, int phase) {
    uint8_t border = status_color(cart);

    memset(fb, 1, P8P_SCREEN_WIDTH * P8P_SCREEN_HEIGHT);
    fill_rect(fb, 0, 0, 128, 4, border);
    fill_rect(fb, 0, 124, 128, 4, border);
    fill_rect(fb, 0, 0, 4, 128, border);
    fill_rect(fb, 124, 0, 4, 128, border);

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            uint8_t color = (uint8_t)(8 + ((x + y + phase) & 7));
            fill_rect(fb, 32 + x * 8, 32 + y * 8, 8, 8, color);
        }
    }

    fill_rect(fb, cursor_x - 2, cursor_y - 2, 5, 5, 7);
    fill_rect(fb, cursor_x - 1, cursor_y - 1, 3, 3, border);
}

int main(int argc, char **argv) {
    /* Slot 0 is the APF instance descriptor used to launch openfpgaOS. */
    const char *cart_path = "slot:4";
    p8p_cart_info_t cart = {0};
    p8p_cart_t loaded_cart = {0};
    p8p_runtime_t *runtime = NULL;
    p8p_menu_t *menu = NULL;
    p8p_settings_t settings;
    p8p_cart_hash_t cart_hash = {{0}};
    uint8_t *framebuffer;
    int cursor_x = 64;
    int cursor_y = 64;
    int phase = 0;
    int running = 0;
    int runtime_failed = 0;
    int menu_open = 0;
    int diagnostics_visible = 0;
    int render_divisor = 1;
    int render_phase = 0;
    int overload_frames = 0;
    int recovery_frames = 0;
    char runtime_error[256] = {0};
    uint16_t previous_physical = 0;
    uint16_t suppress_buttons = 0;
    input_latch_t input_latch = {0};
    uint32_t next_frame_us;
    uint32_t previous_frame_us;
    uint32_t average_period_us = 0;
    uint32_t average_runtime_us = 0;
    uint32_t average_draw_runtime_us = 0;
    uint32_t average_update_runtime_us = 0;
    uint32_t average_audio_us = 0;
    uint32_t average_present_us = 0;

#ifdef OF_PC
    if (argc > 1)
        cart_path = argv[1];
#else
    (void)argc;
    (void)argv;
#endif

    if (p8p_platform_init() != 0) {
        printf("pico8pocket: video initialization failed\n");
        return 1;
    }
    p8p_settings_load(&settings);
    settings.scale = (uint8_t)p8p_platform_set_scale(settings.scale);
    p8p_platform_audio_set_volume(settings.muted ? 0 : settings.volume);

    if (p8p_cart_probe_file(cart_path, &cart) != 0)
        memset(&cart, 0, sizeof(cart));

    printf("pico8pocket: cart=%s valid=%d size=%llu\n",
           p8p_cart_kind_name(cart.kind), cart.valid,
           (unsigned long long)cart.file_size);

    if ((cart.kind == P8P_CART_TEXT || cart.kind == P8P_CART_PNG) && cart.valid &&
        p8p_cart_load_file(cart_path, &loaded_cart) == 0) {
        runtime = p8p_runtime_create();
        if (runtime)
            p8p_runtime_set_cartdata_hooks(runtime, runtime_cartdata_load,
                                           runtime_cartdata_save, NULL);
        if (runtime && p8p_runtime_load(runtime, &loaded_cart) == 0) {
            running = 1;
            p8p_cart_content_hash(&loaded_cart, &cart_hash);
            menu = p8p_menu_create(&settings, &cart_hash,
                                   p8p_cart_kind_name(cart.kind));
            p8p_runtime_set_service_hook(runtime, input_latch_service,
                                         &input_latch);
            if (!menu)
                printf("pico8pocket: system menu allocation failed\n");
            printf("pico8pocket: z8lua cart started at %d fps\n",
                   p8p_runtime_target_fps(runtime));
        } else {
            printf("pico8pocket: runtime load failed: %s\n",
                   runtime ? p8p_runtime_error(runtime) : "out of memory");
            snprintf(runtime_error, sizeof(runtime_error), "%s",
                     runtime ? p8p_runtime_error(runtime) : "out of memory");
            runtime_failed = 1;
        }
    }

    framebuffer = p8p_platform_framebuffer();
    next_frame_us = p8p_platform_time_us();
    previous_frame_us = next_frame_us;
    for (;;) {
        const uint8_t *present_framebuffer = framebuffer;
        const uint8_t *present_palette = NULL;
        uint32_t frame_start_us = p8p_platform_time_us();
        uint32_t measured_period_us = frame_start_us - previous_frame_us;
        previous_frame_us = frame_start_us;
        if (measured_period_us >= 5000u && measured_period_us < 1000000u) {
            average_period_us = average_period_us ?
                (average_period_us * 7u + measured_period_us) / 8u :
                measured_period_us;
        }
        p8p_physical_input_t physical_input;
        uint16_t physical;
        uint16_t physical_pressed;
        uint8_t buttons;
        int target_fps = 60;
        int menu_opened_this_frame = 0;
        int drew_game_frame = 1;
        int present_required = 1;
        enum p8p_system_input_action system_action;

        input_latch_take(&input_latch, &physical_input);
        physical = physical_input.held;
        physical_pressed = physical_input.pressed |
                           (physical & (uint16_t)~previous_physical);
        suppress_buttons &= physical;

        system_action = p8p_system_input_update(
            physical, physical_pressed, menu_open);
        if ((running || runtime_failed) &&
            system_action == P8P_SYSTEM_INPUT_TOGGLE_DIAGNOSTICS) {
            diagnostics_visible ^= 1;
            suppress_buttons |= physical | physical_pressed;
            if (menu_open) {
                menu_open = 0;
                p8p_platform_audio_set_paused(0);
            }
        } else if ((running || runtime_failed) && menu &&
                   system_action == P8P_SYSTEM_INPUT_OPEN_MENU &&
                   !menu_open) {
            p8p_runtime_flush_cartdata(runtime);
            menu_open = 1;
            menu_opened_this_frame = 1;
            p8p_platform_audio_set_paused(1);
            p8p_menu_open(menu, runtime, physical);
        }

        const p8p_control_profile_t *controls =
            p8p_settings_controls(&settings, &cart_hash, NULL);
        input_latch.runtime = runtime;
        input_latch.controls = controls;
        input_latch.suppressed = suppress_buttons;
        uint16_t game_physical = (physical | physical_pressed) &
                                 (uint16_t)~suppress_buttons;
        if ((physical | physical_pressed) & P8P_PHYS_SELECT)
            game_physical &= (uint16_t)~P8P_PHYS_X;
        buttons = p8p_controls_map(controls, game_physical);

        if (menu_open && (running || runtime_failed) &&
            !menu_opened_this_frame) {
            enum p8p_menu_action action =
                p8p_menu_update(menu, runtime, physical, physical_pressed);
            present_framebuffer = p8p_menu_framebuffer(menu);
            present_palette = NULL;
            if (action == P8P_MENU_CLOSE) {
                menu_open = 0;
                suppress_buttons = physical;
                p8p_platform_audio_set_paused(0);
            } else if (action == P8P_MENU_RESTART) {
                if (p8p_runtime_load(runtime, &loaded_cart) != 0) {
                    printf("pico8pocket: restart failed: %s\n",
                           p8p_runtime_error(runtime));
                    running = 0;
                    runtime_failed = 1;
                    snprintf(runtime_error, sizeof(runtime_error), "%s",
                             p8p_runtime_error(runtime));
                } else {
                    running = 1;
                    runtime_failed = 0;
                    runtime_error[0] = '\0';
                    render_divisor = 1;
                    render_phase = 0;
                    overload_frames = 0;
                    recovery_frames = 0;
                    average_draw_runtime_us = 0;
                    average_update_runtime_us = 0;
                }
                menu_open = 0;
                suppress_buttons = physical;
                p8p_platform_audio_set_paused(0);
            } else if (action == P8P_MENU_TOGGLE_DIAGNOSTICS) {
                diagnostics_visible ^= 1;
                menu_open = 0;
                suppress_buttons = physical;
                p8p_platform_audio_set_paused(0);
            } else if (action == P8P_MENU_EXIT) {
                p8p_platform_exit();
            }
        } else if (menu_open && (running || runtime_failed)) {
            present_framebuffer = p8p_menu_framebuffer(menu);
            present_palette = NULL;
        } else if (!menu_open && running) {
            uint32_t runtime_start_us = p8p_platform_time_us();
            target_fps = p8p_runtime_target_fps(runtime);
            if (target_fps == 60 && render_divisor > 1) {
                drew_game_frame = render_phase == 0;
                render_phase = (render_phase + 1) % render_divisor;
            } else {
                drew_game_frame = 1;
                render_phase = 0;
            }
            if (p8p_runtime_step_with_draw(runtime, buttons,
                                           drew_game_frame) != 0) {
                printf("pico8pocket: runtime error: %s\n",
                       p8p_runtime_error(runtime));
                running = 0;
                runtime_failed = 1;
                snprintf(runtime_error, sizeof(runtime_error), "%s",
                         p8p_runtime_error(runtime));
            }
            uint32_t runtime_us = p8p_platform_time_us() - runtime_start_us;
            average_runtime_us = smooth_time(average_runtime_us, runtime_us);
            if (drew_game_frame)
                average_draw_runtime_us = smooth_time(
                    average_draw_runtime_us, runtime_us);
            else
                average_update_runtime_us = smooth_time(
                    average_update_runtime_us, runtime_us);
            if (running) {
                present_framebuffer = p8p_runtime_framebuffer(runtime);
                present_palette = p8p_runtime_screen_palette(runtime);
                if (target_fps == 60 && drew_game_frame &&
                    average_draw_runtime_us) {
                    int desired = adaptive_render_divisor(
                        average_draw_runtime_us, average_update_runtime_us,
                        average_audio_us, average_present_us);
                    if (desired > render_divisor) {
                        recovery_frames = 0;
                        if (++overload_frames >= 3) {
                            render_divisor = desired;
                            render_phase = 0;
                            overload_frames = 0;
                        }
                    } else if (desired < render_divisor) {
                        overload_frames = 0;
                        if (++recovery_frames >= 45) {
                            --render_divisor;
                            render_phase = 0;
                            recovery_frames = 0;
                        }
                    } else {
                        overload_frames = 0;
                        recovery_frames = 0;
                    }
                } else if (target_fps != 60) {
                    render_divisor = 1;
                    render_phase = 0;
                }
                present_required = drew_game_frame || diagnostics_visible;
            }
        }
        if (!running) {
            if (runtime_failed) {
                draw_runtime_error_frame(framebuffer, runtime_error);
            } else {
                if ((buttons & P8P_BTN_LEFT) && cursor_x > 5) --cursor_x;
                if ((buttons & P8P_BTN_RIGHT) && cursor_x < 122) ++cursor_x;
                if ((buttons & P8P_BTN_UP) && cursor_y > 5) --cursor_y;
                if ((buttons & P8P_BTN_DOWN) && cursor_y < 122) ++cursor_y;
                if (buttons & P8P_BTN_O) phase = (phase + 1) & 7;
                if (buttons & P8P_BTN_X) phase = (phase - 1) & 7;
                draw_bringup_frame(framebuffer, &cart, cursor_x, cursor_y,
                                   phase);
            }
            present_framebuffer = framebuffer;
            present_palette = NULL;
            present_required = 1;
        }
        if (!menu_open && diagnostics_visible && average_period_us) {
            unsigned displayed_fps = 1000000u / average_period_us;
            if (running && target_fps == 60 && render_divisor > 1)
                displayed_fps = (displayed_fps + (unsigned)render_divisor - 1u) /
                                (unsigned)render_divisor;
            if (present_palette) {
                for (int pixel = 0; pixel < P8P_SCREEN_WIDTH * P8P_SCREEN_HEIGHT;
                     ++pixel)
                    framebuffer[pixel] =
                        present_palette[present_framebuffer[pixel] & 15];
            } else if (present_framebuffer != framebuffer) {
                memcpy(framebuffer, present_framebuffer,
                       P8P_SCREEN_WIDTH * P8P_SCREEN_HEIGHT);
            }
            present_framebuffer = framebuffer;
            present_palette = NULL;
            draw_profile(framebuffer, displayed_fps,
                         average_runtime_us, average_audio_us,
                         average_present_us);
        }
        if (running) {
            uint32_t audio_start_us = p8p_platform_time_us();
            p8p_platform_audio_pump(runtime);
            average_audio_us = smooth_time(
                average_audio_us, p8p_platform_time_us() - audio_start_us);
        }
        if (present_required) {
            uint32_t present_start_us = p8p_platform_time_us();
            p8p_platform_present(present_framebuffer, present_palette);
            average_present_us = smooth_time(
                average_present_us,
                p8p_platform_time_us() - present_start_us);
        }

        /*
         * Pace logical frames directly.  The previous 60 Hz tick scheduler
         * always inserted an extra duplicate-vsync pass for 30 Hz carts.
         * Once a cart missed its budget that made an already-slow frame wait
         * yet another refresh.  A deadline scheduler sleeps only while the
         * emulator is ahead and immediately continues when it is behind.
         */
        uint32_t frame_period_us = target_fps == 30 ? 33333u : 16667u;
        uint32_t now_us = p8p_platform_time_us();
        next_frame_us += frame_period_us;
        int32_t remaining_us = (int32_t)(next_frame_us - now_us);
        if (remaining_us > 0)
            p8p_platform_sleep_us((uint32_t)remaining_us);
        else
            next_frame_us = now_us;
        previous_physical = physical;
    }
}
