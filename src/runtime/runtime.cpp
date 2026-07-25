#include "p8p/runtime.h"
#include "p8p/audio.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <fix32.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__riscv)
#define P8P_FASTTEXT __attribute__((section(".app_fasttext"), noinline, \
                                   optimize("align-functions=4", \
                                            "align-loops=4")))
#else
#define P8P_FASTTEXT
#endif

using z8::fix32;

struct p8p_runtime {
    lua_State *lua;
    p8p_audio_t *audio;
    char *cart_lua;
    size_t cart_lua_size;
    uint8_t ram[0x10000];
    uint8_t cart_rom[P8P_CART_ROM_SIZE];
    /* One byte per logical pixel is the fast drawing surface.  PICO-8's
     * packed 4-bpp screen RAM is synchronized only when a memory API touches
     * it, avoiding a read-modify-write for every rendered pixel. */
    uint8_t framebuffer[128 * 128];
    uint8_t screen_ram_dirty;
    uint8_t draw_palette[16];
    uint8_t screen_palette[16];
    uint8_t transparent[16];
    /* Derived fast-path state.  These flags are deliberately not serialized:
     * they are cheap to rebuild after loading a state or draw-state RAM. */
    uint8_t palettes_default;
    uint8_t transparency_default;
    uint8_t buttons;
    uint8_t previous_buttons;
    uint16_t held_frames[7];
    uint32_t rng[2];
    int camera_x;
    int camera_y;
    int clip_x0;
    int clip_y0;
    int clip_x1;
    int clip_y1;
    int draw_color;
    uint16_t fill_pattern;
    uint8_t fill_pattern_transparent;
    int cursor_x;
    int cursor_y;
    int target_fps;
    uint8_t draw_frame;
    uint32_t frame_count;
    int persist_ref;
    int restore_ref;
    int cart_thread_ref;
    lua_State *cart_thread;
    uint8_t cart_thread_active;
    uint8_t cart_thread_kind;
    uint8_t restart_requested;
    p8p_runtime_service_fn service_hook;
    void *service_userdata;
    p8p_runtime_profile_fn profile_hook;
    void *profile_userdata;
    uint32_t api_profile_calls[P8P_API_CATEGORY_COUNT];
    uint16_t cart_instruction_slices;
    p8p_runtime_cartdata_load_fn cartdata_load;
    p8p_runtime_cartdata_save_fn cartdata_save;
    void *cartdata_userdata;
    char cartdata_id[65];
    uint8_t cartdata_active;
    uint8_t cartdata_dirty;
    char error[256];
};

struct p8p_runtime_state_header {
    uint8_t magic[8];
    uint32_t version;
    uint32_t fixed_size;
    uint32_t audio_size;
    uint32_t lua_size;
};

struct p8p_runtime_fixed_state {
    uint8_t ram[0x10000];
    uint8_t framebuffer[128 * 128];
    uint8_t screen_ram_dirty;
    uint8_t draw_palette[16];
    uint8_t screen_palette[16];
    uint8_t transparent[16];
    uint8_t buttons;
    uint8_t previous_buttons;
    uint16_t held_frames[6];
    uint32_t rng[2];
    int32_t camera_x;
    int32_t camera_y;
    int32_t clip_x0;
    int32_t clip_y0;
    int32_t clip_x1;
    int32_t clip_y1;
    int32_t draw_color;
    int32_t cursor_x;
    int32_t cursor_y;
    int32_t target_fps;
    uint32_t frame_count;
};

static const uint8_t runtime_state_magic[8] = {
    'P', '8', 'P', 'S', 'T', 'A', 'T', 'E'
};

static p8p_runtime_t *active_runtime;

static void profile_api(p8p_runtime_api_category_t category) {
    if (active_runtime && active_runtime->profile_hook &&
        (unsigned)category < P8P_API_CATEGORY_COUNT)
        ++active_runtime->api_profile_calls[category];
}

static void update_palette_default_flags(p8p_runtime_t *runtime) {
    int palettes_default = 1;
    int transparency_default = runtime->transparent[0] != 0;
    for (int i = 0; i < 16; ++i) {
        if (runtime->draw_palette[i] != i ||
            runtime->screen_palette[i] != i)
            palettes_default = 0;
        if (i && runtime->transparent[i])
            transparency_default = 0;
    }
    runtime->palettes_default = (uint8_t)palettes_default;
    runtime->transparency_default = (uint8_t)transparency_default;
}

static void cartdata_flush(p8p_runtime_t *runtime) {
    if (runtime && runtime->cartdata_active && runtime->cartdata_dirty &&
        runtime->cartdata_save &&
        runtime->cartdata_save(runtime->cartdata_userdata,
                               runtime->cartdata_id,
                               runtime->ram + 0x5e00, 0x100) == 0)
        runtime->cartdata_dirty = 0;
}

static void cartdata_mark_dirty(p8p_runtime_t *runtime) {
    if (runtime && runtime->cartdata_active)
        runtime->cartdata_dirty = 1;
}

static void runtime_service_lua_hook(lua_State *lua, lua_Debug *) {
    if (active_runtime && active_runtime->service_hook)
        active_runtime->service_hook(active_runtime->service_userdata);
    /* Top-level and _init code is allowed to be written as a permanent loop.
     * flip() normally supplies the cooperative boundary, but a few real carts
     * intentionally omit it on some paths.  Yield after a bounded instruction
     * slice so one such path cannot lock the Pocket forever. */
    if (active_runtime && lua == active_runtime->cart_thread &&
        ++active_runtime->cart_instruction_slices >= 16)
        lua_yield(lua, 0);
}

static void install_service_hook(p8p_runtime_t *runtime) {
    if (!runtime || !runtime->lua)
        return;
    if (runtime->service_hook)
        lua_sethook(runtime->lua, runtime_service_lua_hook,
                    LUA_MASKCOUNT, 8192);
    else
        lua_sethook(runtime->lua, NULL, 0, 0);
    if (runtime->cart_thread)
        lua_sethook(runtime->cart_thread, runtime_service_lua_hook,
                    LUA_MASKCOUNT, 8192);
}

static int32_t arg_int(lua_State *lua, int index, int32_t fallback) {
    if (lua_gettop(lua) < index || lua_isnil(lua, index))
        return fallback;
    return (int32_t)lua_tonumber(lua, index);
}

static fix32 arg_number(lua_State *lua, int index, fix32 fallback) {
    if (lua_gettop(lua) < index || lua_isnil(lua, index))
        return fallback;
    return lua_tonumber(lua, index);
}

static void push_int(lua_State *lua, int32_t value) {
    lua_pushnumber(lua, fix32(value));
}

static int in_clip(const p8p_runtime_t *runtime, int x, int y) {
    return x >= runtime->clip_x0 && x < runtime->clip_x1 &&
           y >= runtime->clip_y0 && y < runtime->clip_y1;
}

static void screen_to_ram(p8p_runtime_t *runtime) {
    if (!runtime->screen_ram_dirty)
        return;
    for (int y = 0; y < 128; ++y) {
        const uint8_t *source = runtime->framebuffer + y * 128;
        uint8_t *destination = runtime->ram + 0x6000 + y * 64;
        for (int x = 0; x < 64; ++x)
            destination[x] = (uint8_t)((source[x * 2] & 15) |
                                      ((source[x * 2 + 1] & 15) << 4));
    }
    runtime->screen_ram_dirty = 0;
}

static void ram_to_screen(p8p_runtime_t *runtime) {
    for (int y = 0; y < 128; ++y) {
        const uint8_t *source = runtime->ram + 0x6000 + y * 64;
        uint16_t *destination =
            (uint16_t *)(runtime->framebuffer + y * 128);
        for (int x = 0; x < 64; ++x) {
            uint8_t packed = source[x];
            destination[x] = (uint16_t)((packed & 15) |
                                       ((uint16_t)(packed >> 4) << 8));
        }
    }
    runtime->screen_ram_dirty = 0;
}

static int range_touches_screen(int address, int length) {
    int end;
    if (length <= 0)
        return 0;
    address &= 0xffff;
    end = address + length;
    if (end <= 0x10000)
        return address < 0x8000 && end > 0x6000;
    return (address < 0x8000) || ((end - 0x10000) > 0x6000);
}

static int range_touches_cartdata(int address, int length) {
    if (length <= 0)
        return 0;
    return address < 0x5f00 && address + length > 0x5e00;
}

static int range_touches_draw_state(int address, int length) {
    return length > 0 && address < 0x5f40 && address + length > 0x5f00;
}

static void draw_state_to_ram(p8p_runtime_t *runtime) {
    for (int i = 0; i < 16; ++i) {
        runtime->ram[0x5f00 + i] = (uint8_t)(runtime->draw_palette[i] |
            (runtime->transparent[i] ? 0x10 : 0));
        runtime->ram[0x5f10 + i] = runtime->screen_palette[i];
    }
    runtime->ram[0x5f20] = (uint8_t)runtime->clip_x0;
    runtime->ram[0x5f21] = (uint8_t)runtime->clip_y0;
    runtime->ram[0x5f22] = (uint8_t)runtime->clip_x1;
    runtime->ram[0x5f23] = (uint8_t)runtime->clip_y1;
    runtime->ram[0x5f25] = (uint8_t)runtime->draw_color;
    runtime->ram[0x5f26] = (uint8_t)runtime->cursor_x;
    runtime->ram[0x5f27] = (uint8_t)runtime->cursor_y;
    runtime->ram[0x5f28] = (uint8_t)runtime->camera_x;
    runtime->ram[0x5f29] = (uint8_t)(runtime->camera_x >> 8);
    runtime->ram[0x5f2a] = (uint8_t)runtime->camera_y;
    runtime->ram[0x5f2b] = (uint8_t)(runtime->camera_y >> 8);
    runtime->ram[0x5f31] = (uint8_t)runtime->fill_pattern;
    runtime->ram[0x5f32] = (uint8_t)(runtime->fill_pattern >> 8);
    runtime->ram[0x5f33] = runtime->fill_pattern_transparent;
}

static void palette_entry_to_ram(p8p_runtime_t *runtime, int color) {
    runtime->ram[0x5f00 + color] =
        (uint8_t)(runtime->draw_palette[color] |
                  (runtime->transparent[color] ? 0x10 : 0));
    runtime->ram[0x5f10 + color] = runtime->screen_palette[color];
}

static void cursor_to_ram(p8p_runtime_t *runtime) {
    runtime->ram[0x5f26] = (uint8_t)runtime->cursor_x;
    runtime->ram[0x5f27] = (uint8_t)runtime->cursor_y;
}

static void camera_to_ram(p8p_runtime_t *runtime) {
    runtime->ram[0x5f28] = (uint8_t)runtime->camera_x;
    runtime->ram[0x5f29] = (uint8_t)(runtime->camera_x >> 8);
    runtime->ram[0x5f2a] = (uint8_t)runtime->camera_y;
    runtime->ram[0x5f2b] = (uint8_t)(runtime->camera_y >> 8);
}

static void draw_state_from_ram(p8p_runtime_t *runtime) {
    for (int i = 0; i < 16; ++i) {
        runtime->draw_palette[i] = runtime->ram[0x5f00 + i] & 15;
        runtime->transparent[i] =
            (uint8_t)((runtime->ram[0x5f00 + i] & 0x10) != 0);
        runtime->screen_palette[i] = runtime->ram[0x5f10 + i] & 0x8f;
    }
    update_palette_default_flags(runtime);
    runtime->clip_x0 = runtime->ram[0x5f20];
    runtime->clip_y0 = runtime->ram[0x5f21];
    runtime->clip_x1 = runtime->ram[0x5f22];
    runtime->clip_y1 = runtime->ram[0x5f23];
    runtime->draw_color = runtime->ram[0x5f25];
    runtime->cursor_x = runtime->ram[0x5f26];
    runtime->cursor_y = runtime->ram[0x5f27];
    runtime->camera_x = (int16_t)(runtime->ram[0x5f28] |
        ((uint16_t)runtime->ram[0x5f29] << 8));
    runtime->camera_y = (int16_t)(runtime->ram[0x5f2a] |
        ((uint16_t)runtime->ram[0x5f2b] << 8));
    runtime->fill_pattern = (uint16_t)(runtime->ram[0x5f31] |
        ((uint16_t)runtime->ram[0x5f32] << 8));
    runtime->fill_pattern_transparent = runtime->ram[0x5f33] & 1;
}

