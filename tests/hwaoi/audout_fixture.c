/* Original synthetic fixture for the actual AudioOut adapter. SDL consumes
 * manually chosen input-byte counts; no device, game or firmware is opened.
 * This checks component protocol/ownership, not OS scheduling or DAC latency. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fixture_check(int ok, int line, const char *what) {
    if (!ok) { fprintf(stderr, "fixture assertion at %d: %s\n", line, what); exit(1); }
}
#define TASSERT(x) fixture_check(!!(x), __LINE__, #x)

#include "../../runtime/audout.c"

enum { FH_SESSION = 1, FH_EVENT_W, FH_EVENT_R, FH_MAX = 512 };
typedef struct { unsigned refs, signals; int set; } FixtureEvent;
typedef struct { int kind; void *ptr; } FixtureHandle;
typedef struct { uintptr_t begin; size_t size; } FixtureRange;
struct PlatThread { PlatThreadFn fn; void *arg; };
struct SDL_AudioStream {
    int queued, paused, writes;
    float gain;
    SDL_AudioSpec spec;
    unsigned char first_write[4096];
    int first_write_size;
};
static FixtureHandle handles[FH_MAX];
static FixtureRange ranges[256];
static unsigned nranges;
static unsigned sessions, events, threads, streams, event_signals;
static uint64_t clock_ns = UINT64_C(1000000000);
static const char *audio_mode = "none";
static jmp_buf fatal_jump;
static int catch_fatal;
static char fatal_text[256];

unsigned rt_trace;
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_logf(const char *fmt, ...) { (void)fmt; }
void ipc_backtrace(void) {}
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fatal_text, sizeof fatal_text, fmt, ap);
    va_end(ap);
    if (catch_fatal) longjmp(fatal_jump, 1);
    fprintf(stderr, "unexpected fatal: %s\n", fatal_text);
    exit(70);
}
void ipc_unimplemented(const IpcSession *s, const IpcRequest *q) {
    rt_fatal("unimplemented %s command %u", s->name, q->cmd);
}
const char *cfg_get(const char *name) {
    TASSERT(!strcmp(name, "TOTK_AUDIO") || !strcmp(name, "TOTK_AUDIO_OFF"));
    return !strcmp(name, "TOTK_AUDIO") ? audio_mode : NULL;
}
uint64_t kern_now_ns(void) { return clock_ns; }
int kern_core_away(void) { return 0; }
void kern_core_back(int *away) { TASSERT(*away == 0); }
void plat_mutex_init(PlatMutex *m) { m->p = NULL; }
void plat_mutex_lock(PlatMutex *m) { TASSERT(m->p == NULL); m->p = (void *)1; }
void plat_mutex_unlock(PlatMutex *m) { TASSERT(m->p == (void *)1); m->p = NULL; }
PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    TASSERT(!strcmp(name, "audout"));
    PlatThread *t = malloc(sizeof *t);
    TASSERT(t != NULL);
    *t = (PlatThread){fn, arg};
    threads++;
    return t; /* Deliberately do not execute the worker. */
}
void plat_thread_join(PlatThread *t) { TASSERT(t && threads); threads--; free(t); }
void plat_sleep_ms(uint32_t ms) { (void)ms; rt_fatal("unexpected worker execution"); }

