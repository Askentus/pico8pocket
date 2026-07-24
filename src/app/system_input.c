#include "p8p/system_input.h"

#include "p8p/platform.h"

enum p8p_system_input_action p8p_system_input_update(
    uint16_t held, uint16_t pressed, int menu_open) {
    int select_x;

    /* Accept either order so the chord remains usable on a slow cart. */
    select_x = (pressed & (P8P_PHYS_SELECT | P8P_PHYS_X)) ==
                   (P8P_PHYS_SELECT | P8P_PHYS_X) ||
               ((held & P8P_PHYS_SELECT) && (pressed & P8P_PHYS_X)) ||
               ((held & P8P_PHYS_X) && (pressed & P8P_PHYS_SELECT));
    if (select_x)
        return P8P_SYSTEM_INPUT_TOGGLE_DIAGNOSTICS;

    if (menu_open)
        return P8P_SYSTEM_INPUT_NONE;

    /* Use the hardware edge, even when the button is no longer held. */
    if (pressed & P8P_PHYS_SELECT)
        return P8P_SYSTEM_INPUT_OPEN_MENU;

    return P8P_SYSTEM_INPUT_NONE;
}