static uint8_t screen_get(const p8p_runtime_t *runtime, int x, int y) {
    if ((unsigned)x >= 128u || (unsigned)y >= 128u)
        return 0;
    return runtime->framebuffer[y * 128 + x];
}

static void screen_set_raw(p8p_runtime_t *runtime, int x, int y, uint8_t color) {
    if ((unsigned)x >= 128u || (unsigned)y >= 128u || !in_clip(runtime, x, y))
        return;
    runtime->framebuffer[y * 128 + x] = color & 15;
    runtime->screen_ram_dirty = 1;
}

static void screen_set_unchecked(p8p_runtime_t *runtime, int x, int y,
                                 uint8_t color) {
    runtime->framebuffer[y * 128 + x] = color & 15;
    runtime->screen_ram_dirty = 1;
}

static void screen_set(p8p_runtime_t *runtime, int x, int y, int color) {
    int screen_x = x - runtime->camera_x;
    int screen_y = y - runtime->camera_y;
    int selected = color & 0x0f;
    if (!runtime->fill_pattern) {
        screen_set_raw(runtime, screen_x, screen_y,
                       runtime->draw_palette[selected]);
        return;
    }
    int bit = 15 - ((screen_x & 3) + 4 * (screen_y & 3));
    if ((runtime->fill_pattern >> bit) & 1) {
        if (runtime->fill_pattern_transparent)
            return;
        selected = (color >> 4) & 0x0f;
    }
    screen_set_raw(runtime, screen_x, screen_y,
                   runtime->draw_palette[selected]);
}

static uint8_t sprite_get(const p8p_runtime_t *runtime, int x, int y) {
    uint8_t packed;
    if ((unsigned)x >= 128u || (unsigned)y >= 128u)
        return 0;
    int base = (int)runtime->ram[0x5f54] << 8;
    int address = base + y * 64 + x / 2;
    if (address < 0 || address >= 0x10000)
        return 0;
    packed = runtime->ram[address];
    return (uint8_t)((x & 1) ? packed >> 4 : packed & 0x0f);
}

static uint8_t sprite_get_at_base(const p8p_runtime_t *runtime, int base,
                                  int x, int y) {
    if ((unsigned)x >= 128u || (unsigned)y >= 128u)
        return 0;
    int address = base + y * 64 + x / 2;
    if ((unsigned)address >= 0x10000u)
        return 0;
    uint8_t packed = runtime->ram[address];
    return (uint8_t)((x & 1) ? packed >> 4 : packed & 0x0f);
}

static void sprite_set(p8p_runtime_t *runtime, int x, int y, uint8_t color) {
    uint8_t *packed;
    if ((unsigned)x >= 128u || (unsigned)y >= 128u)
        return;
    int base = (int)runtime->ram[0x5f54] << 8;
    int address = base + y * 64 + x / 2;
    if (address < 0 || address >= 0x10000)
        return;
    packed = &runtime->ram[address];
    if (x & 1)
        *packed = (uint8_t)((*packed & 0x0f) | ((color & 0x0f) << 4));
    else
        *packed = (uint8_t)((*packed & 0xf0) | (color & 0x0f));
}

static uint8_t map_get(const p8p_runtime_t *runtime, int x, int y) {
    int width = runtime->ram[0x5f57] ? runtime->ram[0x5f57] : 256;
    int mapping = runtime->ram[0x5f56];
    int size = mapping >= 0x80 ? 0x10000 - (mapping << 8) : 8192;
    int height = size / width;
    if (x < 0 || y < 0 || x >= width || y >= height)
        return 0;
    int index = y * width + x;
    if (mapping >= 0x80)
        return runtime->ram[(mapping << 8) + index];
    return index < 4096 ? runtime->ram[0x2000 + index]
                        : runtime->ram[index];
}

static void map_set(p8p_runtime_t *runtime, int x, int y, uint8_t value) {
    int width = runtime->ram[0x5f57] ? runtime->ram[0x5f57] : 256;
    int mapping = runtime->ram[0x5f56];
    int size = mapping >= 0x80 ? 0x10000 - (mapping << 8) : 8192;
    int height = size / width;
    if (x < 0 || y < 0 || x >= width || y >= height)
        return;
    int index = y * width + x;
    if (mapping >= 0x80)
        runtime->ram[(mapping << 8) + index] = value;
    else if (index < 4096)
        runtime->ram[0x2000 + index] = value;
    else
        runtime->ram[index] = value;
}

static void draw_line(p8p_runtime_t *runtime, int x0, int y0, int x1, int y1,
                      int color) {
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        screen_set(runtime, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int twice_error = error * 2;
        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static P8P_FASTTEXT void draw_sprite(p8p_runtime_t *runtime, int sprite, int x,
                                     int y, int width, int height, int flip_x,
                                     int flip_y) {
    profile_api(P8P_API_SPRITE);
    int source_x = (sprite & 15) * 8;
    int source_y = (sprite >> 4) * 8;
    int pixel_width = width * 8;
    int pixel_height = height * 8;
    int sprite_base = (int)runtime->ram[0x5f54] << 8;

    int destination_x = x - runtime->camera_x;
    int destination_y = y - runtime->camera_y;
    int start_x = destination_x < runtime->clip_x0 ?
        runtime->clip_x0 - destination_x : 0;
    int start_y = destination_y < runtime->clip_y0 ?
        runtime->clip_y0 - destination_y : 0;
    int end_x = pixel_width;
    int end_y = pixel_height;
    if (destination_x + end_x > runtime->clip_x1)
        end_x = runtime->clip_x1 - destination_x;
    if (destination_y + end_y > runtime->clip_y1)
        end_y = runtime->clip_y1 - destination_y;
    if (start_x >= end_x || start_y >= end_y)
        return;

    int source_in_bounds = source_x >= 0 && source_y >= 0 &&
        source_x + pixel_width <= 128 && source_y + pixel_height <= 128 &&
        sprite_base >= 0 && sprite_base + 8192 <= 0x10000;
    runtime->screen_ram_dirty = 1;
    for (int py = start_y; py < end_y; ++py) {
        int sy = flip_y ? pixel_height - 1 - py : py;
        const uint8_t *source_row = source_in_bounds ?
            runtime->ram + sprite_base + (source_y + sy) * 64 : NULL;
        uint8_t *destination_row = runtime->framebuffer +
            (destination_y + py) * 128 + destination_x;
        for (int px = start_x; px < end_x; ++px) {
            int sx = flip_x ? pixel_width - 1 - px : px;
            int sample_x = source_x + sx;
            uint8_t color;
            if (source_row) {
                uint8_t packed = source_row[sample_x >> 1];
                color = (uint8_t)((sample_x & 1) ? packed >> 4 : packed & 15);
            } else {
                color = sprite_get_at_base(runtime, sprite_base, sample_x,
                                           source_y + sy);
            }
            if (!runtime->transparent[color])
                destination_row[px] = runtime->draw_palette[color] & 15;
        }
    }
}

static P8P_FASTTEXT void draw_hspan(p8p_runtime_t *runtime, int x0, int x1,
                                    int y, int color) {
    x0 -= runtime->camera_x;
    x1 -= runtime->camera_x;
    y -= runtime->camera_y;
    if (x0 > x1) { int temporary = x0; x0 = x1; x1 = temporary; }
    if (y < runtime->clip_y0 || y >= runtime->clip_y1 ||
        x1 < runtime->clip_x0 || x0 >= runtime->clip_x1)
        return;
    if (x0 < runtime->clip_x0) x0 = runtime->clip_x0;
    if (x1 >= runtime->clip_x1) x1 = runtime->clip_x1 - 1;

    if (!runtime->fill_pattern) {
        uint8_t mapped = runtime->draw_palette[color & 15] & 15;
        memset(runtime->framebuffer + y * 128 + x0, mapped,
               (size_t)(x1 - x0 + 1));
        runtime->screen_ram_dirty = 1;
    } else {
        for (int screen_x = x0; screen_x <= x1; ++screen_x)
            screen_set(runtime, screen_x + runtime->camera_x,
                       y + runtime->camera_y, color);
    }
}

static int api_cls(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    uint8_t color = runtime->draw_palette[arg_int(lua, 1, 0) & 15];
    memset(runtime->framebuffer, color & 15, sizeof(runtime->framebuffer));
    runtime->screen_ram_dirty = 1;
    runtime->cursor_x = 0;
    runtime->cursor_y = 0;
    cursor_to_ram(runtime);
    return 0;
}

static int api_pset(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    screen_set(runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
               arg_int(lua, 3, runtime->draw_color));
    return 0;
}

static int api_pget(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    int x = arg_int(lua, 1, 0) - active_runtime->camera_x;
    int y = arg_int(lua, 2, 0) - active_runtime->camera_y;
    push_int(lua, screen_get(active_runtime, x, y));
    return 1;
}

static int api_sget(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    push_int(lua, sprite_get(active_runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0)));
    return 1;
}

static int api_sset(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    sprite_set(active_runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
               (uint8_t)arg_int(lua, 3, 0));
    return 0;
}

static int api_color(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    int previous = active_runtime->draw_color;
    if (lua_gettop(lua) >= 1)
        active_runtime->draw_color = arg_int(lua, 1, 6) & 255;
    active_runtime->ram[0x5f25] = (uint8_t)active_runtime->draw_color;
    push_int(lua, previous);
    return 1;
}

static int api_line(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    int x0 = arg_int(lua, 1, 0);
    int y0 = arg_int(lua, 2, 0);
    int x1 = arg_int(lua, 3, x0);
    int y1 = arg_int(lua, 4, y0);
    int color = arg_int(lua, 5, runtime->draw_color);
    draw_line(runtime, x0, y0, x1, y1, color);
    return 0;
}

static int api_rectfill(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    int x0 = arg_int(lua, 1, 0);
    int y0 = arg_int(lua, 2, 0);
    int x1 = arg_int(lua, 3, x0);
    int y1 = arg_int(lua, 4, y0);
    int color = arg_int(lua, 5, runtime->draw_color);
    if (y0 > y1) { int temp = y0; y0 = y1; y1 = temp; }

    /* Clamp in world coordinates before walking the rows.  Real cartridges
     * use rectfill() for very large ground/water planes (UFO draws down to
     * y=576); calling draw_hspan hundreds of times for rows which are wholly
     * outside the 128-pixel clip is pure overhead on Pocket. */
    int visible_y0 = runtime->camera_y + runtime->clip_y0;
    int visible_y1 = runtime->camera_y + runtime->clip_y1 - 1;
    if (y0 < visible_y0) y0 = visible_y0;
    if (y1 > visible_y1) y1 = visible_y1;
    if (y0 > y1)
        return 0;
    for (int y = y0; y <= y1; ++y)
        draw_hspan(runtime, x0, x1, y, color);
    return 0;
}

static int api_rect(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    int x0 = arg_int(lua, 1, 0);
    int y0 = arg_int(lua, 2, 0);
    int x1 = arg_int(lua, 3, x0);
    int y1 = arg_int(lua, 4, y0);
    int color = arg_int(lua, 5, runtime->draw_color);
    draw_line(runtime, x0, y0, x1, y0, color);
    draw_line(runtime, x1, y0, x1, y1, color);
    draw_line(runtime, x1, y1, x0, y1, color);
    draw_line(runtime, x0, y1, x0, y0, color);
    return 0;
}

static int api_circfill(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    int cx = arg_int(lua, 1, 0);
    int cy = arg_int(lua, 2, 0);
    int radius = arg_int(lua, 3, 4);
    int color = arg_int(lua, 4, runtime->draw_color);
    if (radius < 0)
        return 0;
    int extent = radius;
    int radius_squared = radius * radius;
    for (int offset_y = 0; offset_y <= radius; ++offset_y) {
        while (extent * extent + offset_y * offset_y > radius_squared)
            --extent;
        draw_hspan(runtime, cx - extent, cx + extent, cy - offset_y, color);
        if (offset_y)
            draw_hspan(runtime, cx - extent, cx + extent, cy + offset_y, color);
    }
    return 0;
}

static int api_circ(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    p8p_runtime_t *runtime = active_runtime;
    int cx = arg_int(lua, 1, 0);
    int cy = arg_int(lua, 2, 0);
    int radius = arg_int(lua, 3, 4);
    int color = arg_int(lua, 4, runtime->draw_color);
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        screen_set(runtime, cx + x, cy + y, color);
        screen_set(runtime, cx + y, cy + x, color);
        screen_set(runtime, cx - y, cy + x, color);
        screen_set(runtime, cx - x, cy + y, color);
        screen_set(runtime, cx - x, cy - y, color);
        screen_set(runtime, cx - y, cy - x, color);
        screen_set(runtime, cx + y, cy - x, color);
        screen_set(runtime, cx + x, cy - y, color);
        ++y;
        if (error < 0)
            error += 2 * y + 1;
        else {
            --x;
            error += 2 * (y - x) + 1;
        }
    }
    return 0;
}

