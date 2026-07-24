#include "p8p/audio.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define P8P_AUDIO_CHANNELS 4
#define P8P_AUDIO_RATE 48000u
#define P8P_SFX_BASE 0x3200u
#define P8P_MUSIC_BASE 0x3100u
#define P8P_SFX_BYTES 68u

typedef struct p8p_audio_channel {
    int sfx;
    int end_note;
    uint8_t note;
    uint8_t is_music;
    uint8_t can_loop;
    uint8_t waveform;
    uint8_t effect;
    uint8_t key;
    uint8_t previous_key;
    uint8_t volume;
    uint8_t previous_volume;
    uint32_t phase;
    uint32_t noise;
    uint32_t note_samples;
    uint32_t sample_in_note;
    int32_t increment;
    int32_t increment_step;
    int32_t volume_q16;
    int32_t volume_step;
    uint32_t vibrato_phase;
} p8p_audio_channel_t;

struct p8p_audio {
    uint8_t *ram;
    p8p_audio_channel_t channels[P8P_AUDIO_CHANNELS];
    int music_pattern;
    int music_count;
    uint8_t music_mask;
    uint32_t music_samples_remaining;
    int32_t music_volume_q24;
    int32_t music_fade_step;
};

/* 440 * 2^((key - 33) / 12), converted to a 32-bit phase step at 48 kHz. */
static const uint32_t note_increment[64] = {
    5852465u, 6200470u, 6569170u, 6959793u, 7373644u, 7812103u, 8276635u, 8768789u,
    9290209u, 9842633u, 10427907u, 11047982u, 11704930u, 12400941u, 13138339u, 13919586u,
    14747287u, 15624207u, 16553270u, 17537579u, 18580418u, 19685267u, 20855814u, 22095965u,
    23409859u, 24801882u, 26276679u, 27839171u, 29494575u, 31248413u, 33106541u, 35075158u,
    37160835u, 39370534u, 41711627u, 44191930u, 46819719u, 49603764u, 52553357u, 55678342u,
    58989149u, 62496826u, 66213081u, 70150316u, 74321671u, 78741067u, 83423255u, 88383859u,
    93639437u, 99207528u, 105106715u, 111356685u, 117978298u, 124993653u, 132426162u, 140300631u,
    148643341u, 157482134u, 166846509u, 176767719u, 187278874u, 198415056u, 210213429u, 222713370u
};

static uint8_t *sfx_data(p8p_audio_t *audio, int sfx) {
    return audio->ram + P8P_SFX_BASE + (unsigned)sfx * P8P_SFX_BYTES;
}

static uint32_t samples_per_note(uint8_t speed) {
    uint32_t actual_speed = speed ? speed : 1u;
    return (actual_speed * 183u * P8P_AUDIO_RATE + 11025u) / 22050u;
}

static int last_audible_note(const uint8_t *sfx) {
    int last = 0;
    for (int note = 0; note < 32; ++note)
        if ((sfx[note * 2 + 1] >> 1) & 7)
            last = note + 1;
    return last;
}

static void stop_channel(p8p_audio_channel_t *channel) {
    channel->sfx = -1;
    channel->volume_q16 = 0;
    channel->volume_step = 0;
}

static void configure_note(p8p_audio_t *audio, p8p_audio_channel_t *channel) {
    uint8_t *sfx;
    uint8_t low;
    uint8_t high;
    int32_t target_increment;
    int32_t target_volume;

    if (channel->sfx < 0 || channel->note >= channel->end_note) {
        stop_channel(channel);
        return;
    }
    sfx = sfx_data(audio, channel->sfx);
    low = sfx[channel->note * 2];
    high = sfx[channel->note * 2 + 1];
    channel->key = low & 0x3f;
    channel->waveform = (uint8_t)(((low >> 6) | ((high & 1) << 2)) & 7);
    channel->volume = (high >> 1) & 7;
    channel->effect = (high >> 4) & 7;
    channel->note_samples = samples_per_note(sfx[65]);
    channel->sample_in_note = 0;
    target_increment = (int32_t)note_increment[channel->key];
    target_volume = (int32_t)channel->volume << 16;
    channel->increment_step = 0;
    channel->volume_step = 0;

    switch (channel->effect) {
    case 1: /* slide from the previous note */
        channel->increment = (int32_t)note_increment[channel->previous_key];
        channel->increment_step =
            (target_increment - channel->increment) / (int32_t)channel->note_samples;
        channel->volume_q16 = channel->previous_volume ?
            (int32_t)channel->previous_volume << 16 : target_volume;
        channel->volume_step =
            (target_volume - channel->volume_q16) / (int32_t)channel->note_samples;
        break;
    case 3: /* drop */
        channel->increment = target_increment;
        channel->increment_step = -target_increment / (int32_t)channel->note_samples;
        channel->volume_q16 = target_volume;
        break;
    case 4: /* fade in */
        channel->increment = target_increment;
        channel->volume_q16 = 0;
        channel->volume_step = target_volume / (int32_t)channel->note_samples;
        break;
    case 5: /* fade out */
        channel->increment = target_increment;
        channel->volume_q16 = target_volume;
        channel->volume_step = -target_volume / (int32_t)channel->note_samples;
        break;
    default:
        channel->increment = target_increment;
        channel->volume_q16 = target_volume;
        break;
    }
}

