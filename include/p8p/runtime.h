#ifndef P8P_RUNTIME_H
#define P8P_RUNTIME_H

#include "p8p/cart.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p8p_runtime p8p_runtime_t;
typedef void (*p8p_runtime_service_fn)(void *userdata);
typedef int (*p8p_runtime_cartdata_load_fn)(void *userdata, const char *id,
                                            uint8_t *data, size_t size);
typedef int (*p8p_runtime_cartdata_save_fn)(void *userdata, const char *id,
                                            const uint8_t *data, size_t size);

p8p_runtime_t *p8p_runtime_create(void);
void p8p_runtime_destroy(p8p_runtime_t *runtime);
int p8p_runtime_load(p8p_runtime_t *runtime, const p8p_cart_t *cart);
int p8p_runtime_step(p8p_runtime_t *runtime, uint8_t buttons);
int p8p_runtime_step_with_draw(p8p_runtime_t *runtime, uint8_t buttons,
                               int draw_frame);
void p8p_runtime_set_live_buttons(p8p_runtime_t *runtime, uint8_t buttons);
void p8p_runtime_set_service_hook(p8p_runtime_t *runtime,
                                  p8p_runtime_service_fn callback,
                                  void *userdata);
void p8p_runtime_set_cartdata_hooks(p8p_runtime_t *runtime,
                                    p8p_runtime_cartdata_load_fn load,
                                    p8p_runtime_cartdata_save_fn save,
                                    void *userdata);
void p8p_runtime_flush_cartdata(p8p_runtime_t *runtime);
const uint8_t *p8p_runtime_framebuffer(p8p_runtime_t *runtime);
const uint8_t *p8p_runtime_screen_palette(p8p_runtime_t *runtime);
void p8p_runtime_audio_render(p8p_runtime_t *runtime, int16_t *stereo,
                              size_t frames);
int p8p_runtime_save_state(p8p_runtime_t *runtime, void **data, size_t *size);
int p8p_runtime_load_state(p8p_runtime_t *runtime, const void *data,
                           size_t size);
int p8p_runtime_target_fps(const p8p_runtime_t *runtime);
const char *p8p_runtime_error(const p8p_runtime_t *runtime);
#ifdef P8P_RUNTIME_DEBUG
int p8p_runtime_debug_eval_int(p8p_runtime_t *runtime,
                               const char *expression, int *value);
#endif

#ifdef __cplusplus
}
#endif

#endif