static int api_spr(lua_State *lua) {
    draw_sprite(active_runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
                arg_int(lua, 3, 0), arg_int(lua, 4, 1), arg_int(lua, 5, 1),
                lua_toboolean(lua, 6), lua_toboolean(lua, 7));
    return 0;
}

static int api_sspr(lua_State *lua) {
    profile_api(P8P_API_SPRITE);
    p8p_runtime_t *runtime = active_runtime;
    int source_x = arg_int(lua, 1, 0);
    int source_y = arg_int(lua, 2, 0);
    int source_w = arg_int(lua, 3, 0);
    int source_h = arg_int(lua, 4, 0);
    int destination_x = arg_int(lua, 5, 0);
    int destination_y = arg_int(lua, 6, 0);
    int destination_w = arg_int(lua, 7, source_w);
    int destination_h = arg_int(lua, 8, source_h);
    int flip_x = lua_toboolean(lua, 9);
    int flip_y = lua_toboolean(lua, 10);
    if (!source_w || !source_h || !destination_w || !destination_h)
        return 0;
    if (destination_w < 0) {
        destination_x += destination_w;
        destination_w = -destination_w;
        flip_x = !flip_x;
    }
    if (destination_h < 0) {
        destination_y += destination_h;
        destination_h = -destination_h;
        flip_y = !flip_y;
    }
    int screen_x = destination_x - runtime->camera_x;
    int screen_y = destination_y - runtime->camera_y;
    int start_x = screen_x < runtime->clip_x0 ?
        runtime->clip_x0 - screen_x : 0;
    int start_y = screen_y < runtime->clip_y0 ?
        runtime->clip_y0 - screen_y : 0;
    int end_x = destination_w;
    int end_y = destination_h;
    if (screen_x + end_x > runtime->clip_x1)
        end_x = runtime->clip_x1 - screen_x;
    if (screen_y + end_y > runtime->clip_y1)
        end_y = runtime->clip_y1 - screen_y;
    if (start_x >= end_x || start_y >= end_y)
        return 0;

    int sprite_base = (int)runtime->ram[0x5f54] << 8;
    int sy_numerator = start_y * source_h;
    int sy = sy_numerator / destination_h;
    int sy_error = sy_numerator % destination_h;
    for (int dy = start_y; dy < end_y; ++dy) {
        int sample_y = flip_y ? source_h - 1 - sy : sy;
        int sx_numerator = start_x * source_w;
        int sx = sx_numerator / destination_w;
        int sx_error = sx_numerator % destination_w;
        for (int dx = start_x; dx < end_x; ++dx) {
            int sample_x = flip_x ? source_w - 1 - sx : sx;
            uint8_t color = sprite_get_at_base(
                runtime, sprite_base, source_x + sample_x,
                source_y + sample_y);
            if (!runtime->transparent[color])
                screen_set_unchecked(runtime, screen_x + dx, screen_y + dy,
                                     runtime->draw_palette[color]);
            sx_error += source_w;
            while (sx_error >= destination_w) {
                sx_error -= destination_w;
                ++sx;
            }
        }
        sy_error += source_h;
        while (sy_error >= destination_h) {
            sy_error -= destination_h;
            ++sy;
        }
    }
    return 0;
}

static void draw_oval(p8p_runtime_t *runtime, int x0, int y0, int x1, int y1,
                      int color, int filled) {
    if (x0 > x1) { int temporary = x0; x0 = x1; x1 = temporary; }
    if (y0 > y1) { int temporary = y0; y0 = y1; y1 = temporary; }
    int a = x1 - x0;
    int b = y1 - y0;
    if (!a || !b) {
        draw_line(runtime, x0, y0, x1, y1, color);
        return;
    }
    int odd = b & 1;
    int dx = 4 * (1 - a) * b * b;
    int dy = 4 * (odd + 1) * a * a;
    int error = dx + dy + odd * a * a;
    y0 += (b + 1) / 2;
    y1 = y0 - odd;
    a = 8 * a * a;
    odd = 8 * b * b;

    do {
        if (filled) {
            draw_hspan(runtime, x0, x1, y0, color);
            if (y1 != y0) draw_hspan(runtime, x0, x1, y1, color);
        } else {
            screen_set(runtime, x1, y0, color);
            screen_set(runtime, x0, y0, color);
            if (y1 != y0) {
                screen_set(runtime, x0, y1, color);
                screen_set(runtime, x1, y1, color);
            }
        }
        int twice_error = 2 * error;
        if (twice_error <= dy) {
            ++y0;
            --y1;
            error += dy += a;
        }
        if (twice_error >= dx || 2 * error > dy) {
            ++x0;
            --x1;
            error += dx += odd;
        }
    } while (x0 <= x1);

    while (y0 - y1 < b) {
        if (filled) {
            draw_hspan(runtime, x0 - 1, x1 + 1, y0, color);
            draw_hspan(runtime, x0 - 1, x1 + 1, y1, color);
        } else {
            screen_set(runtime, x0 - 1, y0, color);
            screen_set(runtime, x1 + 1, y0, color);
            screen_set(runtime, x0 - 1, y1, color);
            screen_set(runtime, x1 + 1, y1, color);
        }
        ++y0;
        --y1;
    }
}

static int api_oval(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    draw_oval(active_runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
              arg_int(lua, 3, 0), arg_int(lua, 4, 0),
              arg_int(lua, 5, active_runtime->draw_color), 0);
    return 0;
}

static int api_ovalfill(lua_State *lua) {
    profile_api(P8P_API_GRAPHICS);
    draw_oval(active_runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
              arg_int(lua, 3, 0), arg_int(lua, 4, 0),
              arg_int(lua, 5, active_runtime->draw_color), 1);
    return 0;
}

static int api_tline(lua_State *lua) {
    profile_api(P8P_API_SPRITE);
    p8p_runtime_t *runtime = active_runtime;
    int x0 = arg_int(lua, 1, 0);
    int y0 = arg_int(lua, 2, 0);
    int x1 = arg_int(lua, 3, 0);
    int y1 = arg_int(lua, 4, 0);
    fix32 mx = arg_number(lua, 5, fix32(0));
    fix32 my = arg_number(lua, 6, fix32(0));
    fix32 mdx = arg_number(lua, 7, fix32::frombits(0x2000));
    fix32 mdy = arg_number(lua, 8, fix32(0));
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        int32_t mx_bits = mx.bits();
        int32_t my_bits = my.bits();
        int wrap_x = runtime->ram[0x5f38] ? runtime->ram[0x5f38] : 256;
        int wrap_y = runtime->ram[0x5f39] ? runtime->ram[0x5f39] : 256;
        int map_x = (mx_bits >> 16) % wrap_x;
        int map_y = (my_bits >> 16) % wrap_y;
        if (map_x < 0) map_x += wrap_x;
        if (map_y < 0) map_y += wrap_y;
        map_x += runtime->ram[0x5f3a];
        map_y += runtime->ram[0x5f3b];
        int sprite = map_get(runtime, map_x, map_y);
        if (sprite) {
            int source_x = (sprite & 15) * 8 + ((mx_bits >> 13) & 7);
            int source_y = (sprite >> 4) * 8 + ((my_bits >> 13) & 7);
            uint8_t color = sprite_get(runtime, source_x, source_y);
            if (!runtime->transparent[color]) {
                int screen_x = x0 - runtime->camera_x;
                int screen_y = y0 - runtime->camera_y;
                if ((unsigned)screen_x < 128u && (unsigned)screen_y < 128u &&
                    in_clip(runtime, screen_x, screen_y))
                    screen_set_unchecked(runtime, screen_x, screen_y,
                                         runtime->draw_palette[color]);
            }
        }
        if (x0 == x1 && y0 == y1)
            break;
        int twice_error = error * 2;
        if (twice_error >= dy) { error += dy; x0 += sx; }
        if (twice_error <= dx) { error += dx; y0 += sy; }
        mx += mdx;
        my += mdy;
    }
    return 0;
}

static int api_mget(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    /* lua_tonumber(nil) is zero, which is exactly mget()'s fallback.  BAS
     * Escape calls this hundreds of times per frame; avoiding two gettop +
     * nil checks on every collision probe is measurable on the 100 MHz CPU. */
    int x = (int32_t)lua_tonumber(lua, 1);
    int y = (int32_t)lua_tonumber(lua, 2);
    push_int(lua, map_get(active_runtime, x, y));
    return 1;
}

static int api_mset(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    map_set(active_runtime, arg_int(lua, 1, 0), arg_int(lua, 2, 0),
            (uint8_t)arg_int(lua, 3, 0));
    return 0;
}

static int api_fget(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int sprite = (int32_t)lua_tonumber(lua, 1) & 255;
    uint8_t flags = active_runtime->ram[0x3000 + sprite];
    if (lua_gettop(lua) < 2) {
        push_int(lua, flags);
    } else {
        int flag = (int32_t)lua_tonumber(lua, 2) & 7;
        lua_pushboolean(lua, (flags & (1u << flag)) != 0);
    }
    return 1;
}

static int api_fset(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int sprite = arg_int(lua, 1, 0) & 255;
    if (lua_gettop(lua) < 3) {
        active_runtime->ram[0x3000 + sprite] = (uint8_t)arg_int(lua, 2, 0);
    } else {
        int flag = arg_int(lua, 2, 0) & 7;
        if (lua_toboolean(lua, 3))
            active_runtime->ram[0x3000 + sprite] |= (uint8_t)(1u << flag);
        else
            active_runtime->ram[0x3000 + sprite] &= (uint8_t)~(1u << flag);
    }
    return 0;
}

/* map() spends most of its time dispatching hundreds of ordinary 8x8 tiles.
 * Keep that hot path separate from the fully general scaled/flipped sprite
 * renderer so it does not rebuild the same width, height and camera state for
 * every cell. */
