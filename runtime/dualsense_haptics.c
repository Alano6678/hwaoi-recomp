// See dualsense_haptics.h.
#include "dualsense_haptics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"

#define MIN_FREQ 20.0f
#define MAX_FREQ 1000.0f
#define OUTPUT_GAIN 0.8f          // keeps sustained output below the level where the coils heat up and distort
#define SMOOTH 0.0052f            // one pole, about 4 ms at 48 kHz, on amplitude and frequency changes
#define TIMEOUT_MS 2000           // both sides fade out when the game has not updated them for this long

static float amp_curve(float a) { return a > 0 ? sqrtf(a < 1 ? a : 1) : 0; }   // faint values stay above what is felt
static float clamp_freq(float f, float fallback) { return f > 0 ? (f < MIN_FREQ ? MIN_FREQ : f > MAX_FREQ ? MAX_FREQ : f) : fallback; }

static void advance(DsBand *b, const DsBand *t) {
    b->amp += (t->amp - b->amp) * SMOOTH;
    b->freq += (t->freq - b->freq) * SMOOTH;
    b->phase += b->freq / DS_RATE;
    if (b->phase >= 1.0) b->phase -= 1.0;
}

void ds_haptics_render(DsActuator act[2], const DsTarget tgt[2], float *out, int frames) {
    for (int a = 0; a < 2; a++) {
        act[a].low_t = (DsBand){amp_curve(tgt[a].al), clamp_freq(tgt[a].fl, 160), 0};
        act[a].high_t = (DsBand){amp_curve(tgt[a].ah), clamp_freq(tgt[a].fh, 320), 0};
        float sum = act[a].low_t.amp + act[a].high_t.amp;   // both bands share one signal: keep the sum within full scale
        if (sum > 1) act[a].low_t.amp /= sum, act[a].high_t.amp /= sum;
        if (!act[a].low.freq) act[a].low.freq = 160;
        if (!act[a].high.freq) act[a].high.freq = 320;
    }
    memset(out, 0, (size_t)frames * DS_CHANNELS * sizeof *out);
    for (int i = 0; i < frames; i++)
        for (int a = 0; a < 2; a++) {
            advance(&act[a].low, &act[a].low_t);
            advance(&act[a].high, &act[a].high_t);
            double v = act[a].low.amp * sin(6.283185307179586 * act[a].low.phase) +
                       act[a].high.amp * sin(6.283185307179586 * act[a].high.phase);
            out[i * DS_CHANNELS + 2 + a] = (float)(v > 1 ? 1 : v < -1 ? -1 : v) * OUTPUT_GAIN;
        }
}

static struct {
    SDL_AudioStream *stream;
    SDL_AudioDeviceID dev;
    DsActuator act[2];
    DsTarget tgt[2];
    volatile uint64_t last_ms;
    float buf[4096 * DS_CHANNELS];
    uint64_t next_try_ms;
} H;

static void SDLCALL pull(void *ud, SDL_AudioStream *s, int additional, int total) {
    (void)ud, (void)total;
    int frames = additional / (int)(sizeof(float) * DS_CHANNELS);
    if (frames > 4096) frames = 4096;
    if (frames <= 0) return;
    DsTarget t[2] = {H.tgt[0], H.tgt[1]};
    if (SDL_GetTicks() - H.last_ms > TIMEOUT_MS) t[0].al = t[0].ah = t[1].al = t[1].ah = 0;
    ds_haptics_render(H.act, t, H.buf, frames);
    SDL_PutAudioStreamData(s, H.buf, frames * DS_CHANNELS * (int)sizeof(float));
}

