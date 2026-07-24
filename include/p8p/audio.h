#ifndef P8P_AUDIO_H
#define P8P_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p8p_audio p8p_audio_t;

p8p_audio_t *p8p_audio_create(uint8_t *ram);
void p8p_audio_destroy(p8p_audio_t *audio);
void p8p_audio_reset(p8p_audio_t *audio, uint8_t *ram);
int p8p_audio_sfx(p8p_audio_t *audio, int sfx, int channel,
                  int offset, int length);
void p8p_audio_music(p8p_audio_t *audio, int pattern, int fade_ms, int mask);
int p8p_audio_channel_sfx(const p8p_audio_t *audio, int channel);
int p8p_audio_channel_note(const p8p_audio_t *audio, int channel);
int p8p_audio_music_pattern(const p8p_audio_t *audio);
int p8p_audio_music_count(const p8p_audio_t *audio);
void p8p_audio_render(p8p_audio_t *audio, int16_t *stereo, size_t frames);
size_t p8p_audio_state_size(void);
int p8p_audio_save_state(const p8p_audio_t *audio, void *destination,
                         size_t size);
int p8p_audio_load_state(p8p_audio_t *audio, uint8_t *ram,
                         const void *source, size_t size);

#ifdef __cplusplus
}
#endif

#endif
