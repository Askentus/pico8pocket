#ifndef P8P_STATE_STORE_H
#define P8P_STATE_STORE_H

#include "p8p/cart.h"
#include "p8p/runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P8P_STATE_SLOT_COUNT 10
#define P8P_STATE_THUMB_WIDTH 64
#define P8P_STATE_THUMB_HEIGHT 64
#define P8P_STATE_THUMB_SIZE \
    (P8P_STATE_THUMB_WIDTH * P8P_STATE_THUMB_HEIGHT)
#define P8P_STORE_CONFIG_SIZE 8192

enum p8p_state_slot {
    P8P_STATE_QUICK = 0,
    P8P_STATE_NUMBER_1 = 1
};

typedef struct p8p_cart_hash {
    uint8_t bytes[16];
} p8p_cart_hash_t;

typedef struct p8p_state_meta {
    int exists;
    uint64_t sequence;
    uint32_t raw_size;
    uint32_t compressed_size;
    uint8_t thumbnail[P8P_STATE_THUMB_SIZE];
} p8p_state_meta_t;

void p8p_cart_content_hash(const p8p_cart_t *cart, p8p_cart_hash_t *hash);
int p8p_state_save(int slot, const p8p_cart_hash_t *cart_hash,
                   p8p_runtime_t *runtime);
int p8p_state_load(int slot, const p8p_cart_hash_t *cart_hash,
                   p8p_runtime_t *runtime);
int p8p_state_delete(int slot, const p8p_cart_hash_t *cart_hash);
int p8p_state_get_meta(int slot, const p8p_cart_hash_t *cart_hash,
                       p8p_state_meta_t *meta);

/* Path variants keep the packed store independently testable on a desktop. */
int p8p_state_save_file(const char *path, const p8p_cart_hash_t *cart_hash,
                        p8p_runtime_t *runtime);
int p8p_state_load_file(const char *path, const p8p_cart_hash_t *cart_hash,
                        p8p_runtime_t *runtime);
int p8p_state_delete_file(const char *path, const p8p_cart_hash_t *cart_hash);
int p8p_state_get_meta_file(const char *path,
                            const p8p_cart_hash_t *cart_hash,
                            p8p_state_meta_t *meta);
int p8p_store_read_config(void *data, size_t size);
int p8p_store_write_config(const void *data, size_t size);
int p8p_store_read_config_file(const char *path, void *data, size_t size);
int p8p_store_write_config_file(const char *path, const void *data,
                                size_t size);
int p8p_cartdata_load(const char *id, uint8_t *data, size_t size);
int p8p_cartdata_save(const char *id, const uint8_t *data, size_t size);
int p8p_cartdata_load_file(const char *path, const char *id,
                           uint8_t *data, size_t size);
int p8p_cartdata_save_file(const char *path, const char *id,
                           const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
