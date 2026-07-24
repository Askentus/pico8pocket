#include "p8p/state_store.h"

#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORE_CAPACITY (256u * 1024u)
#define STORE_VERSION 2u
#define STORE_HEADER_SIZE 32u
#define STORE_DATA_OFFSET (STORE_HEADER_SIZE + P8P_STORE_CONFIG_SIZE)
#define RECORD_HEADER_SIZE 64u
#define RECORD_MAGIC 0x53523850u /* P8RS */
#define RECORD_TYPE_STATE 0u
#define RECORD_TYPE_CARTDATA 1u

static const uint8_t store_magic[8] = {
    'P', '8', 'P', 'S', 'T', 'O', 'R', 'E'
};

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(const uint8_t *p) {
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

static void write_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void write_u64(uint8_t *p, uint64_t value) {
    write_u32(p, (uint32_t)value);
    write_u32(p + 4, (uint32_t)(value >> 32));
}

static void hash_bytes(uint64_t *a, uint64_t *b, const uint8_t *data,
                       size_t size) {
    while (size--) {
        *a ^= *data;
        *a *= UINT64_C(1099511628211);
        *b ^= (uint64_t)(*data++ + 0x9d);
        *b *= UINT64_C(14029467366897019727);
        *b ^= *b >> 29;
    }
}

void p8p_cart_content_hash(const p8p_cart_t *cart, p8p_cart_hash_t *hash) {
    uint64_t a = UINT64_C(14695981039346656037);
    uint64_t b = UINT64_C(7809847782465536322);
    if (!hash)
        return;
    memset(hash, 0, sizeof(*hash));
    if (!cart)
        return;
    hash_bytes(&a, &b, cart->rom, sizeof(cart->rom));
    if (cart->lua)
        hash_bytes(&a, &b, (const uint8_t *)cart->lua, cart->lua_size);
    write_u64(hash->bytes, a);
    write_u64(hash->bytes + 8, b);
}

static void initialize_store(uint8_t *store) {
    memset(store, 0xff, STORE_CAPACITY);
    memcpy(store, store_magic, sizeof(store_magic));
    write_u32(store + 8, STORE_VERSION);
    write_u32(store + 12, STORE_DATA_OFFSET);
    write_u64(store + 16, 1);
    write_u64(store + 24, 0);
}

static int load_store(const char *path, uint8_t **result) {
    uint8_t *store;
    FILE *file;
    size_t received = 0;

    if (!path || !result)
        return -1;
    store = (uint8_t *)malloc(STORE_CAPACITY);
    if (!store)
        return -2;
    memset(store, 0xff, STORE_CAPACITY);
    file = fopen(path, "rb");
    if (file) {
        received = fread(store, 1, STORE_CAPACITY, file);
        fclose(file);
    }
    if (received < STORE_HEADER_SIZE ||
        memcmp(store, store_magic, sizeof(store_magic)) != 0 ||
        read_u32(store + 8) != STORE_VERSION ||
        read_u32(store + 12) < STORE_DATA_OFFSET ||
        read_u32(store + 12) > STORE_CAPACITY)
        initialize_store(store);
    *result = store;
    return 0;
}

static int save_store(const char *path, uint8_t *store) {
    FILE *file = fopen(path, "r+b");
    size_t written;
    if (!file)
        file = fopen(path, "wb");
    if (!file)
        return -1;
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -2;
    }
    written = fwrite(store, 1, STORE_CAPACITY, file);
    if (fclose(file) != 0 || written != STORE_CAPACITY)
        return -3;
    return 0;
}

