// Original host implementation; protocol facts from switchbrew/libnx audout.c/h
// and the target SDK OpenAudioOut wrapper. No system firmware code is used.
// PCM16 snapshots stay owned until SDL consumes their input bytes (not a claim
// about DAC latency). Explicit AUDIO=none uses a real-time sample clock instead.
// Only the default device is exposed; unsupported formats/modes fail visibly.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "audout.h"
#include "cfg.h"
#include "platform/platform.h"

#define OUT_MAX_BUFFERS 64
#define OUT_MAX_BYTES (16u * 1024u * 1024u)
typedef struct { uint64_t next, address, capacity, size, offset; } OutWire;
_Static_assert(sizeof(OutWire) == 40, "AudioOutBuffer");
typedef struct { uint64_t tag, bytes, used; uint8_t *data; } OutBuffer;
typedef struct {
    PlatMutex lock;
    PlatThread *thread;
    SDL_AudioStream *stream;
    volatile int32_t running;
    uint32_t event, state, rate, channels;
    uint64_t played, last_ns, fraction, submitted, consumed;
    float volume;
    OutBuffer pending[OUT_MAX_BUFFERS];
    uint64_t released[OUT_MAX_BUFFERS];
    unsigned npending, nreleased;
} AudioOut;

static void out_need(int ok, const char *what) {
    if (!ok) { ipc_backtrace(); rt_fatal("audout: %s", what); }
}
static IpcBuffer out_input(const IpcRequest *rq) {
    if (rq->na && rq->a[0].size) return rq->a[0];
    return rq->nx ? rq->x[0] : (IpcBuffer){0};
}
static IpcBuffer out_output(const IpcRequest *rq, IpcResponse *rs) {
    if (rq->nb && rq->b[0].size) return rq->b[0];
    if (rq->nc && rq->c[0].size) {
        rs->xout[rs->nxout++] = (IpcBuffer){rq->c[0].addr, rq->c[0].size, 0};
        return rq->c[0];
    }
    return (IpcBuffer){0};
}
// Caller holds lock. Retain release IDs until the guest retrieves them.
static void out_release_head(AudioOut *o) {
    out_need(o->npending && o->nreleased < OUT_MAX_BUFFERS, "release queue overflow");
    o->released[o->nreleased++] = o->pending[0].tag;
    free(o->pending[0].data);
    memmove(o->pending, o->pending + 1, (--o->npending) * sizeof(o->pending[0]));
    kern_event_signal(o->event);
}
static void out_consume(AudioOut *o, uint64_t bytes, int played) {
    uint64_t consumed = 0;
    while (o->npending) {
        OutBuffer *b = &o->pending[0];
        uint64_t n = b->bytes - b->used;
        if (n > bytes) n = bytes;
        b->used += n; bytes -= n; consumed += n;
        if (b->used != b->bytes) break;
        out_release_head(o);
    }
    if (played) o->played += consumed / (2 * o->channels);
}
static void out_tick(AudioOut *o, uint64_t now) {
    uint64_t elapsed = now >= o->last_ns ? now - o->last_ns : 0;
    o->last_ns = now;
    if (o->state || !o->npending) { o->fraction = 0; return; }
    if (o->stream) {
        int queued = SDL_GetAudioStreamQueued(o->stream);
        out_need(queued >= 0 && (uint64_t)queued <= o->submitted - o->consumed, "SDL queue accounting");
        uint64_t at = o->submitted - (uint64_t)queued;
        // SDL input is whole PCM frames. Do not accumulate partial-frame loss.
        at -= at % (2 * o->channels);
        out_consume(o, at - o->consumed, 1);
        o->consumed = at;
    } else {
        // Saturate a long suspension to the bounded queue duration; do not wrap.
        if (elapsed > 100000000000ull) elapsed = 100000000000ull;
        uint64_t scaled = elapsed * o->rate + o->fraction;
        uint64_t frames = scaled / 1000000000ull;
        o->fraction = scaled % 1000000000ull;
        out_consume(o, frames * (2 * o->channels), 1);
    }
}
static void out_worker(void *state) {
    AudioOut *o = state;
    while (plat_cas32(&o->running, 0, 0)) {
        plat_mutex_lock(&o->lock);
        out_tick(o, kern_now_ns());
        plat_mutex_unlock(&o->lock);
        plat_sleep_ms(2);
    }
}
static void out_close(void *state) {
    AudioOut *o = state;
    plat_xchg32(&o->running, 0);
    int away = kern_core_away();
    plat_thread_join(o->thread);
    if (o->stream) SDL_DestroyAudioStream(o->stream);
    kern_core_back(&away);
    for (unsigned i = 0; i < o->npending; i++) free(o->pending[i].data);
    kern_handle_close(o->event);
    rt_log("audout: closed after %llu consumed PCM frames", (unsigned long long)o->played);
    free(o);
}
static void out_append(AudioOut *o, const IpcRequest *rq) {
    uint64_t tag = IPC_ARG(rq, uint64_t, 0);
    IpcBuffer in = out_input(rq);
    out_need(in.size == sizeof(OutWire), "Append buffer descriptor size");
    OutWire w;
    memcpy(&w, ipc_guest(in.addr, in.size), sizeof w);
    out_need(tag != 0, "zero buffer identifier");
    out_need(w.next == 0, "linked audio buffers unsupported");
    out_need(w.address && !(w.address & 0xfff) && w.capacity && !(w.capacity & 0xfff), "sample buffer alignment");
    out_need(w.capacity <= OUT_MAX_BYTES && w.offset <= w.capacity && w.size <= w.capacity - w.offset,
             "sample buffer range");
    out_need(w.size % (2 * o->channels) == 0 && w.offset % (2 * o->channels) == 0, "partial PCM frame");
    // Validate the entire advertised guest range before any pointer arithmetic.
    const uint8_t *source = ipc_guest(w.address, w.capacity);
    out_need(o->npending + o->nreleased < OUT_MAX_BUFFERS, "too many owned buffers");
    uint64_t total = w.size;
    for (unsigned i = 0; i < o->npending; i++) {
        out_need(o->pending[i].tag != tag, "duplicate buffer identifier");
        total += o->pending[i].bytes;
    }
    for (unsigned i = 0; i < o->nreleased; i++) out_need(o->released[i] != tag, "unretrieved buffer identifier");
    out_need(total <= OUT_MAX_BYTES, "PCM queue byte limit");
    uint8_t *copy = malloc(w.size ? (size_t)w.size : 1);
    out_need(copy != NULL, "sample allocation");
    memcpy(copy, source + w.offset, (size_t)w.size);
    if (o->stream && w.size)
        out_need(SDL_PutAudioStreamData(o->stream, copy, (int)w.size), "SDL sample submission failed");
    o->submitted += w.size;
    o->pending[o->npending++] = (OutBuffer){tag, w.size, 0, copy};
}
static void out_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *o = s->state;
    plat_mutex_lock(&o->lock);
    out_tick(o, kern_now_ns());
    switch (rq->cmd) {
    case 0: IPC_OUT(rs, o->state); break;
    case 1:
        if (o->state) {
            if (o->stream) out_need(SDL_ResumeAudioStreamDevice(o->stream), "SDL resume failed");
            o->last_ns = kern_now_ns(); o->fraction = 0; o->state = 0;
            rt_log("audout: started %u Hz PCM16 x%u", o->rate, o->channels);
        }
        break;
    case 2:
        // Stop only the host stream, retaining unconsumed buffers for restart or
        // an explicit Flush. No buffer is falsely reported as played on Stop.
        if (o->stream) out_need(SDL_PauseAudioStreamDevice(o->stream), "SDL pause failed");
        o->state = 1; o->fraction = 0; break;
    case 3: case 7: out_append(o, rq); break;
    case 4: rs->copy[rs->ncopy++] = kern_event_readable(o->event); break;
    case 5: case 8: {
        IpcBuffer out = out_output(rq, rs);
        out_need(out.size >= 8 && out.size % 8 == 0, "released-buffer output size");
        uint32_t n = o->nreleased;
        if (n > out.size / 8) n = (uint32_t)(out.size / 8);
        memcpy(ipc_guest(out.addr, out.size), o->released, (size_t)n * 8);
        memmove(o->released, o->released + n, (o->nreleased - n) * 8);
        o->nreleased -= n;
        if (o->nreleased) kern_event_signal(o->event);
        IPC_OUT(rs, n); break;
    }
    case 6: {
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        uint8_t found = 0;
        for (unsigned i = 0; i < o->npending; i++) found |= o->pending[i].tag == tag;
        IPC_OUT(rs, found); break;
    }
    case 9: { uint32_t n = o->npending; IPC_OUT(rs, n); break; }
    case 10: IPC_OUT(rs, o->played); break;
    case 11: {
        uint8_t flushed = o->state && o->npending;
        if (flushed) {
            if (o->stream) out_need(SDL_ClearAudioStream(o->stream), "SDL flush failed");
            while (o->npending) out_release_head(o);
            o->submitted = o->consumed = o->fraction = 0;
        }
        IPC_OUT(rs, flushed); break;
    }
    case 12: {
        float v = IPC_ARG(rq, float, 0);
        out_need(isfinite(v) && v >= 0 && v <= 1, "volume outside 0..1");
        if (o->stream) out_need(SDL_SetAudioStreamGain(o->stream, v), "SDL gain failed");
        o->volume = v; break;
    }
    case 13: IPC_OUT(rs, o->volume); break;
    default: plat_mutex_unlock(&o->lock); ipc_unimplemented(s, rq);
    }
    plat_mutex_unlock(&o->lock);
}
void audout_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 2: {
        IpcBuffer out = out_output(rq, rs);
        uint32_t count = out.size >= 0x100 ? 1 : 0;
        if (count) { char *p = ipc_guest(out.addr, out.size); memset(p, 0, 0x100); memcpy(p, "DeviceOut", 10); }
        IPC_OUT(rs, count); return;
    }
    case 1: case 3: {
        uint32_t rate = IPC_ARG(rq, uint32_t, 0);
        uint16_t channels = IPC_ARG(rq, uint16_t, 4);
        uint16_t reserved = IPC_ARG(rq, uint16_t, 6);
        uint64_t aruid = IPC_ARG(rq, uint64_t, 8);
        out_need(rq->has_pid && rq->ncopy == 1 && rq->copy[0] == 0xffff8001u, "Open requires current-process handle/PID");
        // High half is reserved, not another channel-count field. libnx's
        // historical default sets it to 2 while the low-half count is zero.
        (void)reserved;
        if (!rate) rate = 48000;
        if (!channels) channels = 2;
        out_need(rate == 48000 && (channels == 1 || channels == 2 || channels == 6), "unsupported rate/channel count");
        IpcBuffer in = out_input(rq), out = out_output(rq, rs);
        out_need(in.size && in.size <= 0x100 && out.size >= 0x100, "device name buffers");
        const char *name = ipc_guest(in.addr, in.size);
        out_need(memchr(name, 0, (size_t)in.size) != NULL, "unterminated device name");
        out_need(!*name || !strcmp(name, "DeviceOut"), "unsupported output device");
        char *name_out = ipc_guest(out.addr, out.size);
        memset(name_out, 0, 0x100); memcpy(name_out, "DeviceOut", 10);
        AudioOut *o = calloc(1, sizeof *o);
        out_need(o != NULL, "state allocation");
        o->state = 1; o->rate = rate; o->channels = channels; o->volume = 1;
        o->last_ns = kern_now_ns(); o->event = kern_event_new();
        plat_mutex_init(&o->lock);
        const char *mode = cfg_get("TOTK_AUDIO");
        if (!((mode && !strcmp(mode, "none")) || cfg_get("TOTK_AUDIO_OFF"))) {
            out_need(SDL_InitSubSystem(SDL_INIT_AUDIO), "SDL audio initialization failed (use HWAOI_AUDIO=none for explicit silent mode)");
            SDL_AudioSpec spec = {SDL_AUDIO_S16, (int)channels, (int)rate};
            o->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            out_need(o->stream != NULL, "SDL output device failed (use HWAOI_AUDIO=none for explicit silent mode)");
        }
        o->running = 1;
        o->thread = plat_thread_new(out_worker, o, "audout");
        out_need(o->thread != NULL, "worker creation");
        struct { uint32_t rate, channels, format, state; } info = {rate, channels, 2, 1};
        IPC_OUT(rs, info);
        rs->move[rs->nmove++] = ipc_session_new("IAudioOut", out_handler, o, out_close);
        rt_log("audout: opened %u Hz PCM16 x%u, aruid %#llx, %s", rate, channels,
               (unsigned long long)aruid, o->stream ? "SDL output" : "explicit silent sample clock");
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