static void advance_note(p8p_audio_t *audio, p8p_audio_channel_t *channel) {
    uint8_t *sfx;
    uint8_t loop_start;
    uint8_t loop_end;

    if (channel->sfx < 0)
        return;
    channel->previous_key = channel->key;
    channel->previous_volume = channel->volume;
    ++channel->note;
    sfx = sfx_data(audio, channel->sfx);
    loop_start = sfx[66];
    loop_end = sfx[67];
    if (channel->can_loop && loop_end > loop_start && channel->note >= loop_end)
        channel->note = loop_start;
    if (channel->note >= channel->end_note) {
        stop_channel(channel);
        return;
    }
    configure_note(audio, channel);
}

static void launch_sfx(p8p_audio_t *audio, int sfx_index, int channel_index,
                       int offset, int length, int is_music) {
    p8p_audio_channel_t *channel = &audio->channels[channel_index];
    uint8_t *sfx = sfx_data(audio, sfx_index);
    int end_note = 32;

    if (length > 0 && !is_music && offset + length < end_note)
        end_note = offset + length;
    if (!is_music && sfx[67] <= sfx[66]) {
        int audible_end = last_audible_note(sfx);
        if (sfx[67] == 0 && sfx[66] > 0 && sfx[66] < end_note)
            end_note = sfx[66];
        else if (audible_end < end_note)
            end_note = audible_end;
    }

    memset(channel, 0, sizeof(*channel));
    channel->sfx = sfx_index;
    channel->note = (uint8_t)(offset < 0 ? 0 : offset);
    channel->end_note = end_note;
    channel->is_music = (uint8_t)is_music;
    channel->can_loop = 1;
    channel->previous_key = 24;
    channel->noise = 0x9e3779b9u ^ (uint32_t)(channel_index * 0x10203u);
    configure_note(audio, channel);
}

static uint32_t pattern_duration_samples(p8p_audio_t *audio, int pattern) {
    const uint8_t *song = audio->ram + P8P_MUSIC_BASE + (unsigned)pattern * 4u;
    int duration_looping = -1;
    int duration_nonlooping = -1;

    for (int channel = 0; channel < 4; ++channel) {
        int index = song[channel] & 0x7f;
        uint8_t *sfx;
        int end_note;
        int duration;
        if (index & 0x40)
            continue;
        sfx = sfx_data(audio, index & 0x3f);
        if (sfx[67] > sfx[66]) {
            duration = 32 * (sfx[65] ? sfx[65] : 1);
            if (duration > duration_looping) duration_looping = duration;
        } else {
            end_note = (sfx[67] == 0 && sfx[66] > 0) ? sfx[66] : 32;
            duration_nonlooping = end_note * (sfx[65] ? sfx[65] : 1);
            break;
        }
    }
    int units = duration_nonlooping > 0 ? duration_nonlooping : duration_looping;
    if (units <= 0) units = 32;
    return (uint32_t)(((uint64_t)(unsigned)units * 183u * P8P_AUDIO_RATE + 11025u) /
                      22050u);
}

static void start_music_pattern(p8p_audio_t *audio, int pattern) {
    const uint8_t *song;

    for (int channel = 0; channel < 4; ++channel)
        if (audio->channels[channel].is_music)
            stop_channel(&audio->channels[channel]);
    if (pattern < 0 || pattern > 63) {
        audio->music_pattern = -1;
        audio->music_samples_remaining = 0;
        return;
    }

    audio->music_pattern = pattern;
    audio->music_samples_remaining = pattern_duration_samples(audio, pattern);
    song = audio->ram + P8P_MUSIC_BASE + (unsigned)pattern * 4u;
    for (int channel = 0; channel < 4; ++channel) {
        int sfx = song[channel] & 0x7f;
        if (!(sfx & 0x40) && !(audio->music_mask & (1u << channel)))
            launch_sfx(audio, sfx & 0x3f, channel, 0, 0, 1);
    }
}

static void advance_music(p8p_audio_t *audio) {
    const uint8_t *song;
    int next;

    if (audio->music_pattern < 0)
        return;
    song = audio->ram + P8P_MUSIC_BASE + (unsigned)audio->music_pattern * 4u;
    if (song[2] & 0x80) {
        start_music_pattern(audio, -1);
        audio->music_count = -1;
        return;
    }
    next = audio->music_pattern + 1;
    if (song[1] & 0x80) {
        while (next > 0 && !(audio->ram[P8P_MUSIC_BASE + (unsigned)(next - 1) * 4u] & 0x80))
            --next;
    }
    ++audio->music_count;
    start_music_pattern(audio, next > 63 ? -1 : next);
}

