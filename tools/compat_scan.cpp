#include "p8p/cart.h"
#include "p8p/runtime.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static uint64_t lua_service_hooks;

static void count_lua_service_hook(void *) {
    ++lua_service_hooks;
}

static uint32_t framebuffer_hash(p8p_runtime_t *runtime) {
    const uint8_t *framebuffer = p8p_runtime_framebuffer(runtime);
    uint32_t hash = 2166136261u;
    for (int pixel = 0; pixel < 128 * 128; ++pixel) {
        hash ^= framebuffer[pixel];
        hash *= 16777619u;
    }
    return hash;
}

static uint8_t scripted_input(unsigned frame, uint32_t *rng) {
    if (frame < 90)
        return 0;
    if (frame == 90 || frame == 150 || frame == 210)
        return (1u << 4) | (1u << 6);

    *rng = *rng * 1664525u + 1013904223u;
    uint8_t buttons = 0;
    unsigned direction = (*rng >> 28) & 7u;
    if (direction < 4)
        buttons |= (uint8_t)(1u << direction);
    if (((*rng >> 8) & 15u) == 0)
        buttons |= 1u << 4;
    if (((*rng >> 16) & 15u) == 0)
        buttons |= 1u << 5;
    if (frame % 257u == 0)
        buttons |= 1u << 6;
    if (buttons & (1u << 4))
        buttons |= 1u << 6;
    return buttons;
}

static int run_scan(const char *path, unsigned frames, uint32_t seed,
                    int draw_frame, const char *setup_expression) {
    p8p_cart_t cart = {};
    p8p_runtime_t *runtime = nullptr;
    uint64_t elapsed_ns = 0;
    uint64_t slowest_ns = 0;

    if (p8p_cart_load_file(path, &cart) != 0) {
        std::fprintf(stderr, "cannot load %s\n", path);
        return 2;
    }
    runtime = p8p_runtime_create();
    if (!runtime || p8p_runtime_load(runtime, &cart) != 0) {
        std::fprintf(stderr, "load error: %s\n",
                     runtime ? p8p_runtime_error(runtime) : "out of memory");
        p8p_runtime_destroy(runtime);
        p8p_cart_destroy(&cart);
        return 3;
    }
#ifdef P8P_RUNTIME_DEBUG
    if (setup_expression) {
        int setup_value = 0;
        if (p8p_runtime_debug_eval_int(runtime, setup_expression,
                                       &setup_value) != 0) {
            std::fprintf(stderr, "setup expression failed: %s\n",
                         setup_expression);
            p8p_runtime_destroy(runtime);
            p8p_cart_destroy(&cart);
            return 5;
        }
        std::printf("setup=%s value=%d\n", setup_expression, setup_value);
    }
#else
    (void)setup_expression;
#endif
    std::printf("cart=%s fps=%d lua=%zu frames=%u seed=%u draw=%d\n", path,
                p8p_runtime_target_fps(runtime), cart.lua_size, frames, seed,
                draw_frame);
    lua_service_hooks = 0;
    p8p_runtime_set_service_hook(runtime, count_lua_service_hook, nullptr);
    for (unsigned frame = 0; frame < frames; ++frame) {
        uint8_t buttons = setup_expression ? 0 : scripted_input(frame, &seed);
        auto start = std::chrono::steady_clock::now();
        int result = p8p_runtime_step_with_draw(runtime, buttons, draw_frame);
        auto end = std::chrono::steady_clock::now();
        uint64_t frame_ns = (uint64_t)std::chrono::duration_cast<
            std::chrono::nanoseconds>(end - start).count();
        elapsed_ns += frame_ns;
        if (frame_ns > slowest_ns)
            slowest_ns = frame_ns;
        if (result != 0) {
            std::fprintf(stderr, "frame=%u buttons=%02x error=%s\n", frame,
                         buttons, p8p_runtime_error(runtime));
            p8p_runtime_destroy(runtime);
            p8p_cart_destroy(&cart);
            return 4;
        }
        if ((frame + 1) % 100000u == 0)
            std::printf("frame=%u hash=%08x avg=%.3fms max=%.3fms\n",
                        frame + 1, framebuffer_hash(runtime),
                        (double)elapsed_ns / (double)(frame + 1) / 1000000.0,
                        (double)slowest_ns / 1000000.0);
    }
    std::printf("ok hash=%08x avg=%.3fms max=%.3fms "
                "lua_hooks=%llu lua_kins/frame=%.1f\n",
                framebuffer_hash(runtime),
                frames ? (double)elapsed_ns / (double)frames / 1000000.0 : 0.0,
                (double)slowest_ns / 1000000.0,
                (unsigned long long)lua_service_hooks,
                frames ? (double)lua_service_hooks * 8.192 / (double)frames : 0.0);
    p8p_runtime_destroy(runtime);
    p8p_cart_destroy(&cart);
    return 0;
}

