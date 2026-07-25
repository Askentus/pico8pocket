#include "p8p/settings.h"

#include "p8p/platform.h"

#include <string.h>

#define SETTINGS_VERSION 1u

typedef struct p8p_settings_file {
    uint8_t magic[8];
    uint32_t version;
    uint32_t size;
    p8p_settings_t settings;
} p8p_settings_file_t;

_Static_assert(sizeof(p8p_settings_file_t) <= P8P_STORE_CONFIG_SIZE,
               "settings must fit the reserved store header");

static const uint8_t settings_magic[8] = {
    'P', '8', 'P', 'C', 'O', 'N', 'F', 'G'
};

void p8p_control_profile_defaults(p8p_control_profile_t *profile) {
    if (!profile)
        return;
    memset(profile, 0, sizeof(*profile));
    profile->physical[P8P_ACTION_LEFT] = P8P_PHYS_LEFT;
    profile->physical[P8P_ACTION_RIGHT] = P8P_PHYS_RIGHT;
    profile->physical[P8P_ACTION_UP] = P8P_PHYS_UP;
    profile->physical[P8P_ACTION_DOWN] = P8P_PHYS_DOWN;
    profile->physical[P8P_ACTION_O] = P8P_PHYS_B | P8P_PHYS_Y;
    profile->physical[P8P_ACTION_X] = P8P_PHYS_A | P8P_PHYS_X;
    profile->physical[P8P_ACTION_PAUSE] = P8P_PHYS_START;
}

void p8p_settings_defaults(p8p_settings_t *settings) {
    if (!settings)
        return;
    memset(settings, 0, sizeof(*settings));
    settings->volume = 100;
    settings->scale = 2;
    p8p_control_profile_defaults(&settings->global_controls);
}

int p8p_settings_load(p8p_settings_t *settings) {
    p8p_settings_file_t file;
    if (!settings)
        return -1;
    p8p_settings_defaults(settings);
    if (p8p_store_read_config(&file, sizeof(file)) != 0)
        return 0;
    if (memcmp(file.magic, settings_magic, sizeof(file.magic)) != 0 ||
        file.version != SETTINGS_VERSION || file.size != sizeof(file.settings))
        return 0;
    memcpy(settings, &file.settings, sizeof(*settings));
    settings->language = settings->language != 0;
    settings->muted = settings->muted != 0;
    if (settings->volume > 100) settings->volume = 100;
    /* Scale 3 was the retired FULL SOFT mode. */
    if (settings->scale > 2) settings->scale = 2;
    return 0;
}

int p8p_settings_save(const p8p_settings_t *settings) {
    p8p_settings_file_t file;
    if (!settings)
        return -1;
    memset(&file, 0, sizeof(file));
    memcpy(file.magic, settings_magic, sizeof(file.magic));
    file.version = SETTINGS_VERSION;
    file.size = sizeof(file.settings);
    memcpy(&file.settings, settings, sizeof(file.settings));
    return p8p_store_write_config(&file, sizeof(file));
}

static int hash_equal(const p8p_cart_hash_t *a, const p8p_cart_hash_t *b) {
    return memcmp(a->bytes, b->bytes, sizeof(a->bytes)) == 0;
}

const p8p_control_profile_t *p8p_settings_controls(
    const p8p_settings_t *settings, const p8p_cart_hash_t *hash,
    int *has_override) {
    if (has_override) *has_override = 0;
    if (!settings)
        return NULL;
    if (hash) {
        for (int i = 0; i < P8P_CART_PROFILE_COUNT; ++i) {
            if (settings->carts[i].used &&
                hash_equal(&settings->carts[i].hash, hash)) {
                if (has_override) *has_override = 1;
                return &settings->carts[i].controls;
            }
        }
    }
    return &settings->global_controls;
}

p8p_control_profile_t *p8p_settings_cart_controls(
    p8p_settings_t *settings, const p8p_cart_hash_t *hash, int create) {
    int free_index = -1;
    if (!settings || !hash)
        return NULL;
    for (int i = 0; i < P8P_CART_PROFILE_COUNT; ++i) {
        if (settings->carts[i].used &&
            hash_equal(&settings->carts[i].hash, hash))
            return &settings->carts[i].controls;
        if (!settings->carts[i].used && free_index < 0)
            free_index = i;
    }
    if (!create)
        return NULL;
    if (free_index < 0)
        free_index = 0;
    settings->carts[free_index].used = 1;
    settings->carts[free_index].hash = *hash;
    settings->carts[free_index].controls = settings->global_controls;
    return &settings->carts[free_index].controls;
}

void p8p_settings_remove_cart_controls(p8p_settings_t *settings,
                                       const p8p_cart_hash_t *hash) {
    if (!settings || !hash)
        return;
    for (int i = 0; i < P8P_CART_PROFILE_COUNT; ++i) {
        if (settings->carts[i].used &&
            hash_equal(&settings->carts[i].hash, hash)) {
            memset(&settings->carts[i], 0, sizeof(settings->carts[i]));
            return;
        }
    }
}

uint8_t p8p_controls_map(const p8p_control_profile_t *profile,
                         uint16_t physical) {
    uint8_t result = 0;
    if (!profile)
        return 0;
    for (int action = 0; action < P8P_CONTROL_ACTIONS; ++action)
        if (physical & profile->physical[action])
            result |= (uint8_t)(1u << action);
    return result;
}
