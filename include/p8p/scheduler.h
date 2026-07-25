#ifndef P8P_SCHEDULER_H
#define P8P_SCHEDULER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int p8p_render_divisor_for_load(int target_fps, uint32_t full_frame_us,
                                uint32_t update_only_us, uint32_t audio_us,
                                uint32_t present_us);

#ifdef __cplusplus
}
#endif

#endif