static void draw_map_tile(p8p_runtime_t *runtime, int sprite,
                          int screen_x, int screen_y, int sprite_base) {
    profile_api(P8P_API_SPRITE);
    int start_x = screen_x < runtime->clip_x0 ?
        runtime->clip_x0 - screen_x : 0;
    int start_y = screen_y < runtime->clip_y0 ?
        runtime->clip_y0 - screen_y : 0;
    int end_x = screen_x + 8 > runtime->clip_x1 ?
        runtime->clip_x1 - screen_x : 8;
    int end_y = screen_y + 8 > runtime->clip_y1 ?
        runtime->clip_y1 - screen_y : 8;
    if (start_x >= end_x || start_y >= end_y)
        return;

    int source_x = (sprite & 15) * 8;
    int source_y = (sprite >> 4) * 8;
    int default_colors = runtime->palettes_default &&
                         runtime->transparency_default;
    runtime->screen_ram_dirty = 1;
    for (int y = start_y; y < end_y; ++y) {
        const uint8_t *source = runtime->ram + sprite_base +
            (source_y + y) * 64 + source_x / 2;
        uint8_t *destination = runtime->framebuffer +
            (screen_y + y) * 128 + screen_x;
        int x = start_x;
        if (x & 1) {
            uint8_t color = source[x >> 1] >> 4;
            if (default_colors) {
                if (color) destination[x] = color;
            } else if (!runtime->transparent[color]) {
                destination[x] = runtime->draw_palette[color] & 15;
            }
            ++x;
        }
        for (; x + 1 < end_x; x += 2) {
            uint8_t packed = source[x >> 1];
            uint8_t color0 = packed & 15;
            uint8_t color1 = packed >> 4;
            if (default_colors) {
                if (color0) destination[x] = color0;
                if (color1) destination[x + 1] = color1;
            } else {
                if (!runtime->transparent[color0])
                    destination[x] = runtime->draw_palette[color0] & 15;
                if (!runtime->transparent[color1])
                    destination[x + 1] = runtime->draw_palette[color1] & 15;
            }
        }
        if (x < end_x) {
            uint8_t packed = source[x >> 1];
            uint8_t color = (uint8_t)((x & 1) ? packed >> 4 : packed & 15);
            if (default_colors) {
                if (color) destination[x] = color;
            } else if (!runtime->transparent[color]) {
                destination[x] = runtime->draw_palette[color] & 15;
            }
        }
    }
}

static int api_map(lua_State *lua) {
    profile_api(P8P_API_SPRITE);
    p8p_runtime_t *runtime = active_runtime;
    int cell_x = arg_int(lua, 1, 0);
    int cell_y = arg_int(lua, 2, 0);
    int screen_x = arg_int(lua, 3, 0);
    int screen_y = arg_int(lua, 4, 0);
    int cell_w = arg_int(lua, 5, 16);
    int cell_h = arg_int(lua, 6, 16);
    int layer = arg_int(lua, 7, 0);
    int origin_x = screen_x - runtime->camera_x;
    int origin_y = screen_y - runtime->camera_y;
    int first_x = runtime->clip_x0 > origin_x ?
        (runtime->clip_x0 - origin_x) / 8 : 0;
    int first_y = runtime->clip_y0 > origin_y ?
        (runtime->clip_y0 - origin_y) / 8 : 0;
    int visible_w = runtime->clip_x1 - origin_x;
    int visible_h = runtime->clip_y1 - origin_y;
    int end_x = visible_w > 0 ? (visible_w + 7) / 8 : 0;
    int end_y = visible_h > 0 ? (visible_h + 7) / 8 : 0;
    if (first_x < 0) first_x = 0;
    if (first_y < 0) first_y = 0;
    if (end_x > cell_w) end_x = cell_w;
    if (end_y > cell_h) end_y = cell_h;
    int sprite_base = (int)runtime->ram[0x5f54] << 8;
    int fast_sprite_base = sprite_base >= 0 &&
                           sprite_base + 8192 <= 0x10000;
    for (int y = first_y; y < end_y; ++y) {
        for (int x = first_x; x < end_x; ++x) {
            int sprite = map_get(runtime, cell_x + x, cell_y + y);
            if (sprite && (!layer || (runtime->ram[0x3000 + sprite] & layer))) {
                if (fast_sprite_base)
                    draw_map_tile(runtime, sprite, origin_x + x * 8,
                                  origin_y + y * 8, sprite_base);
                else
                    draw_sprite(runtime, sprite, screen_x + x * 8,
                                screen_y + y * 8, 1, 1, 0, 0);
            }
        }
    }
    return 0;
}

static int api_camera(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    int old_x = active_runtime->camera_x;
    int old_y = active_runtime->camera_y;
    active_runtime->camera_x = arg_int(lua, 1, 0);
    active_runtime->camera_y = arg_int(lua, 2, 0);
    camera_to_ram(active_runtime);
    push_int(lua, old_x);
    push_int(lua, old_y);
    return 2;
}

static int api_clip(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    p8p_runtime_t *runtime = active_runtime;
    int old_x0 = runtime->clip_x0;
    int old_x1 = runtime->clip_x1;
    int old_y0 = runtime->clip_y0;
    int old_y1 = runtime->clip_y1;
    if (lua_gettop(lua) == 0) {
        runtime->clip_x0 = runtime->clip_y0 = 0;
        runtime->clip_x1 = runtime->clip_y1 = 128;
    } else {
        int x = arg_int(lua, 1, 0);
        int y = arg_int(lua, 2, 0);
        int width = arg_int(lua, 3, 128);
        int height = arg_int(lua, 4, 128);
        runtime->clip_x0 = x < 0 ? 0 : x;
        runtime->clip_y0 = y < 0 ? 0 : y;
        runtime->clip_x1 = x + width > 128 ? 128 : x + width;
        runtime->clip_y1 = y + height > 128 ? 128 : y + height;
    }
    runtime->ram[0x5f20] = (uint8_t)runtime->clip_x0;
    runtime->ram[0x5f21] = (uint8_t)runtime->clip_y0;
    runtime->ram[0x5f22] = (uint8_t)runtime->clip_x1;
    runtime->ram[0x5f23] = (uint8_t)runtime->clip_y1;
    push_int(lua, old_x0);
    push_int(lua, old_x1);
    push_int(lua, old_y0);
    push_int(lua, old_y1);
    return 4;
}

static int api_pal(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    p8p_runtime_t *runtime = active_runtime;
    if (lua_gettop(lua) == 0) {
        if (runtime->palettes_default)
            return 0;
        for (int i = 0; i < 16; ++i) {
            runtime->draw_palette[i] = (uint8_t)i;
            runtime->screen_palette[i] = (uint8_t)i;
            palette_entry_to_ram(runtime, i);
        }
        runtime->palettes_default = 1;
    } else if (lua_istable(lua, 1)) {
        int screen = arg_int(lua, 2, 0) == 1;
        lua_pushnil(lua);
        while (lua_next(lua, 1) != 0) {
            if (lua_isnumber(lua, -2) && lua_isnumber(lua, -1)) {
                int from = (int)lua_tonumber(lua, -2) & 15;
                int to = (int)lua_tonumber(lua, -1) &
                         (screen ? 0x8f : 15);
                uint8_t *entry = screen ? &runtime->screen_palette[from] :
                                          &runtime->draw_palette[from];
                if (*entry != (uint8_t)to) {
                    *entry = (uint8_t)to;
                    runtime->palettes_default = 0;
                    palette_entry_to_ram(runtime, from);
                }
            }
            lua_pop(lua, 1);
        }
    } else {
        int from = arg_int(lua, 1, 0) & 15;
        int screen = arg_int(lua, 3, 0) == 1;
        int to = arg_int(lua, 2, from) & (screen ? 0x8f : 15);
        int previous = screen ? runtime->screen_palette[from]
                              : runtime->draw_palette[from];
        if (previous != to) {
            if (screen)
                runtime->screen_palette[from] = (uint8_t)to;
            else
                runtime->draw_palette[from] = (uint8_t)to;
            runtime->palettes_default = 0;
            palette_entry_to_ram(runtime, from);
        }
        push_int(lua, previous);
        return 1;
    }
    return 0;
}

static int api_palt(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    p8p_runtime_t *runtime = active_runtime;
    if (lua_gettop(lua) == 0) {
        if (runtime->transparency_default)
            return 0;
        memset(runtime->transparent, 0, sizeof(runtime->transparent));
        runtime->transparent[0] = 1;
        for (int color = 0; color < 16; ++color)
            runtime->ram[0x5f00 + color] = (uint8_t)(
                runtime->draw_palette[color] | (color == 0 ? 0x10 : 0));
        runtime->transparency_default = 1;
    } else {
        int color = arg_int(lua, 1, 0) & 15;
        uint8_t transparent =
            (uint8_t)(lua_gettop(lua) < 2 || lua_toboolean(lua, 2));
        if (runtime->transparent[color] != transparent) {
            runtime->transparent[color] = transparent;
            runtime->transparency_default = 0;
            runtime->ram[0x5f00 + color] = (uint8_t)(
                runtime->draw_palette[color] | (transparent ? 0x10 : 0));
        }
    }
    return 0;
}

static int api_btn(lua_State *lua) {
    profile_api(P8P_API_INPUT);
    if (lua_gettop(lua) == 0) {
        push_int(lua, active_runtime->buttons & 0x7f);
    } else {
        int button = arg_int(lua, 1, 0);
        int player = arg_int(lua, 2, 0);
        lua_pushboolean(lua, button >= 0 && button < 7 &&
                       player == 0 &&
                       (active_runtime->buttons & (1u << button)) != 0);
    }
    return 1;
}

static int api_btnp(lua_State *lua) {
    profile_api(P8P_API_INPUT);
    p8p_runtime_t *runtime = active_runtime;
    if (lua_gettop(lua) == 0) {
        int mask = 0;
        for (int button = 0; button < 7; ++button) {
            uint16_t held = runtime->held_frames[button];
            if (held == 1 || (held > 15 && ((held - 15) & 3) == 0))
                mask |= 1 << button;
        }
        push_int(lua, mask);
        return 1;
    }
    int button = arg_int(lua, 1, 0);
    int player = arg_int(lua, 2, 0);
    int pressed = 0;
    if (player == 0 && button >= 0 && button < 7) {
        uint16_t held = runtime->held_frames[button];
        pressed = held == 1 || (held > 15 && ((held - 15) & 3) == 0);
    }
    lua_pushboolean(lua, pressed);
    return 1;
}

static int api_peek(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0) & 0xffff;
    int count = arg_int(lua, 2, 1);
    if (count < 1) count = 1;
    if (count > 8192) count = 8192;
    if (range_touches_screen(address, count))
        screen_to_ram(active_runtime);
    for (int i = 0; i < count; ++i)
        push_int(lua, active_runtime->ram[(address + i) & 0xffff]);
    return count;
}

static int api_poke(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0) & 0xffff;
    int values = lua_gettop(lua) - 1;
    if (values < 1)
        values = 1;
    int touches_screen = range_touches_screen(address, values);
    if (touches_screen)
        screen_to_ram(active_runtime);
    for (int i = 0; i < values; ++i)
        active_runtime->ram[(address + i) & 0xffff] =
            (uint8_t)arg_int(lua, i + 2, 0);
    if (touches_screen)
        ram_to_screen(active_runtime);
    if (range_touches_draw_state(address, values))
        draw_state_from_ram(active_runtime);
    if (range_touches_cartdata(address, values))
        cartdata_mark_dirty(active_runtime);
    return 0;
}

static int api_peek2(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0);
    if (address < 0 || address > 0xfffe) {
        push_int(lua, 0);
        return 1;
    }
    if (range_touches_screen(address, 2))
        screen_to_ram(active_runtime);
    push_int(lua, active_runtime->ram[address] |
                  ((int)active_runtime->ram[address + 1] << 8));
    return 1;
}