// The audio endpoint is named after the USB product; Wine and some Linux setups only show "Wireless Controller". SDL does
// not say which audio device belongs to which gamepad, so haptics need exactly one such device with 4 channels (which also
// skips a DualShock 4's stereo one); otherwise the normal rumble stays.
static int open_stream(void) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return 0;
    int n = 0, found = 0, narrow = 0;
    SDL_AudioDeviceID *devs = SDL_GetAudioPlaybackDevices(&n), dev = 0;
    const char *chosen = "";
    for (int i = 0; devs && i < n; i++) {
        const char *name = SDL_GetAudioDeviceName(devs[i]);
        SDL_AudioSpec sp;
        if (!name || (!strstr(name, "DualSense") && !strstr(name, "Wireless Controller"))) continue;
        if (!SDL_GetAudioDeviceFormat(devs[i], &sp, NULL) || sp.channels < DS_CHANNELS) {
            narrow++;
            continue;
        }
        dev = devs[i], chosen = name, found++;
    }
    if (found != 1) {
        static int hinted;
        if (!found && narrow && !hinted++)   // the card is on a profile without the haptic channels (PipeWire "Default")
            rt_log("haptics: a DualSense audio device is present but has fewer than 4 channels: switch its card profile to "
                   "Direct (pactl set-card-profile <card> Direct) and set its output volume to 100%%");
        if (found > 1) rt_log("haptics: %d DualSense audio devices, using the normal rumble", found);
        SDL_free(devs);
        return 0;
    }
    SDL_AudioSpec spec = {.format = SDL_AUDIO_F32, .channels = DS_CHANNELS, .freq = DS_RATE};
    H.stream = SDL_OpenAudioDeviceStream(dev, &spec, pull, NULL);
    if (!H.stream || !SDL_ResumeAudioStreamDevice(H.stream)) {
        rt_log("haptics: cannot start the stream on \"%s\": %s", chosen, SDL_GetError());
        if (H.stream) SDL_DestroyAudioStream(H.stream);
        H.stream = NULL;
        SDL_free(devs);
        return 0;
    }
    H.dev = dev;
    rt_log("haptics: DualSense haptics on \"%s\"", chosen);
    SDL_free(devs);
    return 1;
}

void ds_haptics_set(int side, float al, float fl, float ah, float fh) {
    if (side < 0 || side > 1) return;
    H.tgt[side] = (DsTarget){isfinite(al) ? al : 0, fl, isfinite(ah) ? ah : 0, fh};
    H.last_ms = SDL_GetTicks();
}

int ds_haptics_ready(void) {
    if (!strcmp(cfg_value("TOTK_DS_HAPTICS"), "off")) {
        if (H.stream) SDL_DestroyAudioStream(H.stream), H.stream = NULL;
        return 0;
    }
    if (H.stream && SDL_GetAudioDeviceName(H.dev)) return 1;
    if (H.stream) {   // the controller was unplugged: its audio device is gone
        rt_log("haptics: the DualSense audio device was removed, using the normal rumble");
        SDL_DestroyAudioStream(H.stream);
        H.stream = NULL;
    }
    uint64_t now = SDL_GetTicks();
    if (now < H.next_try_ms) return 0;
    H.next_try_ms = now + 5000;   // a pad plugged in later is found within 5 s
    return open_stream();
}

// --probe-haptics: plays a test pattern and measures it with the DualSense's own accelerometer (it sits in the middle of the
// controller, so it picks up the actuators shaking it): per step the standard deviation of each axis in milli-g, against a
// silent baseline. The controller should lie still on a table.
static SDL_Gamepad *probe_pad(void) {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return NULL;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    SDL_Gamepad *pad = NULL;
    for (int i = 0; ids && i < n && !pad; i++)
        if (SDL_GetGamepadTypeForID(ids[i]) == SDL_GAMEPAD_TYPE_PS5) pad = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    if (pad && !(SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true))) {
        SDL_CloseGamepad(pad);
        pad = NULL;
    }
    return pad;
}