static void allow_range(void *pointer, size_t size) {
    TASSERT(nranges < sizeof ranges / sizeof ranges[0]);
    ranges[nranges++] = (FixtureRange){(uintptr_t)pointer, size};
}
void *ipc_guest(uint64_t address, uint64_t size) {
    for (unsigned i = 0; i < nranges; i++) {
        uint64_t begin = ranges[i].begin, length = ranges[i].size;
        if (address >= begin && address - begin <= length && size <= length - (address - begin))
            return (void *)(uintptr_t)address;
    }
    rt_fatal("fixture guest range rejected");
}
const void *ipc_arg(const IpcRequest *q, uint32_t off, uint32_t size) {
    if (off > q->data_size || size > q->data_size - off) rt_fatal("fixture IPC argument bounds");
    return q->data + off;
}
void ipc_out(IpcResponse *r, const void *p, uint32_t size) {
    TASSERT(size <= sizeof r->data - r->data_size);
    memcpy(r->data + r->data_size, p, size);
    r->data_size += size;
}
static uint32_t handle_new(int kind, void *p) {
    for (uint32_t i = 1; i < FH_MAX; i++) if (!handles[i].ptr) {
        handles[i] = (FixtureHandle){kind, p}; return i;
    }
    rt_fatal("fixture handles exhausted");
}
uint32_t ipc_session_new(const char *name, IpcHandler handler, void *state, void (*close)(void *)) {
    IpcSession *s = calloc(1, sizeof *s);
    TASSERT(s != NULL);
    *s = (IpcSession){.name = name, .handler = handler, .state = state, .close = close, .refs = 1};
    sessions++;
    return handle_new(FH_SESSION, s);
}
uint32_t kern_event_new(void) {
    FixtureEvent *e = calloc(1, sizeof *e);
    TASSERT(e != NULL);
    e->refs = 1; events++;
    return handle_new(FH_EVENT_W, e);
}
uint32_t kern_event_readable(uint32_t writable) {
    TASSERT(writable < FH_MAX && handles[writable].kind == FH_EVENT_W && handles[writable].ptr);
    FixtureEvent *e = handles[writable].ptr;
    e->refs++;
    return handle_new(FH_EVENT_R, e);
}
void kern_event_signal(uint32_t writable) {
    TASSERT(writable < FH_MAX && handles[writable].kind == FH_EVENT_W && handles[writable].ptr);
    FixtureEvent *e = handles[writable].ptr;
    e->set = 1; e->signals++; event_signals++;
}
void kern_handle_close(uint32_t h) {
    TASSERT(h && h < FH_MAX && handles[h].ptr);
    FixtureHandle old = handles[h]; handles[h] = (FixtureHandle){0};
    if (old.kind == FH_SESSION) {
        IpcSession *s = old.ptr;
        TASSERT(s->refs == 1 && sessions);
        s->close(s->state); free(s); sessions--;
    } else {
        FixtureEvent *e = old.ptr;
        TASSERT((old.kind == FH_EVENT_W || old.kind == FH_EVENT_R) && e->refs);
        if (!--e->refs) { TASSERT(events); events--; free(e); }
    }
}

bool SDLCALL SDL_InitSubSystem(SDL_InitFlags flags) { TASSERT(flags == SDL_INIT_AUDIO); return true; }
SDL_AudioStream *SDLCALL SDL_OpenAudioDeviceStream(SDL_AudioDeviceID id, const SDL_AudioSpec *spec,
                                                  SDL_AudioStreamCallback callback, void *userdata) {
    TASSERT(id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK && !callback && !userdata);
    TASSERT(spec->format == SDL_AUDIO_S16 && spec->freq == 48000);
    SDL_AudioStream *s = calloc(1, sizeof *s);
    TASSERT(s != NULL);
    s->paused = 1; s->gain = 1; s->spec = *spec; streams++;
    return s;
}
void SDLCALL SDL_DestroyAudioStream(SDL_AudioStream *s) { TASSERT(s && streams); streams--; free(s); }
bool SDLCALL SDL_PutAudioStreamData(SDL_AudioStream *s, const void *p, int n) {
    TASSERT(s && n >= 0 && n <= 16 * 1024 * 1024);
    if (!s->writes) {
        s->first_write_size = n < (int)sizeof s->first_write ? n : (int)sizeof s->first_write;
        memcpy(s->first_write, p, (size_t)s->first_write_size);
    }
    s->queued += n; s->writes++; return true;
}
int SDLCALL SDL_GetAudioStreamQueued(SDL_AudioStream *s) { TASSERT(s != NULL); return s->queued; }
bool SDLCALL SDL_PauseAudioStreamDevice(SDL_AudioStream *s) { TASSERT(s); s->paused = 1; return true; }
bool SDLCALL SDL_ResumeAudioStreamDevice(SDL_AudioStream *s) { TASSERT(s); s->paused = 0; return true; }
bool SDLCALL SDL_ClearAudioStream(SDL_AudioStream *s) { TASSERT(s); s->queued = 0; return true; }
bool SDLCALL SDL_SetAudioStreamGain(SDL_AudioStream *s, float gain) { TASSERT(s); s->gain = gain; return true; }