static int32_t triangle(uint32_t phase) {
    uint32_t position = phase >> 16;
    int32_t ramp = position < 32768u ? (int32_t)position : (int32_t)(65535u - position);
    return (ramp - 16384) >> 1;
}

static int32_t waveform_sample(p8p_audio_channel_t *channel, int32_t increment) {
    int32_t sample;
    uint32_t phase = channel->phase;
    uint32_t next_phase = phase + (uint32_t)(increment > 0 ? increment : 0);
    channel->phase = next_phase;
    switch (channel->waveform) {
    case 0: /* triangle */
        return triangle(phase);
    case 1: /* tilted saw (integer approximation) */
        sample = (int32_t)(phase >> 16) - 32768;
        return sample < 24576 ? sample >> 2 : (32767 - sample) >> 1;
    case 2: /* saw */
        return (int16_t)(phase >> 16) >> 2;
    case 3: /* square */
        return (phase & 0x80000000u) ? -8192 : 8192;
    case 4: /* pulse */
        return (phase < 0x51000000u) ? 8192 : -8192;
    case 5: /* organ */
        return (triangle(phase) + triangle(phase * 2u) / 2) * 2 / 3;
    case 6: /* noise */
        /* Hold a pseudo-random value for one pitch period. */
        if (next_phase < phase) {
            channel->noise ^= channel->noise << 13;
            channel->noise ^= channel->noise >> 17;
            channel->noise ^= channel->noise << 5;
        }
        return (int16_t)(channel->noise >> 16) >> 2;
    case 7: /* phaser */
        return (triangle(phase) + triangle(phase - phase / 110u)) / 2;
    default:
        return 0;
    }
}

p8p_audio_t *p8p_audio_create(uint8_t *ram) {
    p8p_audio_t *audio = (p8p_audio_t *)calloc(1, sizeof(*audio));
    if (audio)
        p8p_audio_reset(audio, ram);
    return audio;
}

void p8p_audio_destroy(p8p_audio_t *audio) {
    free(audio);
}

void p8p_audio_reset(p8p_audio_t *audio, uint8_t *ram) {
    if (!audio)
        return;
    memset(audio, 0, sizeof(*audio));
    audio->ram = ram;
    audio->music_pattern = -1;
    audio->music_count = -1;
    audio->music_volume_q24 = 1 << 24;
    for (int channel = 0; channel < 4; ++channel)
        audio->channels[channel].sfx = -1;
}

int p8p_audio_sfx(p8p_audio_t *audio, int sfx, int channel,
                  int offset, int length) {
    if (!audio || sfx < -2 || sfx > 63 || channel < -2 || channel > 3 ||
        offset > 31)
        return -1;
    if (channel == -2) {
        for (int i = 0; i < 4; ++i)
            if (audio->channels[i].sfx == sfx)
                stop_channel(&audio->channels[i]);
        return -1;
    }
    if (sfx == -1 || sfx == -2) {
        for (int i = 0; i < 4; ++i) {
            if (channel >= 0 && i != channel)
                continue;
            if (!audio->channels[i].is_music) {
                if (sfx == -1) stop_channel(&audio->channels[i]);
                else audio->channels[i].can_loop = 0;
            }
        }
        return -1;
    }
    if (channel < 0) {
        for (int i = 0; i < 4; ++i)
            if (!(audio->music_mask & (1u << i)) &&
                (audio->channels[i].sfx < 0 || audio->channels[i].sfx == sfx)) {
                channel = i;
                break;
            }
    }
    if (channel < 0)
        for (int i = 0; i < 4; ++i)
            if (!(audio->music_mask & (1u << i))) {
                channel = i;
                break;
            }
    if (channel < 0)
        return -1;
    for (int i = 0; i < 4; ++i)
        if (i != channel && audio->channels[i].sfx == sfx)
            stop_channel(&audio->channels[i]);
    launch_sfx(audio, sfx, channel, offset < 0 ? 0 : offset, length, 0);
    return channel;
}

