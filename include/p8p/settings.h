#ifndef P8P_SETTINGS_H
#define P8P_SETTINGS_H

#include "p8p/state_store.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P8P_CONTROL_ACTIONS 7
#define P8P_CART_PROFILE_COUNT 192

enum p8p_control_action {
    P8P_ACTION_LEFT = 0,
    P8P_ACTION_RIGHT,
    P8P_ACTION_UP,
    P8P_ACTION_DOWN,
    P8P_ACTION_O,
    P8P_ACTION_X,
    P8P_ACTION_PAUSE
};

typedef struct p8p_control_profile {
    uint16_t physical[P8P_CONTROL_ACTIONS];
} p8p_control_profile_t;

typedef struct p8p_cart_profile {
    p8p_cart_hash_t hash;
    p8p_control_profile_t controls;
    uint8_t used;
    uint8_t reserved;
} p8p_cart_profile_t;

typedef struct p8p_settings {
    uint8_t language;
    uint8_t volume;
    uint8_t muted;
    uint8_t scale;
    p8p_control_profile_t global_controls;
    p8p_cart_profile_t carts[P8P_CART_PROFILE_COUNT];
} p8p_settings_t;

void p8p_settings_defaults(p8p_settings_t *settings);
int p8p_settings_load(p8p_settings_t *settings);
int p8p_settings_save(const p8p_settings_t *settings);
const p8p_control_profile_t *p8p_settings_controls(
    const p8p_settings_t *settings, const p8p_cart_hash_t *hash,
    int *has_override);
p8p_control_profile_t *p8p_settings_cart_controls(
    p8p_settings_t *settings, const p8p_cart_hash_t *hash, int create);
void p8p_settings_remove_cart_controls(p8p_settings_t *settings,
                                       const p8p_cart_hash_t *hash);
void p8p_control_profile_defaults(p8p_control_profile_t *profile);
uint8_t p8p_controls_map(const p8p_control_profile_t *profile,
                         uint16_t physical);

#ifdef __cplusplus
}
#endif

#endif
