#include "p8p/scheduler.h"

int p8p_render_divisor_for_load(int target_fps, uint32_t full_frame_us,
                                uint32_t update_only_us, uint32_t audio_us,
                                uint32_t present_us) {
    uint32_t budget_us = target_fps == 30 ? 30000u : 15000u;
    int maximum_divisor = target_fps == 30 ? 2 : 4;
    uint32_t update_us = update_only_us ? update_only_us : full_frame_us / 4u;
    uint32_t draw_us = full_frame_us > update_us ?
                       full_frame_us - update_us : 0;
    uint32_t fixed_us = update_us + audio_us;
    uint32_t render_us = draw_us + present_us;

    for (int divisor = 1; divisor <= maximum_divisor; ++divisor) {
        uint32_t estimated_us = fixed_us +
            (render_us + (uint32_t)divisor - 1u) / (uint32_t)divisor;
        if (estimated_us <= budget_us)
            return divisor;
    }

    /* If the update itself is already too slow, destroying visible FPS only
     * makes sense when skipped rendering buys a substantial speedup. BAS
     * Escape, for example, spends almost all of its frame in _update and gains
     * very little from rendering one frame in four. UFO is the opposite: its
     * draw pass dominates, so the larger divisor still protects game speed. */
    if (maximum_divisor > 1) {
        uint32_t divisor_us = fixed_us +
            (render_us + (uint32_t)maximum_divisor - 1u) /
            (uint32_t)maximum_divisor;
        uint32_t full_us = fixed_us + render_us;
        if ((uint64_t)full_us * 100u >= (uint64_t)divisor_us * 125u)
            return maximum_divisor;
    }
    return 1;
}
