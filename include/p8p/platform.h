#ifndef P8P_PLATFORM_H
#define P8P_PLATFORM_H

#include <stdint.h>

#define P8P_SCREEN_WIDTH  128
#define P8P_SCREEN_HEIGHT 128

enum p8p_button {
    P8P_BTN_LEFT  = 1u << 0,
    P8P_BTN_RIGHT = 1u << 1,
    P8P_BTN_UP    = 1u << 2,
    P8P_BTN_DOWN  = 1u << 3,
    P8P_BTN_O     = 1u << 4,
    P8P_BTN_X     = 1u << 5,
    P8P_BTN_PAUSE = 1u << 6,
    P8P_BTN_MENU  = 1u << 7
};

enum p8p_physical_button {
    P8P_PHYS_LEFT   = 1u << 0,
    P8P_PHYS_RIGHT  = 1u << 1,
    P8P_PHYS_UP     = 1u << 2,
    P8P_PHYS_DOWN   = 1u << 3,
    P8P_PHYS_A      = 1u << 4,
    P8P_PHYS_B      = 1u << 5,
    P8P_PHYS_X      = 1u << 6,
    P8P_PHYS_Y      = 1u << 7,
    P8P_PHYS_L      = 1u << 8,
    P8P_PHYS_R      = 1u << 9,
    P8P_PHYS_START  = 1u << 10,
    P8P_PHYS_SELECT = 1u << 11
};

typedef struct p8p_physical_input {
    uint16_t held;
    uint16_t pressed;
    uint16_t released;
} p8p_physical_input_t;

struct p8p_runtime;

int p8p_platform_init(void);
uint8_t *p8p_platform_framebuffer(void);
void p8p_platform_present(const uint8_t *framebuffer, const uint8_t *palette);
void p8p_platform_audio_pump(struct p8p_runtime *runtime);
void p8p_platform_audio_set_paused(int paused);
void p8p_platform_audio_set_volume(unsigned percent);
unsigned p8p_platform_set_scale(unsigned scale);
uint8_t p8p_platform_dim_color(uint8_t color);
void p8p_platform_poll_input(p8p_physical_input_t *input);
uint16_t p8p_platform_poll_physical(void);
uint8_t p8p_platform_poll_buttons(void);
void p8p_platform_wait_frame(void);
uint32_t p8p_platform_time_us(void);
void p8p_platform_sleep_us(uint32_t microseconds);
void p8p_platform_exit(void);

#endif