static int api_poke2(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0);
    int value = arg_int(lua, 2, 0);
    if (address < 0 || address > 0xfffe)
        return 0;
    int touches_screen = range_touches_screen(address, 2);
    if (touches_screen) screen_to_ram(active_runtime);
    active_runtime->ram[address] = (uint8_t)value;
    active_runtime->ram[address + 1] = (uint8_t)(value >> 8);
    if (touches_screen) ram_to_screen(active_runtime);
    if (range_touches_draw_state(address, 2))
        draw_state_from_ram(active_runtime);
    if (range_touches_cartdata(address, 2)) cartdata_mark_dirty(active_runtime);
    return 0;
}

static int api_peek4(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0);
    if (address < 0 || address > 0xfffc) {
        lua_pushnumber(lua, fix32(0));
        return 1;
    }
    if (range_touches_screen(address, 4))
        screen_to_ram(active_runtime);
    uint32_t bits = (uint32_t)active_runtime->ram[address] |
        ((uint32_t)active_runtime->ram[address + 1] << 8) |
        ((uint32_t)active_runtime->ram[address + 2] << 16) |
        ((uint32_t)active_runtime->ram[address + 3] << 24);
    lua_pushnumber(lua, fix32::frombits((int32_t)bits));
    return 1;
}

static int api_poke4(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int address = arg_int(lua, 1, 0);
    if (address < 0 || address > 0xfffc)
        return 0;
    uint32_t bits = (uint32_t)arg_number(lua, 2, fix32(0)).bits();
    int touches_screen = range_touches_screen(address, 4);
    if (touches_screen) screen_to_ram(active_runtime);
    active_runtime->ram[address] = (uint8_t)bits;
    active_runtime->ram[address + 1] = (uint8_t)(bits >> 8);
    active_runtime->ram[address + 2] = (uint8_t)(bits >> 16);
    active_runtime->ram[address + 3] = (uint8_t)(bits >> 24);
    if (touches_screen) ram_to_screen(active_runtime);
    if (range_touches_draw_state(address, 4))
        draw_state_from_ram(active_runtime);
    if (range_touches_cartdata(address, 4)) cartdata_mark_dirty(active_runtime);
    return 0;
}

static int api_memset(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int destination = arg_int(lua, 1, 0);
    int value = arg_int(lua, 2, 0);
    int length = arg_int(lua, 3, 0);
    if (destination >= 0 && length >= 0 && destination + length <= 0x10000) {
        int touches_screen = range_touches_screen(destination, length);
        if (touches_screen)
            screen_to_ram(active_runtime);
        memset(active_runtime->ram + destination, value, (size_t)length);
        if (touches_screen)
            ram_to_screen(active_runtime);
        if (range_touches_draw_state(destination, length))
            draw_state_from_ram(active_runtime);
        if (range_touches_cartdata(destination, length))
            cartdata_mark_dirty(active_runtime);
    }
    return 0;
}

static int api_memcpy(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int destination = arg_int(lua, 1, 0);
    int source = arg_int(lua, 2, 0);
    int length = arg_int(lua, 3, 0);
    if (destination >= 0 && source >= 0 && length >= 0 &&
        destination + length <= 0x10000 && source + length <= 0x10000) {
        int reads_screen = range_touches_screen(source, length);
        int writes_screen = range_touches_screen(destination, length);
        if (reads_screen || writes_screen)
            screen_to_ram(active_runtime);
        memmove(active_runtime->ram + destination, active_runtime->ram + source,
                (size_t)length);
        if (writes_screen)
            ram_to_screen(active_runtime);
        if (range_touches_draw_state(destination, length))
            draw_state_from_ram(active_runtime);
        if (range_touches_cartdata(destination, length))
            cartdata_mark_dirty(active_runtime);
    }
    return 0;
}

static int api_reload(lua_State *lua) {
    profile_api(P8P_API_MEMORY);
    int destination = arg_int(lua, 1, 0);
    int source = arg_int(lua, 2, 0);
    int length = arg_int(lua, 3, 0x4300);
    if (destination >= 0 && source >= 0 && length >= 0 &&
        destination + length <= 0x10000 &&
        source + length <= (int)P8P_CART_ROM_SIZE) {
        int writes_screen = range_touches_screen(destination, length);
        if (writes_screen)
            screen_to_ram(active_runtime);
        memcpy(active_runtime->ram + destination, active_runtime->cart_rom + source,
               (size_t)length);
        if (writes_screen)
            ram_to_screen(active_runtime);
        if (range_touches_draw_state(destination, length))
            draw_state_from_ram(active_runtime);
    }
    return 0;
}

static int api_cartdata(lua_State *lua) {
    p8p_runtime_t *runtime = active_runtime;
    size_t length = 0;
    const char *id = lua_tolstring(lua, 1, &length);
    if (!id || !length || length > 64)
        return luaL_error(lua, "cart data id must be 1-64 characters");
    if (runtime->cartdata_active &&
        (length != strlen(runtime->cartdata_id) ||
         memcmp(runtime->cartdata_id, id, length) != 0))
        cartdata_flush(runtime);
    memset(runtime->ram + 0x5e00, 0, 0x100);
    memcpy(runtime->cartdata_id, id, length);
    runtime->cartdata_id[length] = '\0';
    runtime->cartdata_active = 1;
    runtime->cartdata_dirty = 0;
    int loaded = runtime->cartdata_load &&
        runtime->cartdata_load(runtime->cartdata_userdata,
                               runtime->cartdata_id,
                               runtime->ram + 0x5e00, 0x100) == 0;
    lua_pushboolean(lua, loaded);
    return 1;
}

static int api_dget(lua_State *lua) {
    int index = arg_int(lua, 1, 0);
    if (index < 0 || index >= 64) {
        lua_pushnumber(lua, fix32(0));
        return 1;
    }
    int address = 0x5e00 + index * 4;
    uint32_t bits = (uint32_t)active_runtime->ram[address] |
        ((uint32_t)active_runtime->ram[address + 1] << 8) |
        ((uint32_t)active_runtime->ram[address + 2] << 16) |
        ((uint32_t)active_runtime->ram[address + 3] << 24);
    lua_pushnumber(lua, fix32::frombits((int32_t)bits));
    return 1;
}

static int api_dset(lua_State *lua) {
    int index = arg_int(lua, 1, 0);
    if (index < 0 || index >= 64)
        return 0;
    uint32_t bits = (uint32_t)arg_number(lua, 2, fix32(0)).bits();
    int address = 0x5e00 + index * 4;
    active_runtime->ram[address] = (uint8_t)bits;
    active_runtime->ram[address + 1] = (uint8_t)(bits >> 8);
    active_runtime->ram[address + 2] = (uint8_t)(bits >> 16);
    active_runtime->ram[address + 3] = (uint8_t)(bits >> 24);
    cartdata_mark_dirty(active_runtime);
    return 0;
}

static void update_rng(p8p_runtime_t *runtime) {
    runtime->rng[1] = runtime->rng[0] +
        ((runtime->rng[1] >> 16) | (runtime->rng[1] << 16));
    runtime->rng[0] += runtime->rng[1];
}

static int api_srand(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    p8p_runtime_t *runtime = active_runtime;
    fix32 seed = arg_number(lua, 1, fix32(0));
    runtime->rng[0] = seed ? (uint32_t)seed.bits() : 0xdeadbeef;
    runtime->rng[1] = runtime->rng[0] ^ 0xbead29ba;
    for (int i = 0; i < 32; ++i)
        update_rng(runtime);
    return 0;
}

static int api_rnd(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    p8p_runtime_t *runtime = active_runtime;
    update_rng(runtime);
    if (lua_istable(lua, 1)) {
        int length = (int)lua_rawlen(lua, 1);
        if (length <= 0) {
            lua_pushnil(lua);
        } else {
            lua_rawgeti(lua, 1, (int)(runtime->rng[1] % (uint32_t)length) + 1);
        }
    } else {
        fix32 range = arg_number(lua, 1, fix32(1));
        uint32_t bits = (uint32_t)range.bits();
        lua_pushnumber(lua, fix32::frombits(bits ? runtime->rng[1] % bits : 0));
    }
    return 1;
}

static int api_time(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    double seconds = (double)active_runtime->frame_count /
                     (double)active_runtime->target_fps;
    lua_pushnumber(lua, fix32(seconds));
    return 1;
}

static int api_cursor(lua_State *lua) {
    profile_api(P8P_API_TEXT);
    int old_x = active_runtime->cursor_x;
    int old_y = active_runtime->cursor_y;
    if (lua_gettop(lua) >= 1) active_runtime->cursor_x = arg_int(lua, 1, old_x);
    if (lua_gettop(lua) >= 2) active_runtime->cursor_y = arg_int(lua, 2, old_y);
    cursor_to_ram(active_runtime);
    push_int(lua, old_x);
    push_int(lua, old_y);
    return 2;
}

static uint16_t glyph_bits(uint8_t character) {
#define GLYPH(r0, r1, r2, r3, r4) \
    ((uint16_t)((r0) << 12) | (uint16_t)((r1) << 9) | \
     (uint16_t)((r2) << 6) | (uint16_t)((r3) << 3) | (uint16_t)(r4))
    if (character >= 'a' && character <= 'z')
        character = (uint8_t)(character - ('a' - 'A'));
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
    case ',': return GLYPH(0, 0, 0, 2, 4);
    case ':': return GLYPH(0, 2, 0, 2, 0);
    case ';': return GLYPH(0, 2, 0, 2, 4);
    case '-': return GLYPH(0, 0, 7, 0, 0);
    case '+': return GLYPH(0, 2, 7, 2, 0);
    case '/': return GLYPH(1, 1, 2, 4, 4);
    case '\\': return GLYPH(4, 4, 2, 1, 1);
    case '(': return GLYPH(1, 2, 2, 2, 1);
    case ')': return GLYPH(4, 2, 2, 2, 4);
    case '[': return GLYPH(3, 2, 2, 2, 3);
    case ']': return GLYPH(6, 2, 2, 2, 6);
    case '<': return GLYPH(1, 2, 4, 2, 1);
    case '>': return GLYPH(4, 2, 1, 2, 4);
    case '=': return GLYPH(0, 7, 0, 7, 0);
    case '_': return GLYPH(0, 0, 0, 0, 7);
    case '\'': return GLYPH(2, 2, 0, 0, 0);
    case '"': return GLYPH(5, 5, 0, 0, 0);
    case '#': return GLYPH(5, 7, 5, 7, 5);
    case '%': return GLYPH(5, 1, 2, 4, 5);
    case '*': return GLYPH(0, 5, 2, 5, 0);
    default: return 0;
    }
#undef GLYPH
}

static int api_print(lua_State *lua) {
    profile_api(P8P_API_TEXT);
    size_t text_length = 0;
    const char *text = luaL_tolstring(lua, 1, &text_length);
    int x = arg_int(lua, 2, active_runtime->cursor_x);
    int y = arg_int(lua, 3, active_runtime->cursor_y);
    int origin_x = x;
    int max_width = 0;
    int color = arg_int(lua, 4, active_runtime->draw_color);

    for (size_t i = 0; text && i < text_length; ++i) {
        uint8_t character = (uint8_t)text[i];
        if (character == '\n') {
            int width = x - origin_x;
            if (width > max_width) max_width = width;
            x = origin_x;
            y += 6;
            continue;
        }
        uint16_t bits = glyph_bits(character);
        for (int row = 0; row < 5; ++row)
            for (int column = 0; column < 3; ++column)
                if (bits & (1u << (14 - row * 3 - column)))
                    screen_set(active_runtime, x + column, y + row, color);
        x += 4;
    }
    if (x - origin_x > max_width) max_width = x - origin_x;
    active_runtime->cursor_x = origin_x;
    active_runtime->cursor_y = y + 6;
    cursor_to_ram(active_runtime);
    lua_pop(lua, 1);
    push_int(lua, max_width);
    return 1;
}