static int valid_record(const uint8_t *store, uint32_t offset,
                        uint32_t used, uint32_t *total) {
    uint32_t record_size;
    uint32_t compressed_size;
    uint32_t thumbnail_size;
    if (offset > used || used - offset < RECORD_HEADER_SIZE)
        return 0;
    if (read_u32(store + offset) != RECORD_MAGIC)
        return 0;
    record_size = read_u32(store + offset + 4);
    compressed_size = read_u32(store + offset + 12);
    thumbnail_size = read_u32(store + offset + 48);
    if (thumbnail_size > P8P_STATE_THUMB_SIZE ||
        record_size < RECORD_HEADER_SIZE + thumbnail_size ||
        record_size != RECORD_HEADER_SIZE + thumbnail_size + compressed_size ||
        record_size > used - offset)
        return 0;
    if (total)
        *total = record_size;
    return 1;
}

static int find_record(const uint8_t *store, const p8p_cart_hash_t *hash,
                       uint32_t type,
                       uint32_t *record_offset, uint32_t *record_size) {
    uint32_t used = read_u32(store + 12);
    uint32_t offset = STORE_DATA_OFFSET;
    if (used < STORE_DATA_OFFSET || used > STORE_CAPACITY)
        return 0;
    while (offset < used) {
        uint32_t size;
        if (!valid_record(store, offset, used, &size))
            return 0;
        if (read_u32(store + offset + 52) == type &&
            memcmp(store + offset + 32, hash->bytes, sizeof(hash->bytes)) == 0) {
            if (record_offset) *record_offset = offset;
            if (record_size) *record_size = size;
            return 1;
        }
        offset += size;
    }
    return 0;
}

static void remove_record(uint8_t *store, uint32_t offset, uint32_t size) {
    uint32_t used = read_u32(store + 12);
    memmove(store + offset, store + offset + size, used - offset - size);
    used -= size;
    memset(store + used, 0xff, size);
    write_u32(store + 12, used);
}

static int evict_oldest(uint8_t *store) {
    uint32_t used = read_u32(store + 12);
    uint32_t offset = STORE_DATA_OFFSET;
    uint32_t oldest_offset = 0;
    uint32_t oldest_size = 0;
    uint64_t oldest_sequence = UINT64_MAX;
    while (offset < used) {
        uint32_t size;
        uint64_t sequence;
        if (!valid_record(store, offset, used, &size))
            return -1;
        sequence = read_u64(store + offset + 20);
        if (sequence < oldest_sequence) {
            oldest_sequence = sequence;
            oldest_offset = offset;
            oldest_size = size;
        }
        offset += size;
    }
    if (!oldest_size)
        return -1;
    remove_record(store, oldest_offset, oldest_size);
    return 0;
}

static void make_thumbnail(p8p_runtime_t *runtime, uint8_t *thumbnail) {
    const uint8_t *framebuffer = p8p_runtime_framebuffer(runtime);
    const uint8_t *palette = p8p_runtime_screen_palette(runtime);
    for (int y = 0; y < P8P_STATE_THUMB_HEIGHT; ++y) {
        for (int x = 0; x < P8P_STATE_THUMB_WIDTH; ++x) {
            uint8_t color = framebuffer[(y * 2) * 128 + x * 2] & 15;
            thumbnail[y * P8P_STATE_THUMB_WIDTH + x] =
                palette ? palette[color] : color;
        }
    }
}