// Collects accelerometer events for ms; returns the sample count and the per-axis standard deviation (milli-g).
static int measure(int ms, double sd[3]) {
    double sum[3] = {0}, sq[3] = {0};
    int n = 0;
    uint64_t end = SDL_GetTicks() + (uint64_t)ms;
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE && e.gsensor.sensor == SDL_SENSOR_ACCEL) {
                for (int k = 0; k < 3; k++) sum[k] += e.gsensor.data[k], sq[k] += (double)e.gsensor.data[k] * e.gsensor.data[k];
                n++;
            }
        SDL_Delay(1);
    }
    for (int k = 0; k < 3; k++) {
        double m = n ? sum[k] / n : 0, v = n ? sq[k] / n - m * m : 0;
        sd[k] = sqrt(v > 0 ? v : 0) * 1000.0 / 9.80665;
    }
    return n;
}

int ds_haptics_probe(void) {
    if (!open_stream()) {
        printf("probe-haptics: no single DualSense audio device with 4 channels (USB needed)\n");
        return 1;
    }
    SDL_Gamepad *pad = probe_pad();
    printf("probe-haptics: accelerometer %s\n", pad ? "on: keep the controller still on a table" : "not available: playing only");
    double base[3] = {0}, sd[3];
    if (pad) {
        SDL_Delay(500);
        int n = measure(1500, base);
        printf("probe-haptics: baseline (silent): %d samples (%.0f Hz), sd %.2f %.2f %.2f mg\n", n, n / 1.5, base[0], base[1], base[2]);
    }
    struct { const char *what; int side; float al, fl, ah, fh; int ms; } steps[] = {
        {"left, low band 80 Hz", 0, 0.7f, 80, 0, 320, 1500}, {"right, low band 80 Hz", 1, 0.7f, 80, 0, 320, 1500},
        {"both, low band 40 Hz", 2, 0.7f, 40, 0, 320, 1500}, {"both, low band 80 Hz", 2, 0.7f, 80, 0, 320, 1500},
        {"both, band 120 Hz", 2, 0, 160, 0.7f, 120, 1500}, {"both, band 250 Hz (above the sensor's Nyquist)", 2, 0, 160, 0.7f, 250, 1500},
        {"both, 80 Hz at 0.2", 2, 0.2f, 80, 0, 320, 1500}, {"both, 80 Hz at 1.0", 2, 1.0f, 80, 0, 320, 1500},
    };
    for (unsigned i = 0; i < sizeof steps / sizeof *steps; i++) {
        ds_haptics_set(0, 0, 160, 0, 320), ds_haptics_set(1, 0, 160, 0, 320);
        if (steps[i].side != 1) ds_haptics_set(0, steps[i].al, steps[i].fl, steps[i].ah, steps[i].fh);
        if (steps[i].side != 0) ds_haptics_set(1, steps[i].al, steps[i].fl, steps[i].ah, steps[i].fh);
        SDL_Delay(300);   // the stream's buffer and the smoothing settle
        int n = pad ? measure(steps[i].ms, sd) : (SDL_Delay((uint32_t)steps[i].ms), 0);
        if (pad) printf("probe-haptics: %-44s sd %.2f %.2f %.2f mg (%.1fx baseline), %d samples\n", steps[i].what, sd[0], sd[1], sd[2],
                        (sd[0] + sd[1] + sd[2]) / (base[0] + base[1] + base[2] + 1e-9), n);
        else printf("probe-haptics: %s\n", steps[i].what);
        fflush(stdout);
    }
    ds_haptics_set(0, 0, 160, 0, 320), ds_haptics_set(1, 0, 160, 0, 320);
    SDL_Delay(300);
    int n = pad ? measure(1000, sd) : 0;
    if (pad) printf("probe-haptics: silent again: sd %.2f %.2f %.2f mg, %d samples\n", sd[0], sd[1], sd[2], n);
    if (pad) SDL_CloseGamepad(pad);
    SDL_DestroyAudioStream(H.stream);
    H.stream = NULL;
    printf("probe-haptics: done\n");
    return 0;
}