static IpcSession manager = {.name = "audout:u", .handler = audout_manager_handler};
static IpcSession *session(uint32_t h) {
    TASSERT(h < FH_MAX && handles[h].kind == FH_SESSION && handles[h].ptr);
    return handles[h].ptr;
}
static IpcResponse call(IpcSession *s, IpcRequest q) {
    IpcResponse r = {0}; s->handler(s, &q, &r); TASSERT(r.result == 0); return r;
}
static uint32_t u32(IpcResponse r) { uint32_t n; TASSERT(r.data_size == 4); memcpy(&n, r.data, 4); return n; }
static uint64_t u64(IpcResponse r) { uint64_t n; TASSERT(r.data_size == 8); memcpy(&n, r.data, 8); return n; }
static uint8_t u8(IpcResponse r) { TASSERT(r.data_size == 1); return r.data[0]; }
static IpcResponse command(IpcSession *s, unsigned cmd) { return call(s, (IpcRequest){.cmd = cmd}); }
static void expect_fatal(IpcSession *s, IpcRequest q, const char *fragment) {
    fatal_text[0] = 0;
    catch_fatal = 1;
    if (!setjmp(fatal_jump)) { IpcResponse r = {0}; s->handler(s, &q, &r); TASSERT(0); }
    catch_fatal = 0;
    TASSERT(strstr(fatal_text, fragment) != NULL);
    /* Rejected calls are isolated from the next case; a fatal terminates the
     * production process while this fixture unwinds only its synthetic lock. */
    if (s != &manager) ((AudioOut *)s->state)->lock.p = NULL;
}
static void clean(void) {
    TASSERT(sessions == 0 && events == 0 && threads == 0 && streams == 0);
    for (unsigned i = 1; i < FH_MAX; i++) TASSERT(handles[i].ptr == NULL);
}
static uint32_t open_out(unsigned cmd, uint32_t rate, uint32_t channels) {
    struct { uint32_t rate, channels; uint64_t aruid; } args = {rate, channels, 0x12345678};
    char input[256] = {0}, output[256]; memset(output, 0xCC, sizeof output);
    allow_range(input, sizeof input); allow_range(output, sizeof output);
    IpcRequest q = {.cmd = cmd, .has_pid = 1, .pid = 0x99, .ncopy = 1,
                    .copy = {0xffff8001u}, .data = (const uint8_t *)&args, .data_size = sizeof args};
    if (cmd == 1) { q.na = q.nb = 1; q.a[0] = (IpcBuffer){(uintptr_t)input, sizeof input};
                   q.b[0] = (IpcBuffer){(uintptr_t)output, sizeof output}; }
    else { q.nx = q.nc = 1; q.x[0] = (IpcBuffer){(uintptr_t)input, sizeof input};
           q.c[0] = (IpcBuffer){(uintptr_t)output, sizeof output}; }
    IpcResponse r = call(&manager, q);
    TASSERT(r.nmove == 1 && r.ncopy == 0 && r.data_size == 16);
    uint32_t info[4]; memcpy(info, r.data, sizeof info);
    TASSERT(info[0] == 48000 && info[1] == ((channels & 0xffff) ? (channels & 0xffff) : 2));
    TASSERT(info[2] == 2 && info[3] == 1 && !strcmp(output, "DeviceOut"));
    if (cmd == 3) TASSERT(r.nxout == 1 && r.xout[0].addr == (uintptr_t)output && r.xout[0].size == 256);
    return r.move[0];
}
static unsigned char *samples(size_t capacity) {
    unsigned char *p = _aligned_malloc(capacity, 4096);
    TASSERT(p && !((uintptr_t)p & 4095));
    memset(p, 0x31, capacity); allow_range(p, capacity); return p;
}
static IpcRequest append_request(uint64_t *tag, OutWire *wire, unsigned cmd) {
    allow_range(wire, sizeof *wire);
    IpcRequest q = {.cmd = cmd, .data = (uint8_t *)tag, .data_size = sizeof *tag};
    if (cmd == 3) { q.na = 1; q.a[0] = (IpcBuffer){(uintptr_t)wire, sizeof *wire}; }
    else { q.nx = 1; q.x[0] = (IpcBuffer){(uintptr_t)wire, sizeof *wire}; }
    return q;
}
static void append(IpcSession *s, uint64_t tag, unsigned char *p, size_t capacity, size_t bytes, size_t offset) {
    OutWire wire = {.address = (uintptr_t)p, .capacity = capacity, .size = bytes, .offset = offset};
    (void)call(s, append_request(&tag, &wire, 7));
}
static void advance(AudioOut *o, uint64_t ns) {
    clock_ns += ns; plat_mutex_lock(&o->lock); out_tick(o, clock_ns); plat_mutex_unlock(&o->lock);
}
static uint32_t released(IpcSession *s, uint64_t *out, size_t size, int pointer) {
    allow_range(out, size);
    IpcRequest q = {.cmd = pointer ? 8 : 5};
    if (pointer) { q.nc = 1; q.c[0] = (IpcBuffer){(uintptr_t)out, size}; }
    else { q.nb = 1; q.b[0] = (IpcBuffer){(uintptr_t)out, size}; }
    IpcResponse r = call(s, q);
    TASSERT(r.ncopy == 0 && r.nmove == 0);
    if (pointer) TASSERT(r.nxout == 1 && r.xout[0].addr == (uintptr_t)out && r.xout[0].size == size);
    else TASSERT(r.nxout == 0);
    return u32(r);
}