static int run_input_probe(const char *path) {
    p8p_cart_t cart = {};
    if (p8p_cart_load_file(path, &cart) != 0) {
        std::fprintf(stderr, "cannot load %s\n", path);
        return 2;
    }
    for (int button = 0; button < 6; ++button) {
        p8p_runtime_t *control = p8p_runtime_create();
        p8p_runtime_t *probe = p8p_runtime_create();
        if (!control || !probe || p8p_runtime_load(control, &cart) != 0 ||
            p8p_runtime_load(probe, &cart) != 0) {
            std::fprintf(stderr, "probe load failed\n");
            p8p_runtime_destroy(control);
            p8p_runtime_destroy(probe);
            p8p_cart_destroy(&cart);
            return 3;
        }
        int first_difference = -1;
        int different_frames = 0;
        for (unsigned frame = 0; frame < 500; ++frame) {
            uint8_t shared = 0;
            uint8_t tested = shared;
            if (button < 4)
                tested |= ((frame / 30u) & 1u) ?
                          (uint8_t)(1u << button) : 0;
            else if (frame % 45u == 0)
                tested |= (uint8_t)(1u << button);
            if (p8p_runtime_step(control, shared) != 0 ||
                p8p_runtime_step(probe, tested) != 0) {
                std::fprintf(stderr, "probe button=%d frame=%u error=%s / %s\n",
                             button, frame, p8p_runtime_error(control),
                             p8p_runtime_error(probe));
                p8p_runtime_destroy(control);
                p8p_runtime_destroy(probe);
                p8p_cart_destroy(&cart);
                return 4;
            }
            if (framebuffer_hash(control) != framebuffer_hash(probe)) {
                if (first_difference < 0)
                    first_difference = (int)frame;
                ++different_frames;
            }
        }
        std::printf("button=%d first_difference=%d different_frames=%d\n",
                    button, first_difference, different_frames);
        p8p_runtime_destroy(control);
        p8p_runtime_destroy(probe);
    }
    p8p_cart_destroy(&cart);
    return 0;
}

#ifdef P8P_RUNTIME_DEBUG
static int run_eval(const char *path, unsigned frames, uint8_t buttons,
                    const char *expression) {
    p8p_cart_t cart = {};
    p8p_runtime_t *runtime = nullptr;
    if (p8p_cart_load_file(path, &cart) != 0)
        return 2;
    runtime = p8p_runtime_create();
    if (!runtime || p8p_runtime_load(runtime, &cart) != 0) {
        std::fprintf(stderr, "load error: %s\n",
                     runtime ? p8p_runtime_error(runtime) : "out of memory");
        p8p_runtime_destroy(runtime);
        p8p_cart_destroy(&cart);
        return 3;
    }
    for (unsigned frame = 0; frame < frames; ++frame) {
        if (p8p_runtime_step(runtime, buttons) != 0) {
            std::fprintf(stderr, "frame %u: %s\n", frame,
                         p8p_runtime_error(runtime));
            p8p_runtime_destroy(runtime);
            p8p_cart_destroy(&cart);
            return 4;
        }
    }
    int value = 0;
    int result = p8p_runtime_debug_eval_int(runtime, expression, &value);
    std::printf("eval result=%d value=%d expression=%s\n", result, value,
                expression);
    p8p_runtime_destroy(runtime);
    p8p_cart_destroy(&cart);
    return result == 0 ? 0 : 5;
}
#endif

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s CART [FRAMES [SEED [nodraw]]] | "
                     "CART --bench EXPR FRAMES [SEED [nodraw]] | "
                     "CART --probe\n",
                     argv[0]);
        return 1;
    }
    if (argc >= 3 && std::strcmp(argv[2], "--probe") == 0)
        return run_input_probe(argv[1]);
#ifdef P8P_RUNTIME_DEBUG
    if (argc >= 6 && std::strcmp(argv[2], "--eval") == 0)
        return run_eval(argv[1],
                        (unsigned)std::strtoul(argv[3], nullptr, 0),
                        (uint8_t)std::strtoul(argv[4], nullptr, 0), argv[5]);
#endif
    if (argc >= 3 && std::strcmp(argv[2], "--source") == 0) {
        p8p_cart_t cart = {};
        if (p8p_cart_load_file(argv[1], &cart) != 0)
            return 2;
        std::fwrite(cart.lua, 1, cart.lua_size, stdout);
        p8p_cart_destroy(&cart);
        return 0;
    }
#ifdef P8P_RUNTIME_DEBUG
    if (argc >= 5 && std::strcmp(argv[2], "--bench") == 0) {
        unsigned frames = (unsigned)std::strtoul(argv[4], nullptr, 0);
        uint32_t seed = argc >= 6 ?
            (uint32_t)std::strtoul(argv[5], nullptr, 0) : 0x12345678u;
        int draw_frame = !(argc >= 7 &&
                           std::strcmp(argv[6], "nodraw") == 0);
        return run_scan(argv[1], frames, seed, draw_frame, argv[3]);
    }
#endif
    unsigned frames = argc >= 3 ? (unsigned)std::strtoul(argv[2], nullptr, 0) :
                                  100000u;
    uint32_t seed = argc >= 4 ? (uint32_t)std::strtoul(argv[3], nullptr, 0) :
                                0x12345678u;
    int draw_frame = !(argc >= 5 && std::strcmp(argv[4], "nodraw") == 0);
    return run_scan(argv[1], frames, seed, draw_frame, nullptr);
}
