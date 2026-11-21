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
    int n = 0, found = 0;
    SDL_AudioDeviceID *devs = SDL_GetAudioPlaybackDevices(&n), dev = 0;
    const char *chosen = "";
    for (int i = 0; devs && i < n; i++) {
        const char *name = SDL_GetAudioDeviceName(devs[i]);
        SDL_AudioSpec sp;
        if (!name || (!strstr(name, "DualSense") && !strstr(name, "Wireless Controller"))) continue;
        if (!SDL_GetAudioDeviceFormat(devs[i], &sp, NULL) || sp.channels < DS_CHANNELS) continue;
        dev = devs[i], chosen = name, found++;
    }
    if (found != 1) {
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
    if (H.stream) return 1;
    uint64_t now = SDL_GetTicks();
    if (now < H.next_try_ms) return 0;
    H.next_try_ms = now + 5000;   // a pad plugged in later is found within 5 s
    return open_stream();
}

// --probe-haptics: left, right, then both, low then high band, then a sweep.
int ds_haptics_probe(void) {
    if (!open_stream()) {
        printf("probe-haptics: no single DualSense audio device with 4 channels (USB needed)\n");
        return 1;
    }
    struct { const char *what; int side; float al, fl, ah, fh; int ms; } steps[] = {
        {"left, low band 80 Hz", 0, 0.7f, 80, 0, 320, 1200}, {"right, low band 80 Hz", 1, 0.7f, 80, 0, 320, 1200},
        {"both, high band 250 Hz", 2, 0, 160, 0.7f, 250, 1200}, {"both, low and high mixed", 2, 0.4f, 100, 0.4f, 300, 1500},
    };
    for (unsigned i = 0; i < sizeof steps / sizeof *steps; i++) {
        printf("probe-haptics: %s\n", steps[i].what);
        fflush(stdout);
        ds_haptics_set(0, 0, 160, 0, 320), ds_haptics_set(1, 0, 160, 0, 320);
        if (steps[i].side != 1) ds_haptics_set(0, steps[i].al, steps[i].fl, steps[i].ah, steps[i].fh);
        if (steps[i].side != 0) ds_haptics_set(1, steps[i].al, steps[i].fl, steps[i].ah, steps[i].fh);
        SDL_Delay(steps[i].ms);
    }
    printf("probe-haptics: sweep 40 to 400 Hz\n");
    fflush(stdout);
    for (int k = 0; k < 120; k++) {
        float f = 40 + 360.0f * k / 119;
        ds_haptics_set(0, 0, 160, 0.6f, f), ds_haptics_set(1, 0, 160, 0.6f, f);
        SDL_Delay(25);
    }
    ds_haptics_set(0, 0, 160, 0, 320), ds_haptics_set(1, 0, 160, 0, 320);
    SDL_Delay(300);
    SDL_DestroyAudioStream(H.stream);
    H.stream = NULL;
    printf("probe-haptics: done\n");
    return 0;
}
