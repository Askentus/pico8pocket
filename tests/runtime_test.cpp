#include "p8p/cart.h"
#include "p8p/runtime.h"
#include "p8p/state_store.h"
#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static int service_hook_calls;
static int inject_pause;
static p8p_runtime_t *service_hook_runtime;
static int profile_event_calls[4];

static void count_service_hook(void *) {
    ++service_hook_calls;
    if (inject_pause && service_hook_runtime)
        p8p_runtime_set_live_buttons(service_hook_runtime, 1u << 6);
}

static void count_profile_event(void *, p8p_runtime_profile_event_t event) {
    if ((unsigned)event < 4)
        ++profile_event_calls[event];
}

static int test_cartdata_load(void *userdata, const char *id,
                              uint8_t *data, size_t size) {
    return p8p_cartdata_load_file((const char *)userdata, id, data, size);
}

static int test_cartdata_save(void *userdata, const char *id,
                              const uint8_t *data, size_t size) {
    return p8p_cartdata_save_file((const char *)userdata, id, data, size);
}

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ++failures; \
    } \
} while (0)

#define TEST_NOTES_4 "21070210702107021070"
#define TEST_NOTES_32 \
    TEST_NOTES_4 TEST_NOTES_4 TEST_NOTES_4 TEST_NOTES_4 \
    TEST_NOTES_4 TEST_NOTES_4 TEST_NOTES_4 TEST_NOTES_4