static int api_sfx(lua_State *lua) {
    int channel = p8p_audio_sfx(active_runtime->audio,
                                arg_int(lua, 1, -1), arg_int(lua, 2, -1),
                                arg_int(lua, 3, 0), arg_int(lua, 4, 0));
    push_int(lua, channel < 0 ? 0 : channel);
    return 1;
}

static int api_music(lua_State *lua) {
    p8p_audio_music(active_runtime->audio,
                    arg_int(lua, 1, -1), arg_int(lua, 2, 0),
                    arg_int(lua, 3, 0));
    return 0;
}

static int api_flip(lua_State *lua) {
    int is_main_thread = lua_pushthread(lua);
    lua_pop(lua, 1);
    if (is_main_thread)
        return 0;
    return lua_yield(lua, 0);
}

static int api_fillp(lua_State *lua) {
    profile_api(P8P_API_DRAW_STATE);
    p8p_runtime_t *runtime = active_runtime;
    int32_t previous = ((int32_t)runtime->fill_pattern << 16) |
                       ((int32_t)runtime->fill_pattern_transparent << 8);
    int32_t bits = arg_number(lua, 1, fix32(0)).bits();
    runtime->fill_pattern = (uint16_t)((uint32_t)bits >> 16);
    runtime->fill_pattern_transparent = (uint8_t)((bits >> 15) & 1);
    runtime->ram[0x5f31] = (uint8_t)runtime->fill_pattern;
    runtime->ram[0x5f32] = (uint8_t)(runtime->fill_pattern >> 8);
    runtime->ram[0x5f33] = runtime->fill_pattern_transparent;
    lua_pushnumber(lua, fix32::frombits(previous));
    return 1;
}

static int api_stat(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    p8p_runtime_t *runtime = active_runtime;
    int item = arg_int(lua, 1, 0);
    switch (item) {
    case 0:
        lua_pushnumber(lua, fix32(1));
        break;
    case 1:
    case 2:
        /* A conservative non-zero load also keeps carts which use stat(1)
         * as a cooperative-yield heuristic from choosing a busy loop. */
        lua_pushnumber(lua, fix32::frombits(0xc000));
        break;
    case 4:
    case 6:
        lua_pushstring(lua, "");
        break;
    case 5:
        push_int(lua, 42);
        break;
    case 7:
    case 8:
        push_int(lua, runtime->target_fps);
        break;
    case 16: case 17: case 18: case 19:
        push_int(lua, p8p_audio_channel_sfx(runtime->audio, item - 16));
        break;
    case 20: case 21: case 22: case 23:
        push_int(lua, p8p_audio_channel_note(runtime->audio, item - 20));
        break;
    case 24:
    case 54:
        push_int(lua, p8p_audio_music_pattern(runtime->audio));
        break;
    case 25:
    case 55:
        push_int(lua, p8p_audio_music_count(runtime->audio));
        break;
    case 28:
    case 30:
    case 120:
    case 121:
    case 122:
        lua_pushboolean(lua, 0);
        break;
    case 29:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 38:
    case 39:
    case 90:
    case 91:
    case 92:
    case 93:
    case 94:
    case 95:
    case 102:
        push_int(lua, 0);
        break;
    case 31:
    case 100:
        lua_pushstring(lua, "");
        break;
    case 101:
        lua_pushnil(lua);
        break;
    case 108:
        push_int(lua, 32);
        break;
    case 46: case 47: case 48: case 49:
        push_int(lua, p8p_audio_channel_sfx(runtime->audio, item - 46));
        break;
    case 50: case 51: case 52: case 53:
        push_int(lua, p8p_audio_channel_note(runtime->audio, item - 50));
        break;
    default:
        lua_pushnil(lua);
        break;
    }
    return 1;
}

static int api_run(lua_State *lua) {
    (void)lua;
    active_runtime->restart_requested = 1;
    return 0;
}

static int api_stub(lua_State *lua) {
    (void)lua;
    return 0;
}

static int api_serial(lua_State *lua) {
    (void)lua;
    push_int(lua, 0);
    return 1;
}

static int api_all_next(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    if (!lua_istable(lua, lua_upvalueindex(1))) {
        lua_pushnil(lua);
        return 1;
    }
    int index = (int)lua_tointeger(lua, lua_upvalueindex(2));
    lua_rawgeti(lua, lua_upvalueindex(1), index);
    if (lua_rawequal(lua, -1, lua_upvalueindex(3))) {
        lua_pop(lua, 1);
        ++index;
    } else {
        lua_pop(lua, 1);
    }
    int length = (int)lua_rawlen(lua, lua_upvalueindex(1));
    while (index <= length) {
        lua_rawgeti(lua, lua_upvalueindex(1), index);
        if (!lua_isnil(lua, -1))
            break;
        lua_pop(lua, 1);
        ++index;
    }
    lua_pushinteger(lua, index);
    lua_replace(lua, lua_upvalueindex(2));
    if (index > length) {
        lua_pushnil(lua);
        lua_replace(lua, lua_upvalueindex(3));
        lua_pushnil(lua);
        return 1;
    }
    lua_pushvalue(lua, -1);
    lua_replace(lua, lua_upvalueindex(3));
    return 1;
}

static int api_all(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    if (lua_gettop(lua) >= 1)
        lua_pushvalue(lua, 1);
    else
        lua_pushnil(lua);
    lua_pushinteger(lua, 1);
    lua_pushnil(lua);
    lua_pushcclosure(lua, api_all_next, 3);
    return 1;
}

static int api_foreach(lua_State *lua) {
    profile_api(P8P_API_HELPER);
    if (!lua_istable(lua, 1) || !lua_isfunction(lua, 2))
        return 0;
    lua_settop(lua, 2);

    int index = 1;
    while (index <= (int)lua_rawlen(lua, 1)) {
        lua_rawgeti(lua, 1, index);
        if (lua_isnil(lua, -1)) {
            lua_pop(lua, 1);
            ++index;
            continue;
        }

        /* Keep the yielded value below the callback.  If the callback deletes
         * it, the next element shifts into this same index and must be visited
         * rather than skipped, matching PICO-8 foreach()/all() semantics. */
        lua_pushvalue(lua, 2);
        lua_pushvalue(lua, -2);
        lua_call(lua, 1, 0);

        lua_rawgeti(lua, 1, index);
        int unchanged = lua_rawequal(lua, -1, -2);
        lua_pop(lua, 2);
        if (unchanged)
            ++index;
    }
    return 0;
}

static int api_cooperative_stub(lua_State *lua) {
    int is_main_thread = lua_pushthread(lua);
    lua_pop(lua, 1);
    return is_main_thread ? 0 : lua_yield(lua, 0);
}

static const luaL_Reg runtime_api[] = {
    {"cls", api_cls}, {"pset", api_pset}, {"pget", api_pget},
    {"sget", api_sget}, {"sset", api_sset}, {"color", api_color},
    {"line", api_line}, {"rect", api_rect}, {"rectfill", api_rectfill},
    {"circ", api_circ}, {"circfill", api_circfill}, {"oval", api_oval},
    {"ovalfill", api_ovalfill}, {"spr", api_spr}, {"sspr", api_sspr},
    {"tline", api_tline},
    {"mget", api_mget}, {"mset", api_mset}, {"map", api_map},
    {"mapdraw", api_map}, {"fget", api_fget}, {"fset", api_fset},
    {"camera", api_camera}, {"clip", api_clip}, {"pal", api_pal},
    {"palt", api_palt}, {"btn", api_btn}, {"btnp", api_btnp},
    {"peek", api_peek}, {"poke", api_poke}, {"peek2", api_peek2},
    {"poke2", api_poke2}, {"peek4", api_peek4}, {"poke4", api_poke4},
    {"memset", api_memset},
    {"memcpy", api_memcpy}, {"reload", api_reload}, {"rnd", api_rnd},
    {"cartdata", api_cartdata}, {"dget", api_dget}, {"dset", api_dset},
    {"srand", api_srand}, {"time", api_time}, {"t", api_time},
    {"flip", api_flip},
    {"cursor", api_cursor}, {"print", api_print}, {"sfx", api_sfx},
    {"music", api_music}, {"fillp", api_fillp}, {"menuitem", api_stub},
    {"printh", api_stub}, {"extcmd", api_stub}, {"serial", api_serial},
    {"mkdir", api_cooperative_stub}, {"cd", api_stub},
    {"stat", api_stat}, {"run", api_run}, {"all", api_all},
    {"foreach", api_foreach}, {NULL, NULL}
};

static void register_pico8_button_constants(lua_State *lua) {
    static const uint8_t pico8_names[6] = {
        0x8b, /* left */
        0x91, /* right */
        0x94, /* up */
        0x83, /* down */
        0x8e, /* O */
        0x97  /* X */
    };
    static const char *utf8_names[6] = {
        "⬅️", "➡️", "⬆️", "⬇️", "🅾️", "❎"
    };
    char pico8_name[2] = {0, 0};
    for (int button = 0; button < 6; ++button) {
        pico8_name[0] = (char)pico8_names[button];
        push_int(lua, button);
        lua_setglobal(lua, pico8_name);
        push_int(lua, button);
        lua_setglobal(lua, utf8_names[button]);
    }
}

static const char bootstrap_lua[] =
    "function count(c,v) local n=0 for i=1,#c do if c[i]~=nil and (v==nil or c[i]==v) then n+=1 end end return n end\n"
    "function add(c,v,i) if c~=nil then i=i and mid(1,i\\1,#c+1) or #c+1 for j=#c,i,-1 do c[j+1]=c[j] end c[i]=v return v end end\n"
    "function del(c,v) if c~=nil then local n=#c for i=1,n do if c[i]==v then for j=i,n do c[j]=c[j+1] end return v end end end end\n"
    "function deli(c,i) if c~=nil then i=i and mid(1,i\\1,#c) or #c local v=c[i] for j=i,#c do c[j]=c[j+1] end return v end end\n"
    "sub=string.sub chr=chr ord=ord unpack=table.unpack\n"
    "cocreate=coroutine.create coresume=coroutine.resume "
    "costatus=coroutine.status yield=coroutine.yield\n"
    "rawset(debug.getregistry(),'__PICO8_SANDBOX',_G)\n"
    "eris.__p8p_perm={} eris.__p8p_unperm={} eris.__p8p_original={}\n"
    "function eris.__p8p_init()\n"
    " local keys={} for k in pairs(_G) do keys[#keys+1]=k end table.sort(keys)\n"
    " local seen={} local n=0 local function permanent(v) local t=type(v)\n"
    "  if t~='table' and t~='function' and t~='userdata' and t~='thread' then return end\n"
    "  if seen[v] then return end seen[v]=true n+=1 eris.__p8p_perm[v]=n eris.__p8p_unperm[n]=v\n"
    "  if t=='table' and v~=_G and v~=eris then for k,x in pairs(v) do permanent(k) permanent(x) end permanent(getmetatable(v)) end\n"
    " end\n"
    " for i,k in ipairs(keys) do local v=_G[k] permanent(v) eris.__p8p_original[k]=v end\n"
    "end\n"
    "function eris.__p8p_save()\n"
    " local changed={} for k,v in pairs(_G) do if eris.__p8p_original[k]~=v then changed[k]=v end end\n"
    " return eris.persist(eris.__p8p_perm,changed)\n"
    "end\n"
    "function eris.__p8p_load(blob)\n"
    " local changed=eris.unpersist(eris.__p8p_unperm,blob)\n"
    " local stale={} for k,v in pairs(_G) do if eris.__p8p_original[k]~=v then stale[#stale+1]=k end end\n"
    " for k in all(stale) do _G[k]=nil end for k,v in pairs(changed) do _G[k]=v end\n"
    "end\n";