void p8p_audio_music(p8p_audio_t *audio, int pattern, int fade_ms, int mask) {
    if (!audio || pattern < -1 || pattern > 63)
        return;
    audio->music_mask = (uint8_t)(mask & 15);
    if (pattern < 0 && fade_ms > 0) {
        uint32_t samples = (uint32_t)fade_ms * 48u;
        audio->music_fade_step = -(audio->music_volume_q24 / (int32_t)samples);
        if (audio->music_fade_step == 0)
            audio->music_fade_step = -1;
        return;
    }
    if (pattern >= 0 && fade_ms > 0) {
        uint32_t samples = (uint32_t)fade_ms * 48u;
        audio->music_volume_q24 = 0;
        audio->music_fade_step = (1 << 24) / (int32_t)samples;
        if (audio->music_fade_step == 0)
            audio->music_fade_step = 1;
    } else {
        audio->music_fade_step = 0;
        audio->music_volume_q24 = pattern < 0 ? 0 : 1 << 24;
    }
    audio->music_count = pattern < 0 ? -1 : 0;
    start_music_pattern(audio, pattern);
}

int p8p_audio_channel_sfx(const p8p_audio_t *audio, int channel) {
    if (!audio || channel < 0 || channel >= P8P_AUDIO_CHANNELS)
        return -1;
    return audio->channels[channel].sfx;
}

int p8p_audio_channel_note(const p8p_audio_t *audio, int channel) {
    if (!audio || channel < 0 || channel >= P8P_AUDIO_CHANNELS ||
        audio->channels[channel].sfx < 0)
        return -1;
    return audio->channels[channel].note;
}

int p8p_audio_music_pattern(const p8p_audio_t *audio) {
    return audio ? audio->music_pattern : -1;
}

int p8p_audio_music_count(const p8p_audio_t *audio) {
    return audio ? audio->music_count : -1;
}

void p8p_audio_render(p8p_audio_t *audio, int16_t *stereo, size_t frames) {
    if (!stereo)
        return;
    if (!audio || !audio->ram) {
        memset(stereo, 0, frames * 2 * sizeof(*stereo));
        return;
    }

    for (size_t frame = 0; frame < frames; ++frame) {
        int32_t mix = 0;
        if (audio->music_pattern >= 0) {
            if (audio->music_samples_remaining == 0)
                advance_music(audio);
            if (audio->music_samples_remaining)
                --audio->music_samples_remaining;
        }
        if (audio->music_fade_step) {
            audio->music_volume_q24 += audio->music_fade_step;
            if (audio->music_volume_q24 <= 0) {
                audio->music_volume_q24 = 0;
                audio->music_fade_step = 0;
                start_music_pattern(audio, -1);
            } else if (audio->music_volume_q24 >= (1 << 24)) {
                audio->music_volume_q24 = 1 << 24;
                audio->music_fade_step = 0;
            }
        }

        for (int index = 0; index < 4; ++index) {
            p8p_audio_channel_t *channel = &audio->channels[index];
            int32_t increment;
            int32_t sample;
            int32_t volume;
            if (channel->sfx < 0)
                continue;
            increment = channel->increment;
            if (channel->effect == 2) {
                int32_t lfo = triangle(channel->vibrato_phase);
                channel->vibrato_phase += 671089u; /* 7.5 Hz */
                increment += (int32_t)(((int64_t)increment * lfo) >> 19);
            } else if (channel->effect == 6 || channel->effect == 7) {
                uint8_t *sfx = sfx_data(audio, channel->sfx);
                int rate = (sfx[65] <= 8 ? 2 : 1) *
                           (channel->effect == 6 ? 30 : 15);
                int arp = (int)(channel->sample_in_note /
                                (P8P_AUDIO_RATE / (unsigned)rate)) & 3;
                int note = (channel->note & ~3) | arp;
                uint8_t key = sfx[note * 2] & 0x3f;
                increment = (int32_t)note_increment[key];
            }
            sample = waveform_sample(channel, increment);
            volume = channel->volume_q16;
            if (channel->is_music)
                volume = (volume >> 8) * (audio->music_volume_q24 >> 16);
            mix += sample * ((volume + 4096) >> 13) / 56;
            channel->increment += channel->increment_step;
            channel->volume_q16 += channel->volume_step;
            if (++channel->sample_in_note >= channel->note_samples)
                advance_note(audio, channel);
        }
        if (mix > 32767) mix = 32767;
        if (mix < -32768) mix = -32768;
        stereo[frame * 2] = (int16_t)mix;
        stereo[frame * 2 + 1] = (int16_t)mix;
    }
}

size_t p8p_audio_state_size(void) {
    return sizeof(p8p_audio_t) - offsetof(p8p_audio_t, channels);
}

int p8p_audio_save_state(const p8p_audio_t *audio, void *destination,
                         size_t size) {
    size_t expected = p8p_audio_state_size();
    if (!audio || !destination || size != expected)
        return -1;
    memcpy(destination, &audio->channels, expected);
    return 0;
}

int p8p_audio_load_state(p8p_audio_t *audio, uint8_t *ram,
                         const void *source, size_t size) {
    size_t expected = p8p_audio_state_size();
    if (!audio || !ram || !source || size != expected)
        return -1;
    memcpy(&audio->channels, source, expected);
    audio->ram = ram;
    return 0;
}