int main(void) {
    static const uint8_t source[] =
        "pico-8 cartridge // http://www.pico-8.com\n"
        "version 42\n"
        "__lua__\n"
        "x=0\n"
        "local tins=table.insert holder={}\n"
        "function _init() cartdata('runtime-test') saved=dget(0) "
        "dset(0,saved+1) empty={} add(nil,1) del(nil,1) deli(nil) "
        "foreach(nil,function() add(empty,99) end) add(empty,7) "
        "seq={1,2,3} seen=0 foreach(seq,function(v) seen=seen*10+v "
        "if v==2 then del(seq,v) end end) "
        "cls(1) sfx(0) end\n"
        "function _update60() for i=1,16384 do local z=i end "
        "if btn(\x91) then x+=1 tins(holder,x) end end\n"
        "function _draw() local loopn=0 for i=1,10 do loopn+=1 end\n"
        " cls(0) pset(x,3,8) rectfill(10,10,12,12,9) print('a',20,20,7)\n"
        " circfill(30,30,2,10) spr(0,50,50)\n"
        " camera(5,6) clip(40,40,3,3) rectfill(44,45,50,50,11)\n"
        " camera() clip()\n"
        " pset(5,4,12)\n"
        " ovalfill(70,70,74,72,10) sspr(0,0,2,1,80,80,4,2)\n"
        " poke2(0x4300,0x1234) if peek2(0x4300)==0x1234 then pset(90,2,7) end\n"
        " poke4(0x4304,1.5) if peek4(0x4304)==1.5 then pset(91,2,8) end\n"
        " if 5.5/2==2.75 and -5.5/2==-2.75 and 2.75*2==5.5 "
        "then pset(92,2,9) end\n"
        " if seen==123 and #seq==2 and seq[2]==3 then pset(93,2,10) end\n"
        " if peek(0x6102)~=0xc0 then pset(4,4,8) end\n"
        " poke(0x6103,0x34) pal(8,12,1) "
        "pal({[0]=128,[1]=9,[15]=139},1) pset(loopn,6,13)\n"
        "end\n"
        "__gfx__\n"
        "1200000000000000\n"
        "__gff__\n"
        "05\n"
        "__map__\n"
        "01\n"
        "__sfx__\n"
        "00100000" TEST_NOTES_32 "\n";
    p8p_cart_t cart = {};
    p8p_cart_t png_cart = {};
    p8p_cart_t legacy_cart = {};
    p8p_cart_t celeste_cart = {};
    p8p_cart_t flip_cart = {};
    p8p_cart_t glyph_cart = {};
    p8p_cart_t nested_short_if_cart = {};
    p8p_cart_t live_input_cart = {};
    p8p_cart_t runaway_cart = {};
    p8p_cart_t run_cart = {};
    p8p_cart_t env_fallback_cart = {};
    p8p_runtime_t *runtime;
    const uint8_t *framebuffer;
    const uint8_t *screen_palette;
    int16_t audio_samples[512 * 2];
    void *saved_state = NULL;
    size_t saved_state_size = 0;
    p8p_cart_hash_t cart_hash = {};
    p8p_state_meta_t state_meta = {};
    p8p_runtime_api_profile_t api_profile = {};
    unsigned char config_pattern[32];
    unsigned char config_readback[32];
    char store_path[] = "/tmp/pico8pocket-state-XXXXXX";
    int store_fd = mkstemp(store_path);
    int have_celeste = 0;

    CHECK(store_fd >= 0);
    if (store_fd >= 0)
        close(store_fd);
    for (size_t i = 0; i < sizeof(config_pattern); ++i)
        config_pattern[i] = (unsigned char)(i * 7u + 3u);

    CHECK(p8p_cart_load_text_memory(source, sizeof(source) - 1, &cart) == 0);
    CHECK(cart.lua != NULL);
    CHECK(strstr(cart.lua, "function _update60()") != NULL);
    CHECK(cart.rom[0] == 0x21);
    CHECK(cart.rom[0x3000] == 0x05);
    CHECK(cart.rom[0x2000] == 0x01);
    CHECK(cart.rom[0x3200] == 0x21);
    CHECK(cart.rom[0x3201] == 0x0e);
    CHECK(cart.rom[0x3241] == 0x10);

    CHECK(p8p_cart_load_file(
        ".deps/fake-08/test/carts/cartparsetest.p8.png", &png_cart) == 0);
    CHECK(png_cart.lua != NULL);
    CHECK(strcmp(png_cart.lua, "a=1") == 0 || strcmp(png_cart.lua, "a=1\n") == 0);
    CHECK(png_cart.rom[0] == 0xff);
    CHECK(png_cart.rom[1] == 0x01);
    CHECK(png_cart.rom[0x2000] == 0x01);
    CHECK(png_cart.rom[0x3001] == 0x01);
    CHECK(p8p_cart_load_file(
        ".deps/fake-08/test/carts/test_legacypng_cart.p8.png", &legacy_cart) == 0);
    CHECK(strcmp(legacy_cart.lua, "print(\"0.1.10c\")\n") == 0);
    have_celeste = p8p_cart_load_file("assets/cards/celeste.p8.png",
                                     &celeste_cart) == 0;
    if (have_celeste)
        CHECK(celeste_cart.lua_size > 20000);
    static const uint8_t flip_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "n=0 repeat n+=1 pset(n,1,8) flip() until n==3\n"
        "function _update() n+=1 end\n";
    CHECK(p8p_cart_load_text_memory(flip_source, sizeof(flip_source) - 1,
                                    &flip_cart) == 0);
    static const uint8_t glyph_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "v=0 function _update60() "
        "if btn(\x8b) then v=0 elseif btn(\x91) then v=1 "
        "elseif btn(\x94) then v=2 elseif btn(\x83) then v=3 "
        "elseif btn(\x8e) then v=4 elseif btn(\x97) then v=5 "
        "elseif btn(6) then v=6 end end\n"
        "function _draw() cls() pset(v,0,7) end\n";
    CHECK(p8p_cart_load_text_memory(glyph_source, sizeof(glyph_source) - 1,
                                    &glyph_cart) == 0);
    static const uint8_t nested_short_if_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "n=0 co=cocreate(function() yield() n+=4 end)\n"
        "function _update60()\n"
        "if(false) if(true) n=99\n"
        "n+=1 if(costatus(co)~='dead') coresume(co)\nend\n"
        "function _draw() cls() pset(n,0,7) end\n";
    CHECK(p8p_cart_load_text_memory(nested_short_if_source,
                                    sizeof(nested_short_if_source) - 1,
                                    &nested_short_if_cart) == 0);
    static const uint8_t live_input_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "n=0 function _update60() while not btn(6) do n+=1 end end\n"
        "function _draw() cls() pset(1,0,7) end\n";
    CHECK(p8p_cart_load_text_memory(live_input_source,
                                    sizeof(live_input_source) - 1,
                                    &live_input_cart) == 0);
    static const uint8_t runaway_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "n=0 while true do n+=1 end\n";
    CHECK(p8p_cart_load_text_memory(runaway_source,
                                    sizeof(runaway_source) - 1,
                                    &runaway_cart) == 0);
    static const uint8_t run_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "function _init() cartdata('run-test') n=dget(0) dset(0,n+1) end\n"
        "function _update() if n==0 then run() end end\n"
        "function _draw() cls() pset(n,0,7) end\n";
    CHECK(p8p_cart_load_text_memory(run_source, sizeof(run_source) - 1,
                                    &run_cart) == 0);
    static const uint8_t env_fallback_source[] =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "function station() return 9 end t={}\n"
        "function sandbox_lookup(_ENV) return type(circfill)=='function' end\n"
        "function _draw() cls() if t.station==nil and sandbox_lookup({}) "
        "then pset(2,0,7) end end\n";
    CHECK(p8p_cart_load_text_memory(env_fallback_source,
                                    sizeof(env_fallback_source) - 1,
                                    &env_fallback_cart) == 0);
    runtime = p8p_runtime_create();
    CHECK(runtime != NULL);
    if (runtime) {
        p8p_runtime_set_cartdata_hooks(runtime, test_cartdata_load,
                                       test_cartdata_save, store_path);
        int loaded = p8p_runtime_load(runtime, &cart);
        if (loaded != 0)
            fprintf(stderr, "runtime load: %s\n", p8p_runtime_error(runtime));
        CHECK(loaded == 0);
        CHECK(p8p_runtime_target_fps(runtime) == 60);
        p8p_runtime_flush_cartdata(runtime);
        unsigned char cartdata_readback[256] = {};
        CHECK(p8p_cartdata_load_file(store_path, "runtime-test",
                                     cartdata_readback,
                                     sizeof(cartdata_readback)) == 0);
        CHECK(cartdata_readback[2] == 1);
        p8p_runtime_set_service_hook(runtime, count_service_hook, NULL);
        memset(profile_event_calls, 0, sizeof(profile_event_calls));
        p8p_runtime_set_profile_hook(runtime, count_profile_event, NULL);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(service_hook_calls > 0);
        CHECK(profile_event_calls[P8P_PROFILE_UPDATE_BEGIN] == 1);
        CHECK(profile_event_calls[P8P_PROFILE_UPDATE_END] == 1);
        CHECK(profile_event_calls[P8P_PROFILE_DRAW_BEGIN] == 1);
        CHECK(profile_event_calls[P8P_PROFILE_DRAW_END] == 1);
        p8p_runtime_get_api_profile(runtime, &api_profile);
        CHECK(api_profile.calls[P8P_API_SPRITE] > 0);
        CHECK(api_profile.calls[P8P_API_GRAPHICS] > 0);
        CHECK(api_profile.calls[P8P_API_MEMORY] > 0);
        CHECK(api_profile.calls[P8P_API_DRAW_STATE] > 0);
        CHECK(api_profile.calls[P8P_API_TEXT] > 0);
        CHECK(api_profile.calls[P8P_API_INPUT] > 0);
        p8p_runtime_set_profile_hook(runtime, NULL, NULL);
        memset(profile_event_calls, 0, sizeof(profile_event_calls));
        p8p_runtime_set_profile_hook(runtime, count_profile_event, NULL);
        CHECK(p8p_runtime_step_with_draw(runtime, 0, 0) == 0);
        CHECK(profile_event_calls[P8P_PROFILE_UPDATE_BEGIN] == 1);
        CHECK(profile_event_calls[P8P_PROFILE_UPDATE_END] == 1);
        CHECK(profile_event_calls[P8P_PROFILE_DRAW_BEGIN] == 0);
        CHECK(profile_event_calls[P8P_PROFILE_DRAW_END] == 0);
        p8p_runtime_get_api_profile(runtime, &api_profile);
        CHECK(api_profile.calls[P8P_API_INPUT] > 0);
        CHECK(api_profile.calls[P8P_API_SPRITE] == 0);
        CHECK(api_profile.calls[P8P_API_GRAPHICS] == 0);
        CHECK(api_profile.calls[P8P_API_MEMORY] == 0);
        CHECK(api_profile.calls[P8P_API_DRAW_STATE] == 0);
        CHECK(api_profile.calls[P8P_API_TEXT] == 0);
        p8p_runtime_set_profile_hook(runtime, NULL, NULL);
        framebuffer = p8p_runtime_framebuffer(runtime);
        CHECK(framebuffer != NULL);
        CHECK(framebuffer[3 * 128 + 1] == 8);
        CHECK(framebuffer[10 * 128 + 10] == 9);
        CHECK(framebuffer[12 * 128 + 12] == 9);
        CHECK(framebuffer[20 * 128 + 21] == 7);
        CHECK(framebuffer[28 * 128 + 30] == 10);
        CHECK(framebuffer[29 * 128 + 29] == 10);
        CHECK(framebuffer[29 * 128 + 28] == 0);
        CHECK(framebuffer[30 * 128 + 32] == 10);
        CHECK(framebuffer[50 * 128 + 50] == 1);
        CHECK(framebuffer[50 * 128 + 51] == 2);
        CHECK(framebuffer[40 * 128 + 40] == 11);
        CHECK(framebuffer[42 * 128 + 42] == 11);
        CHECK(framebuffer[43 * 128 + 42] == 0);
        CHECK(framebuffer[4 * 128 + 4] == 0);
        CHECK(framebuffer[4 * 128 + 5] == 12);
        CHECK(framebuffer[4 * 128 + 6] == 4);
        CHECK(framebuffer[4 * 128 + 7] == 3);
        CHECK(framebuffer[6 * 128 + 10] == 13);
        CHECK(framebuffer[70 * 128 + 72] == 10);
        CHECK(framebuffer[80 * 128 + 80] == 1);
        CHECK(framebuffer[80 * 128 + 81] == 1);
        CHECK(framebuffer[80 * 128 + 82] == 2);
        CHECK(framebuffer[2 * 128 + 90] == 7);
        CHECK(framebuffer[2 * 128 + 91] == 8);
        CHECK(framebuffer[2 * 128 + 92] == 9);
        CHECK(framebuffer[2 * 128 + 93] == 10);
        screen_palette = p8p_runtime_screen_palette(runtime);
        CHECK(screen_palette != NULL);
        CHECK(screen_palette[0] == 128);
        CHECK(screen_palette[1] == 9);
        CHECK(screen_palette[8] == 12);
        CHECK(screen_palette[15] == 139);
        memset(audio_samples, 0, sizeof(audio_samples));
        p8p_runtime_audio_render(runtime, audio_samples, 512);
        int audible_samples = 0;
        for (int sample = 0; sample < 512; ++sample) {
            CHECK(audio_samples[sample * 2] == audio_samples[sample * 2 + 1]);
            audible_samples += audio_samples[sample * 2] != 0;
        }
        CHECK(audible_samples > 400);

        CHECK(p8p_runtime_save_state(runtime, &saved_state,
                                     &saved_state_size) == 0);
        CHECK(saved_state != NULL);
        CHECK(saved_state_size > 65536);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(p8p_runtime_load_state(runtime, saved_state,
                                     saved_state_size) == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        framebuffer = p8p_runtime_framebuffer(runtime);
        CHECK(framebuffer[3 * 128 + 1] == 8);
        CHECK(framebuffer[3 * 128 + 2] == 0);
        free(saved_state);
        saved_state = NULL;

        p8p_cart_content_hash(&cart, &cart_hash);
        CHECK(p8p_store_write_config_file(store_path, config_pattern,
                                          sizeof(config_pattern)) == 0);
        CHECK(p8p_state_save_file(store_path, &cart_hash, runtime) == 0);
        memset(config_readback, 0, sizeof(config_readback));
        CHECK(p8p_store_read_config_file(store_path, config_readback,
                                         sizeof(config_readback)) == 0);
        CHECK(memcmp(config_pattern, config_readback,
                     sizeof(config_pattern)) == 0);
        CHECK(p8p_state_get_meta_file(store_path, &cart_hash,
                                      &state_meta) == 0);
        CHECK(state_meta.exists == 1);
        CHECK(state_meta.raw_size > 65536);
        CHECK(state_meta.compressed_size < state_meta.raw_size);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(p8p_state_load_file(store_path, &cart_hash, runtime) == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        framebuffer = p8p_runtime_framebuffer(runtime);
        CHECK(framebuffer[3 * 128 + 1] == 8);
        CHECK(framebuffer[3 * 128 + 2] == 0);
        CHECK(p8p_state_delete_file(store_path, &cart_hash) == 0);
        CHECK(p8p_state_get_meta_file(store_path, &cart_hash,
                                      &state_meta) == 0);
        CHECK(state_meta.exists == 0);

        loaded = p8p_runtime_load(runtime, &flip_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1 * 128 + 1] == 8);
        CHECK(p8p_runtime_step_with_draw(runtime, 0, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1 * 128 + 2] == 8);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1 * 128 + 3] == 8);

        loaded = p8p_runtime_load(runtime, &glyph_cart);
        CHECK(loaded == 0);
        for (int button = 0; button < 7; ++button) {
            CHECK(p8p_runtime_step(runtime, (uint8_t)(1u << button)) == 0);
            CHECK(p8p_runtime_framebuffer(runtime)[button] == 7);
        }
        CHECK(p8p_runtime_step_with_draw(runtime, 1u << 0, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[6] == 7);
        CHECK(p8p_runtime_framebuffer(runtime)[0] == 0);
        CHECK(p8p_runtime_step(runtime, 1u << 1) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
        CHECK(p8p_runtime_framebuffer(runtime)[6] == 0);

        loaded = p8p_runtime_load(runtime, &nested_short_if_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[6] == 7);

        loaded = p8p_runtime_load(runtime, &live_input_cart);
        CHECK(loaded == 0);
        service_hook_runtime = runtime;
        inject_pause = 1;
        service_hook_calls = 0;
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(service_hook_calls > 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);
        inject_pause = 0;
        service_hook_runtime = NULL;

        loaded = p8p_runtime_load(runtime, &runaway_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);

        loaded = p8p_runtime_load(runtime, &run_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[1] == 7);

        loaded = p8p_runtime_load(runtime, &env_fallback_cart);
        CHECK(loaded == 0);
        CHECK(p8p_runtime_step(runtime, 0) == 0);
        CHECK(p8p_runtime_framebuffer(runtime)[2] == 7);

        if (have_celeste) {
            loaded = p8p_runtime_load(runtime, &celeste_cart);
            if (loaded != 0)
                fprintf(stderr, "Celeste load: %s\n", p8p_runtime_error(runtime));
            CHECK(loaded == 0);
            CHECK(p8p_runtime_target_fps(runtime) == 30);
            for (int frame = 0; frame < 120 && loaded == 0; ++frame) {
                int step = p8p_runtime_step(runtime,
                                           frame == 90 ? (1u << 4) : 0);
                if (step != 0) {
                    fprintf(stderr, "Celeste frame %d: %s\n", frame,
                            p8p_runtime_error(runtime));
                    loaded = step;
                }
            }
            CHECK(loaded == 0);
            framebuffer = p8p_runtime_framebuffer(runtime);
            unsigned framebuffer_sum = 0;
            for (int pixel = 0; pixel < 128 * 128; ++pixel)
                framebuffer_sum += framebuffer[pixel];
            CHECK(framebuffer_sum != 0);
            CHECK(p8p_runtime_save_state(runtime, &saved_state,
                                         &saved_state_size) == 0);
            if (saved_state) {
                mz_ulong compressed_capacity = mz_compressBound(saved_state_size);
                unsigned char *compressed =
                    (unsigned char *)malloc((size_t)compressed_capacity);
                CHECK(compressed != NULL);
                if (compressed) {
                    CHECK(mz_compress2(compressed, &compressed_capacity,
                                       (const unsigned char *)saved_state,
                                       saved_state_size, MZ_BEST_SPEED) == MZ_OK);
                    printf("Celeste state: %zu raw, %lu compressed\n",
                           saved_state_size,
                           (unsigned long)compressed_capacity);
                    free(compressed);
                }
                CHECK(p8p_runtime_step(runtime, 0) == 0);
                CHECK(p8p_runtime_load_state(runtime, saved_state,
                                             saved_state_size) == 0);
                free(saved_state);
                saved_state = NULL;
            }
        } else {
            puts("Celeste acceptance test skipped (local cart not present)");
        }

        p8p_runtime_destroy(runtime);
    }
    p8p_cart_destroy(&cart);
    p8p_cart_destroy(&png_cart);
    p8p_cart_destroy(&legacy_cart);
    p8p_cart_destroy(&celeste_cart);
    p8p_cart_destroy(&flip_cart);
    p8p_cart_destroy(&glyph_cart);
    p8p_cart_destroy(&nested_short_if_cart);
    p8p_cart_destroy(&live_input_cart);
    p8p_cart_destroy(&runaway_cart);
    p8p_cart_destroy(&run_cart);
    p8p_cart_destroy(&env_fallback_cart);
    if (store_fd >= 0)
        unlink(store_path);

    if (failures) {
        fprintf(stderr, "%d runtime test(s) failed\n", failures);
        return 1;
    }
    puts("z8lua runtime smoke test passed");
    return 0;
}

#undef TEST_NOTES_32
#undef TEST_NOTES_4