static void set_error(p8p_runtime_t *runtime, const char *prefix) {
    const char *message = runtime->lua ? lua_tostring(runtime->lua, -1) : NULL;
    snprintf(runtime->error, sizeof(runtime->error), "%s%s%s", prefix,
             message ? ": " : "", message ? message : "");
    if (runtime->lua && lua_gettop(runtime->lua) > 0)
        lua_pop(runtime->lua, 1);
}

static void register_api(lua_State *lua) {
    for (const luaL_Reg *entry = runtime_api; entry->name; ++entry)
        lua_register(lua, entry->name, entry->func);
}

static void set_thread_error(p8p_runtime_t *runtime, const char *prefix,
                             lua_State *thread) {
    const char *message = thread ? lua_tostring(thread, -1) : NULL;
    snprintf(runtime->error, sizeof(runtime->error), "%s%s%s", prefix,
             message ? ": " : "", message ? message : "");
    if (thread && lua_gettop(thread) > 0)
        lua_pop(thread, 1);
}

static void release_cart_thread(p8p_runtime_t *runtime) {
    if (runtime->cart_thread_ref != LUA_NOREF)
        luaL_unref(runtime->lua, LUA_REGISTRYINDEX, runtime->cart_thread_ref);
    runtime->cart_thread_ref = LUA_NOREF;
    runtime->cart_thread = NULL;
    runtime->cart_thread_active = 0;
}

static int start_init_thread(p8p_runtime_t *runtime) {
    lua_getglobal(runtime->lua, "_init");
    if (!lua_isfunction(runtime->lua, -1)) {
        lua_pop(runtime->lua, 1);
        return 0;
    }
    runtime->cart_thread = lua_newthread(runtime->lua);
    runtime->cart_thread_ref = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
    lua_xmove(runtime->lua, runtime->cart_thread, 1);
    runtime->cart_thread_kind = 1;
    runtime->cart_instruction_slices = 0;
    install_service_hook(runtime);
    int status = lua_resume(runtime->cart_thread, runtime->lua, 0);
    if (status == LUA_YIELD) {
        runtime->cart_thread_active = 1;
        return 0;
    }
    if (status != LUA_OK) {
        set_thread_error(runtime, "_init", runtime->cart_thread);
        return -1;
    }
    release_cart_thread(runtime);
    return 0;
}

static int finish_cart_load(p8p_runtime_t *runtime) {
    lua_getglobal(runtime->lua, "_update60");
    runtime->target_fps = lua_isfunction(runtime->lua, -1) ? 60 : 30;
    lua_pop(runtime->lua, 1);
    return start_init_thread(runtime);
}

static int dispatch_frame(lua_State *lua) {
    p8p_runtime_t *runtime = active_runtime;
    const char *update = runtime->target_fps == 60 ? "_update60" : "_update";

    lua_getglobal(lua, update);
    if (lua_isfunction(lua, -1)) {
        if (runtime->profile_hook)
            runtime->profile_hook(runtime->profile_userdata,
                                  P8P_PROFILE_UPDATE_BEGIN);
        lua_call(lua, 0, 0);
        if (runtime->profile_hook)
            runtime->profile_hook(runtime->profile_userdata,
                                  P8P_PROFILE_UPDATE_END);
    } else {
        lua_pop(lua, 1);
    }

    if (runtime->draw_frame) {
        lua_getglobal(lua, "_draw");
        if (lua_isfunction(lua, -1)) {
            if (runtime->profile_hook)
                runtime->profile_hook(runtime->profile_userdata,
                                      P8P_PROFILE_DRAW_BEGIN);
            lua_call(lua, 0, 0);
            if (runtime->profile_hook)
                runtime->profile_hook(runtime->profile_userdata,
                                      P8P_PROFILE_DRAW_END);
        } else {
            lua_pop(lua, 1);
        }
    }
    return 0;
}

extern "C" p8p_runtime_t *p8p_runtime_create(void) {
    p8p_runtime_t *runtime = (p8p_runtime_t *)calloc(1, sizeof(*runtime));
    if (!runtime)
        return NULL;
    runtime->target_fps = 30;
    runtime->draw_frame = 1;
    runtime->draw_color = 6;
    runtime->clip_x1 = runtime->clip_y1 = 128;
    for (int i = 0; i < 16; ++i) {
        runtime->draw_palette[i] = (uint8_t)i;
        runtime->screen_palette[i] = (uint8_t)i;
    }
    runtime->transparent[0] = 1;
    runtime->palettes_default = 1;
    runtime->transparency_default = 1;
    runtime->audio = p8p_audio_create(runtime->ram);
    if (!runtime->audio) {
        free(runtime);
        return NULL;
    }
    return runtime;
}

extern "C" void p8p_runtime_destroy(p8p_runtime_t *runtime) {
    if (!runtime)
        return;
    cartdata_flush(runtime);
    if (runtime->lua)
        lua_close(runtime->lua);
    if (active_runtime == runtime)
        active_runtime = NULL;
    free(runtime->cart_lua);
    p8p_audio_destroy(runtime->audio);
    free(runtime);
}

extern "C" int p8p_runtime_load(p8p_runtime_t *runtime, const p8p_cart_t *cart) {
    if (!runtime || !cart || !cart->lua)
        return -1;
    char *cart_lua = (char *)malloc(cart->lua_size + 1);
    if (!cart_lua)
        return -2;
    memcpy(cart_lua, cart->lua, cart->lua_size);
    cart_lua[cart->lua_size] = '\0';
    cartdata_flush(runtime);
    runtime->cartdata_active = 0;
    runtime->cartdata_dirty = 0;
    runtime->cartdata_id[0] = '\0';
    if (runtime->lua)
        lua_close(runtime->lua);
    free(runtime->cart_lua);
    runtime->cart_lua = cart_lua;
    runtime->cart_lua_size = cart->lua_size;
    runtime->cart_thread = NULL;
    runtime->cart_thread_ref = LUA_NOREF;
    runtime->cart_thread_active = 0;
    memset(runtime->ram, 0, sizeof(runtime->ram));
    memcpy(runtime->cart_rom, cart->rom, sizeof(runtime->cart_rom));
    memcpy(runtime->ram, cart->rom, sizeof(runtime->cart_rom));
    runtime->ram[0x5f54] = 0x00;
    runtime->ram[0x5f55] = 0x60;
    runtime->ram[0x5f56] = 0x20;
    runtime->ram[0x5f57] = 128;
    runtime->ram[0x5f5c] = 15;
    runtime->ram[0x5f5d] = 4;
    runtime->ram[0x5f5e] = 0xff;
    for (int i = 0; i < 16; ++i) {
        runtime->draw_palette[i] = (uint8_t)i;
        runtime->screen_palette[i] = (uint8_t)i;
    }
    memset(runtime->transparent, 0, sizeof(runtime->transparent));
    runtime->transparent[0] = 1;
    runtime->palettes_default = 1;
    runtime->transparency_default = 1;
    runtime->camera_x = runtime->camera_y = 0;
    runtime->clip_x0 = runtime->clip_y0 = 0;
    runtime->clip_x1 = runtime->clip_y1 = 128;
    runtime->draw_color = 6;
    runtime->cursor_x = runtime->cursor_y = 0;
    runtime->fill_pattern = 0;
    runtime->fill_pattern_transparent = 0;
    draw_state_to_ram(runtime);
    ram_to_screen(runtime);
    p8p_audio_reset(runtime->audio, runtime->ram);
    runtime->error[0] = '\0';
    runtime->frame_count = 0;
    runtime->buttons = runtime->previous_buttons = 0;
    memset(runtime->held_frames, 0, sizeof(runtime->held_frames));
    runtime->lua = luaL_newstate();
    if (!runtime->lua) {
        snprintf(runtime->error, sizeof(runtime->error), "cannot create z8lua state");
        return -2;
    }
    active_runtime = runtime;
    luaL_openlibs(runtime->lua);
    lua_setpico8memory(runtime->lua, runtime->ram);
    register_api(runtime->lua);
    register_pico8_button_constants(runtime->lua);
    api_srand(runtime->lua);

    if (luaL_dostring(runtime->lua, bootstrap_lua) != LUA_OK) {
        set_error(runtime, "bootstrap");
        return -3;
    }
    lua_getglobal(runtime->lua, "eris");
    lua_getfield(runtime->lua, -1, "__p8p_init");
    if (lua_pcall(runtime->lua, 0, 0, 0) != LUA_OK) {
        set_error(runtime, "state init");
        lua_pop(runtime->lua, 1);
        return -3;
    }
    lua_getfield(runtime->lua, -1, "__p8p_save");
    runtime->persist_ref = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
    lua_getfield(runtime->lua, -1, "__p8p_load");
    runtime->restore_ref = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
    lua_pop(runtime->lua, 1);
    runtime->cart_thread = lua_newthread(runtime->lua);
    runtime->cart_thread_ref = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
    runtime->cart_thread_kind = 0;
    runtime->restart_requested = 0;
    if (!runtime->cart_thread ||
        luaL_loadbuffer(runtime->cart_thread, runtime->cart_lua,
                        runtime->cart_lua_size,
                        "cart.p8") != LUA_OK) {
        set_thread_error(runtime, "cart", runtime->cart_thread);
        return -4;
    }
    runtime->cart_instruction_slices = 0;
    install_service_hook(runtime);
    int cart_status = lua_resume(runtime->cart_thread, runtime->lua, 0);
    if (cart_status == LUA_YIELD) {
        runtime->cart_thread_active = 1;
        runtime->target_fps = 30;
    } else if (cart_status == LUA_OK) {
        release_cart_thread(runtime);
        if (finish_cart_load(runtime) != 0)
            return -5;
    } else {
        set_thread_error(runtime, "cart", runtime->cart_thread);
        return -4;
    }
    install_service_hook(runtime);
    return 0;
}

extern "C" void p8p_runtime_set_service_hook(
    p8p_runtime_t *runtime, p8p_runtime_service_fn callback, void *userdata) {
    if (!runtime)
        return;
    runtime->service_hook = callback;
    runtime->service_userdata = userdata;
    install_service_hook(runtime);
}

extern "C" void p8p_runtime_set_profile_hook(
    p8p_runtime_t *runtime, p8p_runtime_profile_fn callback, void *userdata) {
    if (!runtime)
        return;
    runtime->profile_hook = callback;
    runtime->profile_userdata = userdata;
}

extern "C" void p8p_runtime_get_api_profile(
    const p8p_runtime_t *runtime, p8p_runtime_api_profile_t *profile) {
    if (!profile)
        return;
    if (!runtime) {
        memset(profile, 0, sizeof(*profile));
        return;
    }
    memcpy(profile->calls, runtime->api_profile_calls,
           sizeof(profile->calls));
}

extern "C" void p8p_runtime_set_cartdata_hooks(
    p8p_runtime_t *runtime, p8p_runtime_cartdata_load_fn load,
    p8p_runtime_cartdata_save_fn save, void *userdata) {
    if (!runtime)
        return;
    cartdata_flush(runtime);
    runtime->cartdata_load = load;
    runtime->cartdata_save = save;
    runtime->cartdata_userdata = userdata;
}

extern "C" void p8p_runtime_flush_cartdata(p8p_runtime_t *runtime) {
    cartdata_flush(runtime);
}

extern "C" int p8p_runtime_step(p8p_runtime_t *runtime, uint8_t buttons) {
    return p8p_runtime_step_with_draw(runtime, buttons, 1);
}

