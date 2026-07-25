#include "p8p/cart.h"
#include "p8p/menu.h"
#include "p8p/platform.h"
#include "p8p/runtime.h"
#include "p8p/scheduler.h"
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

static uint16_t error_glyph(unsigned char character);

static void draw_metric(uint8_t *fb, int x, int y, char label, uint8_t color,
                        unsigned value, int digits) {
    unsigned limit = digits == 3 ? 999u : 99u;
    unsigned divisor = digits == 3 ? 100u : 10u;

    if (value > limit) value = limit;
    fill_rect(fb, x, y, 5 + digits * 4, 7, 0);
    for (int glyph_index = 0; glyph_index <= digits; ++glyph_index) {
        unsigned char character = glyph_index == 0 ? (unsigned char)label :
            (unsigned char)('0' + (value / divisor) % 10u);
        uint16_t bits = error_glyph(character);
        for (int row = 0; row < 5; ++row)
            for (int column = 0; column < 3; ++column)
                if (bits & (1u << (14 - row * 3 - column)))
                    fb[(y + row + 1) * P8P_SCREEN_WIDTH +
                       x + 1 + glyph_index * 4 + column] = color;
        if (glyph_index > 0 && divisor > 1)
            divisor /= 10u;
    }
}

static void draw_profile(uint8_t *fb, unsigned logical_fps,
                         unsigned visible_fps, unsigned update_us,
                         unsigned draw_us, unsigned audio_us,
                         unsigned present_us, unsigned render_divisor,
                         unsigned lua_instructions,
                         const p8p_runtime_api_profile_t *apis) {
    draw_metric(fb, 0, 0,  'L', 7,  logical_fps, 2);
    draw_metric(fb, 0, 7,  'V', 6,  visible_fps, 2);
    draw_metric(fb, 0, 14, 'U', 11, (update_us + 500) / 1000, 2);
    draw_metric(fb, 0, 21, 'D', 8,  (draw_us + 500) / 1000, 2);
    draw_metric(fb, 0, 28, 'A', 9,  (audio_us + 500) / 1000, 2);
    draw_metric(fb, 0, 35, 'P', 12, (present_us + 500) / 1000, 2);
    draw_metric(fb, 0, 42, 'R', 14, render_divisor, 2);
    draw_metric(fb, 0, 49, 'K', 10, (lua_instructions + 500) / 1000, 3);
    if (apis) {
        draw_metric(fb, 18, 0,  'S', 12, apis->calls[P8P_API_SPRITE], 3);
        draw_metric(fb, 18, 7,  'G', 8,  apis->calls[P8P_API_GRAPHICS], 3);
        draw_metric(fb, 18, 14, 'M', 9,  apis->calls[P8P_API_MEMORY], 3);
        draw_metric(fb, 18, 21, 'C', 14, apis->calls[P8P_API_DRAW_STATE], 3);
        draw_metric(fb, 18, 28, 'T', 7,  apis->calls[P8P_API_TEXT], 3);
        draw_metric(fb, 18, 35, 'B', 11, apis->calls[P8P_API_INPUT], 3);
        draw_metric(fb, 18, 42, 'Q', 10, apis->calls[P8P_API_HELPER], 3);
    }
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

typedef struct runtime_profile {
    uint32_t update_start_us;
    uint32_t draw_start_us;
    uint32_t average_update_us;
    uint32_t average_draw_us;
} runtime_profile_t;

static void runtime_profile_event(void *userdata,
                                  p8p_runtime_profile_event_t event) {
    runtime_profile_t *profile = (runtime_profile_t *)userdata;
    uint32_t now_us;

    if (!profile)
        return;
    now_us = p8p_platform_time_us();
    switch (event) {
    case P8P_PROFILE_UPDATE_BEGIN:
        profile->update_start_us = now_us;
        break;
    case P8P_PROFILE_UPDATE_END:
        profile->average_update_us = smooth_time(
            profile->average_update_us, now_us - profile->update_start_us);
        break;
    case P8P_PROFILE_DRAW_BEGIN:
        profile->draw_start_us = now_us;
        break;
    case P8P_PROFILE_DRAW_END:
        profile->average_draw_us = smooth_time(
            profile->average_draw_us, now_us - profile->draw_start_us);
        break;
    }
}

static void set_diagnostics(p8p_runtime_t *runtime, int *visible,
                            runtime_profile_t *profile,
                            uint32_t *average_instructions,
                            int enabled) {
    *visible = enabled != 0;
    memset(profile, 0, sizeof(*profile));
    *average_instructions = 0;
    p8p_runtime_set_profile_hook(runtime,
        *visible ? runtime_profile_event : NULL,
        *visible ? profile : NULL);
}

typedef struct input_latch {
    p8p_physical_input_t pending;
    uint16_t previous_held;
    uint32_t last_poll_us;
    p8p_runtime_t *runtime;
    const p8p_control_profile_t *controls;
    uint16_t suppressed;
    uint32_t instruction_hooks;
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
    if (latch)
        ++latch->instruction_hooks;
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
    uint32_t average_draw_runtime_us = 0;
    uint32_t average_update_runtime_us = 0;
    uint32_t average_audio_us = 0;
    uint32_t average_present_us = 0;
    uint32_t average_instructions = 0;
    runtime_profile_t runtime_profile = {0};

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
            set_diagnostics(runtime, &diagnostics_visible, &runtime_profile,
                            &average_instructions, !diagnostics_visible);
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
                    memset(&runtime_profile, 0, sizeof(runtime_profile));
                    average_instructions = 0;
                }
                menu_open = 0;
                suppress_buttons = physical;
                p8p_platform_audio_set_paused(0);
            } else if (action == P8P_MENU_TOGGLE_DIAGNOSTICS) {
                set_diagnostics(runtime, &diagnostics_visible,
                                &runtime_profile, &average_instructions,
                                !diagnostics_visible);
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
            uint32_t instruction_hooks_before = input_latch.instruction_hooks;
            target_fps = p8p_runtime_target_fps(runtime);
            if (render_divisor > 1) {
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
            if (diagnostics_visible) {
                uint32_t hook_count = input_latch.instruction_hooks -
                                      instruction_hooks_before;
                average_instructions = smooth_time(
                    average_instructions, hook_count * 8192u);
            }
            if (drew_game_frame)
                average_draw_runtime_us = smooth_time(
                    average_draw_runtime_us, runtime_us);
            else
                average_update_runtime_us = smooth_time(
                    average_update_runtime_us, runtime_us);
            if (running) {
                present_framebuffer = p8p_runtime_framebuffer(runtime);
                present_palette = p8p_runtime_screen_palette(runtime);
                if (drew_game_frame && average_draw_runtime_us) {
                    int desired = p8p_render_divisor_for_load(
                        target_fps, average_draw_runtime_us,
                        average_update_runtime_us, average_audio_us,
                        average_present_us);
                    if (desired > render_divisor) {
                        recovery_frames = 0;
                        if (++overload_frames >= 3) {
                            render_divisor = desired;
                            render_phase = 0;
                            overload_frames = 0;
                        }
                    } else if (desired < render_divisor) {
                        overload_frames = 0;
                        if (++recovery_frames >= 6) {
                            /* Timings are already smoothed and have now stayed
                             * below the lower-load threshold for six rendered
                             * frames.  Return directly to the divisor they can
                             * sustain: stepping 4->3->2->1 left particle-heavy
                             * carts visibly blurry/jerky for almost a second
                             * after an effect had ended. */
                            render_divisor = desired;
                            render_phase = 0;
                            recovery_frames = 0;
                        }
                    } else {
                        overload_frames = 0;
                        recovery_frames = 0;
                    }
                }
                /* Do not let the profiler force extra hardware flips: it
                 * refreshes with the same cadence as visible game frames. */
                present_required = drew_game_frame;
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
        if (!menu_open && diagnostics_visible && average_period_us &&
            present_required) {
            unsigned logical_fps = 1000000u / average_period_us;
            unsigned visible_fps;
            if (logical_fps > (unsigned)target_fps)
                logical_fps = (unsigned)target_fps;
            visible_fps = logical_fps;
            if (running && render_divisor > 1)
                visible_fps = (visible_fps + (unsigned)render_divisor - 1u) /
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
            p8p_runtime_api_profile_t api_profile;
            p8p_runtime_get_api_profile(runtime, &api_profile);
            draw_profile(framebuffer, logical_fps, visible_fps,
                         runtime_profile.average_update_us,
                         runtime_profile.average_draw_us,
                         average_audio_us, average_present_us,
                         (unsigned)render_divisor, average_instructions,
                         &api_profile);
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
        else if (remaining_us < -(int32_t)(frame_period_us * 4u))
            /* Keep a bounded amount of lateness so cheap update-only frames
             * can repay one expensive rendered frame.  Resetting the deadline
             * after every single overrun defeated render skipping: Celeste 2
             * had a 44 ms draw frame followed by an 18 ms update frame (62 ms
             * per two 30 Hz ticks, within budget), yet the reset made the
             * cheap frame wait a fresh 33 ms and reduced the game to ~23 FPS.
             * Drop the debt only when a cart is over four complete frames
             * behind, preventing an unrecoverable catch-up spiral. */
            next_frame_us = now_us;
        previous_physical = physical;
    }
}
