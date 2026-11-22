// DualSense haptics from the game's HD Rumble values. The controller's two voice-coil actuators are channels 3 and 4 of its
// 4-channel USB audio device, so each side's two bands (amplitude 0..1 at a frequency in Hz) are played as two sine waves
// on that side's actuator instead of being collapsed into motor strengths. Same technique as RenHDrum (Apache-2.0) and
// Eden's input_common/helpers/dualsense_haptics; this is an independent implementation. USB only: over Bluetooth the
// controller has no audio device and the normal rumble path stays.
#pragma once

typedef struct { float amp, freq; double phase; } DsBand;
typedef struct { DsBand low, high, low_t, high_t; } DsActuator;
typedef struct { float al, fl, ah, fh; } DsTarget;   // low/high band amplitude and frequency requested for one actuator

#define DS_RATE 48000
#define DS_CHANNELS 4

// Pure: fills `frames` interleaved 4-channel frames (channels 3 and 4 carry the left and right actuator) from the targets,
// advancing the smoothing and phase in act[0] (left) and act[1] (right).
void ds_haptics_render(DsActuator act[2], const DsTarget tgt[2], float *out, int frames);

int ds_haptics_ready(void);   // 1 when a DualSense stream is open (opens it on demand; TOTK_DS_HAPTICS=off closes it)
void ds_haptics_set(int side, float al, float fl, float ah, float fh);   // side 0 left, 1 right
int ds_haptics_probe(void);   // --probe-haptics: plays a test pattern on the DualSense, no game data needed
