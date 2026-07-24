#ifndef P8P_SYSTEM_INPUT_H
#define P8P_SYSTEM_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum p8p_system_input_action {
    P8P_SYSTEM_INPUT_NONE = 0,
    P8P_SYSTEM_INPUT_OPEN_MENU,
    P8P_SYSTEM_INPUT_TOGGLE_DIAGNOSTICS
};

enum p8p_system_input_action p8p_system_input_update(
    uint16_t held, uint16_t pressed, int menu_open);

#ifdef __cplusplus
}
#endif

#endif