int p8p_state_save_file(const char *path, const p8p_cart_hash_t *cart_hash,
                        p8p_runtime_t *runtime) {
    uint8_t *store = NULL;
    void *raw = NULL;
    size_t raw_size = 0;
    mz_ulong compressed_size;
    uint32_t offset;
    uint32_t old_size;
    uint32_t used;
    uint32_t record_size;
    uint64_t sequence;
    int result;

    if (!path || !cart_hash || !runtime)
        return -1;
    if (p8p_runtime_save_state(runtime, &raw, &raw_size) != 0)
        return -2;
    result = load_store(path, &store);
    if (result != 0) {
        free(raw);
        return -3;
    }
    if (find_record(store, cart_hash, RECORD_TYPE_STATE, &offset, &old_size))
        remove_record(store, offset, old_size);
    compressed_size = mz_compressBound(raw_size);
    if (compressed_size > STORE_CAPACITY) {
        result = -4;
        goto done;
    }
    record_size = RECORD_HEADER_SIZE + P8P_STATE_THUMB_SIZE +
                  (uint32_t)compressed_size;
    while (read_u32(store + 12) > STORE_CAPACITY - record_size) {
        if (evict_oldest(store) != 0) {
            result = -5;
            goto done;
        }
    }
    used = read_u32(store + 12);
    if (mz_compress2(store + used + RECORD_HEADER_SIZE + P8P_STATE_THUMB_SIZE,
                     &compressed_size, (const unsigned char *)raw, raw_size,
                     MZ_BEST_SPEED) != MZ_OK) {
        result = -6;
        goto done;
    }
    record_size = RECORD_HEADER_SIZE + P8P_STATE_THUMB_SIZE +
                  (uint32_t)compressed_size;
    sequence = read_u64(store + 16);
    if (!sequence) sequence = 1;
    memset(store + used, 0, RECORD_HEADER_SIZE);
    write_u32(store + used, RECORD_MAGIC);
    write_u32(store + used + 4, record_size);
    write_u32(store + used + 8, (uint32_t)raw_size);
    write_u32(store + used + 12, (uint32_t)compressed_size);
    write_u32(store + used + 16,
              (uint32_t)mz_crc32(0, (const unsigned char *)raw, raw_size));
    write_u64(store + used + 20, sequence);
    memcpy(store + used + 32, cart_hash->bytes, sizeof(cart_hash->bytes));
    write_u32(store + used + 48, P8P_STATE_THUMB_SIZE);
    write_u32(store + used + 52, RECORD_TYPE_STATE);
    make_thumbnail(runtime, store + used + RECORD_HEADER_SIZE);
    write_u32(store + 12, used + record_size);
    write_u64(store + 16, sequence + 1);
    result = save_store(path, store);

done:
    free(raw);
    free(store);
    return result;
}

int p8p_state_load_file(const char *path, const p8p_cart_hash_t *cart_hash,
                        p8p_runtime_t *runtime) {
    uint8_t *store = NULL;
    uint8_t *raw = NULL;
    uint32_t offset;
    uint32_t record_size;
    uint32_t raw_size;
    uint32_t compressed_size;
    mz_ulong output_size;
    int result;

    if (!path || !cart_hash || !runtime)
        return -1;
    if (load_store(path, &store) != 0)
        return -2;
    if (!find_record(store, cart_hash, RECORD_TYPE_STATE,
                     &offset, &record_size)) {
        free(store);
        return -3;
    }
    (void)record_size;
    raw_size = read_u32(store + offset + 8);
    compressed_size = read_u32(store + offset + 12);
    if (!raw_size || raw_size > 16u * 1024u * 1024u) {
        free(store);
        return -4;
    }
    raw = (uint8_t *)malloc(raw_size);
    if (!raw) {
        free(store);
        return -5;
    }
    output_size = raw_size;
    if (mz_uncompress(raw, &output_size,
                      store + offset + RECORD_HEADER_SIZE + P8P_STATE_THUMB_SIZE,
                      compressed_size) != MZ_OK || output_size != raw_size ||
        mz_crc32(0, raw, raw_size) != read_u32(store + offset + 16)) {
        result = -6;
    } else {
        result = p8p_runtime_load_state(runtime, raw, raw_size);
    }
    free(raw);
    free(store);
    return result;
}

int p8p_state_delete_file(const char *path, const p8p_cart_hash_t *cart_hash) {
    uint8_t *store = NULL;
    uint32_t offset;
    uint32_t size;
    int result;
    if (!path || !cart_hash)
        return -1;
    if (load_store(path, &store) != 0)
        return -2;
    if (!find_record(store, cart_hash, RECORD_TYPE_STATE, &offset, &size)) {
        free(store);
        return -3;
    }
    remove_record(store, offset, size);
    result = save_store(path, store);
    free(store);
    return result;
}

