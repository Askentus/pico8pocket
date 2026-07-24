#ifndef P8P_MENU_H
#define P8P_MENU_H

#include "p8p/runtime.h"
#include "p8p/settings.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p8p_menu p8p_menu_t;

enum p8p_menu_action {
    P8P_MENU_NONE = 0,
    P8P_MENU_CLOSE,
    P8P_MENU_RESTART,
    P8P_MENU_TOGGLE_DIAGNOSTICS,
    P8P_MENU_EXIT
};

p8p_menu_t *p8p_menu_create(p8p_settings_t *settings,
                            const p8p_cart_hash_t *cart_hash,
                            const char *cart_kind);
void p8p_menu_destroy(p8p_menu_t *menu);
void p8p_menu_open(p8p_menu_t *menu, p8p_runtime_t *runtime,
                   uint16_t physical_buttons);
enum p8p_menu_action p8p_menu_update(p8p_menu_t *menu,
                                     p8p_runtime_t *runtime,
                                     uint16_t physical_buttons,
                                     uint16_t pressed_buttons);
const uint8_t *p8p_menu_framebuffer(const p8p_menu_t *menu);

#ifdef __cplusplus
}
#endif

#endif