static int restart_current_cart(p8p_runtime_t *runtime) {
    p8p_cart_t *cart = (p8p_cart_t *)calloc(1, sizeof(*cart));
    if (!cart)
        return -2;
    memcpy(cart->rom, runtime->cart_rom, sizeof(cart->rom));
    cart->lua = runtime->cart_lua;
    cart->lua_size = runtime->cart_lua_size;
    int result = p8p_runtime_load(runtime, cart);
    free(cart);
    return result;
}

extern "C" void p8p_runtime_set_live_buttons(p8p_runtime_t *runtime,
                                               uint8_t buttons) {
    if (!runtime)
        return;
    uint8_t next = buttons & 0x7f;
    for (int button = 0; button < 7; ++button) {
        uint8_t mask = (uint8_t)(1u << button);
        if (!(next & mask))
            runtime->held_frames[button] = 0;
        else if (!(runtime->buttons & mask))
            runtime->held_frames[button] = 1;
    }
    runtime->buttons = next;
}

extern "C" int p8p_runtime_step_with_draw(p8p_runtime_t *runtime,
                                            uint8_t buttons,
                                            int draw_frame) {
    if (!runtime || !runtime->lua)
        return -1;
    if (runtime->restart_requested)
        return restart_current_cart(runtime);
    active_runtime = runtime;
    if (runtime->profile_hook)
        memset(runtime->api_profile_calls, 0,
               sizeof(runtime->api_profile_calls));
    runtime->draw_frame = draw_frame != 0;
    runtime->previous_buttons = runtime->buttons;
    runtime->buttons = buttons & 0x7f;
    for (int i = 0; i < 7; ++i) {
        if (runtime->buttons & (1u << i)) {
            if (runtime->held_frames[i] != 0xffff)
                ++runtime->held_frames[i];
        } else {
            runtime->held_frames[i] = 0;
        }
    }
    ++runtime->frame_count;
    if (runtime->cartdata_dirty &&
        runtime->frame_count % (uint32_t)(runtime->target_fps * 5) == 0)
        cartdata_flush(runtime);
    if (runtime->cart_thread_active) {
        int thread_kind = runtime->cart_thread_kind;
        runtime->cart_instruction_slices = 0;
        int status = lua_resume(runtime->cart_thread, runtime->lua, 0);
        if (status == LUA_YIELD)
            return 0;
        if (status != LUA_OK) {
            set_thread_error(runtime, "frame", runtime->cart_thread);
            return -2;
        }
        release_cart_thread(runtime);
        if (thread_kind == 0 && finish_cart_load(runtime) != 0)
            return -2;
        install_service_hook(runtime);
        return 0;
    }
    lua_pushcfunction(runtime->lua, dispatch_frame);
    if (lua_pcall(runtime->lua, 0, 0, 0) != LUA_OK) {
        set_error(runtime, "frame");
        return -2;
    }

    if (runtime->restart_requested)
        return restart_current_cart(runtime);

    return 0;
}

static void capture_fixed_state(const p8p_runtime_t *runtime,
                                p8p_runtime_fixed_state *state) {
    memcpy(state->ram, runtime->ram, sizeof(state->ram));
    memcpy(state->framebuffer, runtime->framebuffer, sizeof(state->framebuffer));
    state->screen_ram_dirty = runtime->screen_ram_dirty;
    memcpy(state->draw_palette, runtime->draw_palette, sizeof(state->draw_palette));
    memcpy(state->screen_palette, runtime->screen_palette, sizeof(state->screen_palette));
    memcpy(state->transparent, runtime->transparent, sizeof(state->transparent));
    state->buttons = runtime->buttons;
    state->previous_buttons = runtime->previous_buttons;
    memcpy(state->held_frames, runtime->held_frames, sizeof(state->held_frames));
    memcpy(state->rng, runtime->rng, sizeof(state->rng));
    state->camera_x = runtime->camera_x;
    state->camera_y = runtime->camera_y;
    state->clip_x0 = runtime->clip_x0;
    state->clip_y0 = runtime->clip_y0;
    state->clip_x1 = runtime->clip_x1;
    state->clip_y1 = runtime->clip_y1;
    state->draw_color = runtime->draw_color;
    state->cursor_x = runtime->cursor_x;
    state->cursor_y = runtime->cursor_y;
    state->target_fps = runtime->target_fps;
    state->frame_count = runtime->frame_count;
}

static void restore_fixed_state(p8p_runtime_t *runtime,
                                const p8p_runtime_fixed_state *state) {
    memcpy(runtime->ram, state->ram, sizeof(runtime->ram));
    memcpy(runtime->framebuffer, state->framebuffer, sizeof(runtime->framebuffer));
    runtime->screen_ram_dirty = state->screen_ram_dirty;
    memcpy(runtime->draw_palette, state->draw_palette, sizeof(runtime->draw_palette));
    memcpy(runtime->screen_palette, state->screen_palette, sizeof(runtime->screen_palette));
    memcpy(runtime->transparent, state->transparent, sizeof(runtime->transparent));
    update_palette_default_flags(runtime);
    runtime->buttons = state->buttons;
    runtime->previous_buttons = state->previous_buttons;
    memcpy(runtime->held_frames, state->held_frames, sizeof(runtime->held_frames));
    runtime->held_frames[6] = 0;
    memcpy(runtime->rng, state->rng, sizeof(runtime->rng));
    runtime->camera_x = state->camera_x;
    runtime->camera_y = state->camera_y;
    runtime->clip_x0 = state->clip_x0;
    runtime->clip_y0 = state->clip_y0;
    runtime->clip_x1 = state->clip_x1;
    runtime->clip_y1 = state->clip_y1;
    runtime->draw_color = state->draw_color;
    runtime->cursor_x = state->cursor_x;
    runtime->cursor_y = state->cursor_y;
    runtime->target_fps = state->target_fps;
    runtime->frame_count = state->frame_count;
}

extern "C" int p8p_runtime_save_state(p8p_runtime_t *runtime, void **data,
                                        size_t *size) {
    size_t lua_size;
    size_t audio_size;
    size_t total_size;
    uint8_t *output;
    p8p_runtime_state_header header;
    p8p_runtime_fixed_state *fixed;
    const char *lua_blob;

    if (!runtime || !runtime->lua || !data || !size)
        return -1;
    cartdata_flush(runtime);
    *data = NULL;
    *size = 0;
    active_runtime = runtime;
    lua_sethook(runtime->lua, NULL, 0, 0);
    lua_rawgeti(runtime->lua, LUA_REGISTRYINDEX, runtime->persist_ref);
    int persist_status = lua_pcall(runtime->lua, 0, 1, 0);
    install_service_hook(runtime);
    if (persist_status != LUA_OK) {
        set_error(runtime, "save state");
        return -2;
    }
    lua_blob = lua_tolstring(runtime->lua, -1, &lua_size);
    if (!lua_blob) {
        lua_pop(runtime->lua, 1);
        snprintf(runtime->error, sizeof(runtime->error), "save state: no Lua data");
        return -3;
    }
    audio_size = p8p_audio_state_size();
    if (lua_size > UINT32_MAX || audio_size > UINT32_MAX ||
        lua_size > SIZE_MAX - sizeof(header) - sizeof(fixed) - audio_size) {
        lua_pop(runtime->lua, 1);
        snprintf(runtime->error, sizeof(runtime->error), "save state: too large");
        return -4;
    }
    total_size = sizeof(header) + sizeof(*fixed) + audio_size + lua_size;
    output = (uint8_t *)malloc(total_size);
    if (!output) {
        lua_pop(runtime->lua, 1);
        snprintf(runtime->error, sizeof(runtime->error), "save state: out of memory");
        return -5;
    }
    memcpy(header.magic, runtime_state_magic, sizeof(header.magic));
    header.version = 1;
    header.fixed_size = (uint32_t)sizeof(*fixed);
    header.audio_size = (uint32_t)audio_size;
    header.lua_size = (uint32_t)lua_size;
    fixed = (p8p_runtime_fixed_state *)(output + sizeof(header));
    capture_fixed_state(runtime, fixed);
    memcpy(output, &header, sizeof(header));
    if (p8p_audio_save_state(runtime->audio,
                             output + sizeof(header) + sizeof(*fixed),
                             audio_size) != 0) {
        free(output);
        lua_pop(runtime->lua, 1);
        return -6;
    }
    memcpy(output + sizeof(header) + sizeof(*fixed) + audio_size,
           lua_blob, lua_size);
    lua_pop(runtime->lua, 1);
    *data = output;
    *size = total_size;
    return 0;
}

extern "C" int p8p_runtime_load_state(p8p_runtime_t *runtime,
                                        const void *data, size_t size) {
    p8p_runtime_state_header header;
    const uint8_t *input = (const uint8_t *)data;
    const p8p_runtime_fixed_state *fixed;
    const uint8_t *audio;
    const uint8_t *lua_blob;
    size_t expected;

    if (!runtime || !runtime->lua || !data || size < sizeof(header))
        return -1;
    memcpy(&header, input, sizeof(header));
    if (memcmp(header.magic, runtime_state_magic, sizeof(header.magic)) != 0 ||
        header.version != 1 || header.fixed_size != sizeof(*fixed) ||
        header.audio_size != p8p_audio_state_size()) {
        snprintf(runtime->error, sizeof(runtime->error), "load state: incompatible data");
        return -2;
    }
    expected = sizeof(header) + (size_t)header.fixed_size +
               (size_t)header.audio_size + (size_t)header.lua_size;
    if (expected != size) {
        snprintf(runtime->error, sizeof(runtime->error), "load state: truncated data");
        return -3;
    }
    fixed = (const p8p_runtime_fixed_state *)(input + sizeof(header));
    audio = input + sizeof(header) + header.fixed_size;
    lua_blob = audio + header.audio_size;
    active_runtime = runtime;
    lua_sethook(runtime->lua, NULL, 0, 0);
    lua_rawgeti(runtime->lua, LUA_REGISTRYINDEX, runtime->restore_ref);
    lua_pushlstring(runtime->lua, (const char *)lua_blob, header.lua_size);
    int restore_status = lua_pcall(runtime->lua, 1, 0, 0);
    install_service_hook(runtime);
    if (restore_status != LUA_OK) {
        set_error(runtime, "load state");
        return -4;
    }
    restore_fixed_state(runtime, fixed);
    if (p8p_audio_load_state(runtime->audio, runtime->ram, audio,
                             header.audio_size) != 0)
        return -5;
    return 0;
}

extern "C" const uint8_t *p8p_runtime_framebuffer(p8p_runtime_t *runtime) {
    return runtime ? runtime->framebuffer : NULL;
}

extern "C" const uint8_t *p8p_runtime_screen_palette(p8p_runtime_t *runtime) {
    return runtime ? runtime->screen_palette : NULL;
}

extern "C" void p8p_runtime_audio_render(p8p_runtime_t *runtime,
                                          int16_t *stereo, size_t frames) {
    p8p_audio_render(runtime ? runtime->audio : NULL, stereo, frames);
}

extern "C" int p8p_runtime_target_fps(const p8p_runtime_t *runtime) {
    return runtime ? runtime->target_fps : 0;
}

extern "C" const char *p8p_runtime_error(const p8p_runtime_t *runtime) {
    return runtime ? runtime->error : "no runtime";
}

#ifdef P8P_RUNTIME_DEBUG
extern "C" int p8p_runtime_debug_eval_int(p8p_runtime_t *runtime,
                                            const char *expression,
                                            int *value) {
    char source[256];
    if (!runtime || !runtime->lua || !expression || !value)
        return -1;
    snprintf(source, sizeof(source), "return %s", expression);
    active_runtime = runtime;
    if (luaL_dostring(runtime->lua, source) != LUA_OK) {
        if (lua_gettop(runtime->lua) > 0)
            lua_pop(runtime->lua, 1);
        return -2;
    }
    *value = (int)lua_tonumber(runtime->lua, -1);
    lua_pop(runtime->lua, 1);
    return 0;
}
#endif