int p8p_state_get_meta_file(const char *path,
                            const p8p_cart_hash_t *cart_hash,
                            p8p_state_meta_t *meta) {
    uint8_t *store = NULL;
    uint32_t offset;
    uint32_t size;
    if (!path || !cart_hash || !meta)
        return -1;
    memset(meta, 0, sizeof(*meta));
    if (load_store(path, &store) != 0)
        return -2;
    if (find_record(store, cart_hash, RECORD_TYPE_STATE, &offset, &size)) {
        (void)size;
        meta->exists = 1;
        meta->sequence = read_u64(store + offset + 20);
        meta->raw_size = read_u32(store + offset + 8);
        meta->compressed_size = read_u32(store + offset + 12);
        memcpy(meta->thumbnail, store + offset + RECORD_HEADER_SIZE,
               sizeof(meta->thumbnail));
    }
    free(store);
    return 0;
}

static int slot_path(int slot, char path[16]) {
    if (slot < 0 || slot >= P8P_STATE_SLOT_COUNT)
        return -1;
    snprintf(path, 16, "save:%d", slot);
    return 0;
}

int p8p_state_save(int slot, const p8p_cart_hash_t *cart_hash,
                   p8p_runtime_t *runtime) {
    char path[16];
    return slot_path(slot, path) == 0 ?
        p8p_state_save_file(path, cart_hash, runtime) : -1;
}

int p8p_state_load(int slot, const p8p_cart_hash_t *cart_hash,
                   p8p_runtime_t *runtime) {
    char path[16];
    return slot_path(slot, path) == 0 ?
        p8p_state_load_file(path, cart_hash, runtime) : -1;
}

int p8p_state_delete(int slot, const p8p_cart_hash_t *cart_hash) {
    char path[16];
    return slot_path(slot, path) == 0 ?
        p8p_state_delete_file(path, cart_hash) : -1;
}

int p8p_state_get_meta(int slot, const p8p_cart_hash_t *cart_hash,
                       p8p_state_meta_t *meta) {
    char path[16];
    return slot_path(slot, path) == 0 ?
        p8p_state_get_meta_file(path, cart_hash, meta) : -1;
}

static void cartdata_hash(const char *id, p8p_cart_hash_t *hash) {
    static const uint8_t prefix[] = "pico8pocket-cartdata:";
    uint64_t a = UINT64_C(14695981039346656037);
    uint64_t b = UINT64_C(7809847782465536322);
    memset(hash, 0, sizeof(*hash));
    hash_bytes(&a, &b, prefix, sizeof(prefix) - 1);
    hash_bytes(&a, &b, (const uint8_t *)id, strlen(id));
    write_u64(hash->bytes, a);
    write_u64(hash->bytes + 8, b);
}