static void test_manager(void) {
    for (unsigned cmd = 0; cmd <= 2; cmd += 2) {
        char names[256]; memset(names, 0xCC, sizeof names); allow_range(names, sizeof names);
        IpcRequest q = {.cmd = cmd};
        if (cmd == 0) { q.nb = 1; q.b[0] = (IpcBuffer){(uintptr_t)names, sizeof names}; }
        else { q.nc = 1; q.c[0] = (IpcBuffer){(uintptr_t)names, sizeof names}; }
        IpcResponse r = call(&manager, q);
        TASSERT(u32(r) == 1 && !strcmp(names, "DeviceOut"));
        for (unsigned i = 10; i < sizeof names; i++) TASSERT(names[i] == 0);
        TASSERT(r.nxout == (cmd == 2));
    }
    TASSERT(u32(command(&manager, 0)) == 0);
    uint32_t h = open_out(1, 0, 0x00020000); kern_handle_close(h); clean();
    h = open_out(3, 48000, 6); kern_handle_close(h); clean();
    uint32_t bad_params[4] = {48000, 2, 0, 0};
    IpcRequest bad = {.cmd = 3, .data = (uint8_t *)bad_params, .data_size = 16};
    expect_fatal(&manager, bad, "current-process");
    bad.has_pid = 1; bad.ncopy = 1; bad.copy[0] = 0x1234;
    expect_fatal(&manager, bad, "current-process");
    bad.copy[0] = 0xffff8001; bad_params[0] = 44100;
    expect_fatal(&manager, bad, "rate/channel");
    expect_fatal(&manager, (IpcRequest){.cmd = 99}, "unimplemented"); clean();
}
static void test_fractional(void) {
    uint32_t h = open_out(3, 48000, 2); IpcSession *s = session(h); AudioOut *o = s->state;
    unsigned char *p = samples(4096); (void)command(s, 1);
    advance(o, UINT64_C(10000000000)); /* Empty clock must not credit the next buffer. */
    append(s, 1, p, 4096, 192, 0);
    advance(o, 0); TASSERT(o->played == 0 && o->npending == 1);
    for (unsigned i = 0; i < 10000; i++) advance(o, 100);
    TASSERT(o->played == 48 && o->npending == 0 && o->nreleased == 1);
    TASSERT(u64(command(s, 10)) == 48);
    uint64_t tag = 0; TASSERT(released(s, &tag, sizeof tag, 1) == 1 && tag == 1);
    advance(o, UINT64_C(10000000000));
    append(s, 2, p, 4096, 4, 0); advance(o, 0); TASSERT(o->npending == 1 && o->played == 48);
    advance(o, 20833); TASSERT(o->npending == 1);
    advance(o, 1); TASSERT(o->npending == 0 && o->played == 49);
    kern_handle_close(h); _aligned_free(p); clean();
}
static void test_pause(void) {
    uint32_t h = open_out(3, 48000, 2); IpcSession *s = session(h); AudioOut *o = s->state;
    unsigned char *p = samples(4096); append(s, 10, p, 4096, 8, 0);
    advance(o, UINT64_C(10000000000)); TASSERT(o->played == 0 && o->nreleased == 0);
    TASSERT(u32(command(s, 0)) == 1); (void)command(s, 1); advance(o, 20834);
    TASSERT(o->played == 1 && o->pending[0].used == 4);
    (void)command(s, 2); advance(o, UINT64_C(10000000000));
    TASSERT(o->played == 1 && o->npending == 1 && o->nreleased == 0);
    (void)command(s, 1); advance(o, 20834);
    TASSERT(o->played == 2 && o->npending == 0 && o->nreleased == 1);
    kern_handle_close(h); _aligned_free(p); clean();
}
static void test_snapshot(void) {
    audio_mode = "sdl";
    uint32_t h = open_out(3, 48000, 2); IpcSession *s = session(h); AudioOut *o = s->state;
    unsigned char *p = samples(4096);
    for (unsigned i = 0; i < 8; i++) p[16 + i] = (unsigned char)(0x80 + i);
    append(s, 0xABC, p, 4096, 8, 16);
    TASSERT(o->stream && o->stream->paused && o->stream->first_write_size == 8);
    TASSERT(!memcmp(o->pending[0].data, p + 16, 8));
    TASSERT(!memcmp(o->stream->first_write, p + 16, 8));
    memset(p, 0, 4096); TASSERT(o->pending[0].data[0] == 0x80);
    (void)command(s, 1); TASSERT(!o->stream->paused);
    o->stream->queued = 7; advance(o, 1); TASSERT(o->played == 0 && o->consumed == 0);
    o->stream->queued = 5; advance(o, 1); TASSERT(o->played == 0 && o->consumed == 0);
    o->stream->queued = 4; advance(o, 1); TASSERT(o->played == 1 && o->consumed == 4);
    o->stream->queued = 1; advance(o, 1); TASSERT(o->played == 1 && o->consumed == 4);
    o->stream->queued = 0; advance(o, 1);
    TASSERT(o->played == 2 && o->consumed == 8 && o->npending == 0 && o->nreleased == 1);
    kern_handle_close(h); _aligned_free(p); clean();
}
static void test_invalid(void) {
    uint32_t h = open_out(3, 48000, 2); IpcSession *s = session(h); AudioOut *o = s->state;
    unsigned char *p = samples(4096); uint64_t tag = 1;
    OutWire valid = {.address = (uintptr_t)p, .capacity = 4096, .size = 8, .offset = 4}, w;
    w = valid; w.address++; expect_fatal(s, append_request(&tag, &w, 7), "alignment");
    w = valid; w.capacity = 4095; expect_fatal(s, append_request(&tag, &w, 7), "alignment");
    w = valid; w.offset = 4100; expect_fatal(s, append_request(&tag, &w, 7), "range");
    w = valid; w.offset = UINT64_MAX - 3; expect_fatal(s, append_request(&tag, &w, 7), "range");
    w = valid; w.offset = 4092; expect_fatal(s, append_request(&tag, &w, 7), "range");
    w = valid; w.size = 7; expect_fatal(s, append_request(&tag, &w, 7), "partial PCM");
    w = valid; w.offset = 2; expect_fatal(s, append_request(&tag, &w, 7), "partial PCM");
    w = valid; w.next = 9; expect_fatal(s, append_request(&tag, &w, 7), "linked");
    w = valid; w.address = 0x1000; expect_fatal(s, append_request(&tag, &w, 7), "guest range");
    w = valid; tag = 0; expect_fatal(s, append_request(&tag, &w, 7), "zero buffer"); tag = 1;
    IpcRequest q = append_request(&tag, &w, 7); q.x[0].size--;
    expect_fatal(s, q, "descriptor size");
    TASSERT(o->npending == 0 && o->nreleased == 0 && o->submitted == 0);
    (void)call(s, append_request(&tag, &valid, 3));
    expect_fatal(s, append_request(&tag, &valid, 7), "duplicate");
    TASSERT(o->npending == 1); (void)command(s, 11);
    expect_fatal(s, append_request(&tag, &valid, 7), "unretrieved");
    uint64_t out; TASSERT(released(s, &out, 8, 0) == 1 && out == tag);
    (void)call(s, append_request(&tag, &valid, 7)); TASSERT(o->npending == 1);
    kern_handle_close(h); _aligned_free(p); clean();
}
static void test_capacity(void) {
    uint32_t h = open_out(3, 48000, 2); IpcSession *s = session(h); AudioOut *o = s->state;
    unsigned char *p = samples(4096);
    for (uint64_t tag = 1; tag <= 64; tag++) append(s, tag, p, 4096, 4, 0);
    TASSERT(o->npending == 64 && u32(command(s, 9)) == 64);
    OutWire w = {.address = (uintptr_t)p, .capacity = 4096, .size = 4}; uint64_t tag = 65;
    expect_fatal(s, append_request(&tag, &w, 7), "too many owned");
    TASSERT(u8(command(s, 11)) == 1 && o->nreleased == 64);
    expect_fatal(s, append_request(&tag, &w, 7), "too many owned");
    uint64_t out; TASSERT(released(s, &out, 8, 1) == 1 && out == 1);
    (void)call(s, append_request(&tag, &w, 7)); TASSERT(o->npending == 1 && o->nreleased == 63);
    kern_handle_close(h); _aligned_free(p); clean();
    nranges = 0;
    h = open_out(3, 48000, 2); s = session(h); o = s->state;
    p = samples(16u * 1024u * 1024u); append(s, 100, p, 16u * 1024u * 1024u, 16u * 1024u * 1024u, 0);
    w = (OutWire){.address = (uintptr_t)p, .capacity = 4096, .size = 4}; tag = 101;
    expect_fatal(s, append_request(&tag, &w, 7), "queue byte limit");
    TASSERT(o->npending == 1); kern_handle_close(h); _aligned_free(p); clean();
}
static void test_fifo(void) {
    uint32_t h = open_out(3, 48000, 2); IpcSession *s = session(h); AudioOut *o = s->state;
    unsigned char *p = samples(4096);
    append(s, 100, p, 4096, 4, 0); append(s, 200, p, 4096, 4, 0); append(s, 300, p, 4096, 4, 0);
    IpcResponse event_reply = command(s, 4);
    TASSERT(event_reply.ncopy == 1 && event_reply.nmove == 0);
    uint32_t event_handle = event_reply.copy[0]; FixtureEvent *event = handles[event_handle].ptr;
    TASSERT(!event->set); (void)command(s, 1); advance(o, 1000000);
    TASSERT(o->played == 3 && o->nreleased == 3 && event->signals == 3 && event->set);
    event->set = 0;
    uint64_t out[2] = {UINT64_MAX, UINT64_MAX};
    TASSERT(released(s, out, 8, 1) == 1 && out[0] == 100 && out[1] == UINT64_MAX);
    TASSERT(event->set && event->signals == 4 && o->nreleased == 2);
    event->set = 0;
    TASSERT(released(s, out, sizeof out, 0) == 2 && out[0] == 200 && out[1] == 300);
    TASSERT(!event->set && event->signals == 4 && o->nreleased == 0);
    out[0] = UINT64_MAX; TASSERT(released(s, out, sizeof out, 1) == 0 && out[0] == UINT64_MAX);
    kern_handle_close(event_handle); kern_handle_close(h); _aligned_free(p); clean();
}
static void test_event_life(void) {
    uint32_t h = open_out(3, 48000, 2); IpcSession *s = session(h); AudioOut *o = s->state;
    uint32_t writable = o->event; FixtureEvent *e = handles[writable].ptr;
    IpcResponse first = command(s, 4); TASSERT(first.ncopy == 1 && first.nmove == 0 && e->refs == 2);
    IpcResponse second = {.ncopy = 1, .copy = {0xC0FFEE}};
    IpcRequest q = {.cmd = 4}; s->handler(s, &q, &second);
    TASSERT(second.ncopy == 2 && second.copy[0] == 0xC0FFEE && e->refs == 3);
    TASSERT(first.copy[0] != second.copy[1]);
    kern_handle_close(first.copy[0]); TASSERT(e->refs == 2);
    kern_handle_close(h); TASSERT(!handles[writable].ptr && sessions == 0 && events == 1 && e->refs == 1);
    TASSERT(handles[second.copy[1]].kind == FH_EVENT_R);
    kern_handle_close(second.copy[1]); clean();
}
static void test_controls(void) {
    audio_mode = "sdl";
    uint32_t h = open_out(3, 48000, 2); IpcSession *s = session(h); AudioOut *o = s->state;
    unsigned char *p = samples(4096); append(s, 77, p, 4096, 8, 0);
    uint64_t tag = 77; IpcRequest q = {.cmd = 6, .data = (uint8_t *)&tag, .data_size = 8};
    TASSERT(u8(call(s, q)) == 1); tag = 88; TASSERT(u8(call(s, q)) == 0);
    (void)command(s, 1); TASSERT(u8(command(s, 11)) == 0 && o->npending == 1);
    (void)command(s, 2); TASSERT(o->stream->paused);
    TASSERT(u8(command(s, 11)) == 1 && o->npending == 0 && o->nreleased == 1 && o->played == 0);
    TASSERT(o->stream->queued == 0 && o->submitted == 0 && o->consumed == 0);
    TASSERT(u8(command(s, 11)) == 0);
    float value = 0.25f; q = (IpcRequest){.cmd = 12, .data = (uint8_t *)&value, .data_size = 4};
    (void)call(s, q); TASSERT(o->stream->gain == 0.25f);
    IpcResponse r = command(s, 13); TASSERT(r.data_size == 4); memcpy(&value, r.data, 4); TASSERT(value == 0.25f);
    value = -0.01f; expect_fatal(s, q, "volume outside");
    value = 1.01f; expect_fatal(s, q, "volume outside");
    value = NAN; expect_fatal(s, q, "volume outside");
    uint64_t out; allow_range(&out, sizeof out);
    q = (IpcRequest){.cmd = 8, .nc = 1, .c = {{(uintptr_t)&out, 7}}};
    expect_fatal(s, q, "output size");
    expect_fatal(s, (IpcRequest){.cmd = 99}, "unimplemented");
    kern_handle_close(h); _aligned_free(p); clean();
}

int main(int argc, char **argv) {
    TASSERT(argc == 2);
    if (!strcmp(argv[1], "manager")) test_manager();
    else if (!strcmp(argv[1], "fractional")) test_fractional();
    else if (!strcmp(argv[1], "pause")) test_pause();
    else if (!strcmp(argv[1], "snapshot")) test_snapshot();
    else if (!strcmp(argv[1], "invalid")) test_invalid();
    else if (!strcmp(argv[1], "capacity")) test_capacity();
    else if (!strcmp(argv[1], "fifo")) test_fifo();
    else if (!strcmp(argv[1], "event-life")) test_event_life();
    else if (!strcmp(argv[1], "controls")) test_controls();
    else TASSERT(0);
    puts("audout fixture check passed");
    return 0;
}
