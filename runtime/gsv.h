// Local Switch 2 graphics-supervisor transport for SDK bootstrap and offline chat.
// GraphicsFrameLengthMode remains unsupported unless explicitly opting in with
// HWAOI_GSV_LOCAL_FRAME_POLICY=1. That experimental path records known opaque
// requests only; it does not emulate console GPU scheduling or change NVN pacing.
#pragma once
#include "ipc.h"
uint32_t gsv_user_session_new(void);