int p8p_cartdata_save_file(const char *path, const char *id,
                           const uint8_t *data, size_t size) {
    uint8_t *store = NULL;
    p8p_cart_hash_t hash;
    mz_ulong compressed_size;
    uint32_t offset;
    uint32_t old_size;
    uint32_t used;
    uint32_t record_size;
    uint64_t sequence;
    int result;
    if (!path || !id || !*id || strlen(id) > 64 || !data || size != 0x100)
        return -1;
    if (load_store(path, &store) != 0)
        return -2;
    cartdata_hash(id, &hash);
    if (find_record(store, &hash, RECORD_TYPE_CARTDATA, &offset, &old_size))
        remove_record(store, offset, old_size);
    compressed_size = mz_compressBound(size);
    record_size = RECORD_HEADER_SIZE + (uint32_t)compressed_size;
    while (read_u32(store + 12) > STORE_CAPACITY - record_size) {
        if (evict_oldest(store) != 0) {
            free(store);
            return -3;
        }
    }
    used = read_u32(store + 12);
    if (mz_compress2(store + used + RECORD_HEADER_SIZE, &compressed_size,
                     data, size, MZ_BEST_SPEED) != MZ_OK) {
        free(store);
        return -4;
    }
    record_size = RECORD_HEADER_SIZE + (uint32_t)compressed_size;
    sequence = read_u64(store + 16);
    if (!sequence) sequence = 1;
    memset(store + used, 0, RECORD_HEADER_SIZE);
    write_u32(store + used, RECORD_MAGIC);
    write_u32(store + used + 4, record_size);
    write_u32(store + used + 8, (uint32_t)size);
    write_u32(store + used + 12, (uint32_t)compressed_size);
    write_u32(store + used + 16,
              (uint32_t)mz_crc32(0, data, size));
    write_u64(store + used + 20, sequence);
    memcpy(store + used + 32, hash.bytes, sizeof(hash.bytes));
    write_u32(store + used + 48, 0);
    write_u32(store + used + 52, RECORD_TYPE_CARTDATA);
    write_u32(store + 12, used + record_size);
    write_u64(store + 16, sequence + 1);
    result = save_store(path, store);
    free(store);
    return result;
}

int p8p_cartdata_load_file(const char *path, const char *id,
                           uint8_t *data, size_t size) {
    uint8_t *store = NULL;
    p8p_cart_hash_t hash;
    uint32_t offset;
    uint32_t record_size;
    uint32_t raw_size;
    uint32_t compressed_size;
    mz_ulong output_size;
    int result = -3;
    if (!path || !id || !*id || strlen(id) > 64 || !data || size != 0x100)
        return -1;
    memset(data, 0, size);
    if (load_store(path, &store) != 0)
        return -2;
    cartdata_hash(id, &hash);
    if (!find_record(store, &hash, RECORD_TYPE_CARTDATA,
                     &offset, &record_size))
        goto done;
    (void)record_size;
    raw_size = read_u32(store + offset + 8);
    compressed_size = read_u32(store + offset + 12);
    if (raw_size != size) {
        result = -4;
        goto done;
    }
    output_size = size;
    if (mz_uncompress(data, &output_size,
                      store + offset + RECORD_HEADER_SIZE,
                      compressed_size) != MZ_OK || output_size != size ||
        mz_crc32(0, data, size) != read_u32(store + offset + 16)) {
        memset(data, 0, size);
        result = -5;
    } else {
        result = 0;
    }
done:
    free(store);
    return result;
}

int p8p_cartdata_load(const char *id, uint8_t *data, size_t size) {
    return p8p_cartdata_load_file("save:0", id, data, size);
}

int p8p_cartdata_save(const char *id, const uint8_t *data, size_t size) {
    return p8p_cartdata_save_file("save:0", id, data, size);
}

int p8p_store_read_config_file(const char *path, void *data, size_t size) {
    uint8_t *store = NULL;
    if (!path || !data || size > P8P_STORE_CONFIG_SIZE)
        return -1;
    if (load_store(path, &store) != 0)
        return -2;
    memcpy(data, store + STORE_HEADER_SIZE, size);
    free(store);
    return 0;
}

int p8p_store_write_config_file(const char *path, const void *data,
                                size_t size) {
    uint8_t *store = NULL;
    int result;
    if (!path || !data || size > P8P_STORE_CONFIG_SIZE)
        return -1;
    if (load_store(path, &store) != 0)
        return -2;
    memset(store + STORE_HEADER_SIZE, 0xff, P8P_STORE_CONFIG_SIZE);
    memcpy(store + STORE_HEADER_SIZE, data, size);
    result = save_store(path, store);
    free(store);
    return result;
}

int p8p_store_read_config(void *data, size_t size) {
    return p8p_store_read_config_file("save:0", data, size);
}

int p8p_store_write_config(const void *data, size_t size) {
    return p8p_store_write_config_file("save:0", data, size);
}
