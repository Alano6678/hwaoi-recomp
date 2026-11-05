/* Original synthetic audio fixture: actual hwopus.c and libopus, no game data. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/hwopus.c"

static unsigned checks, live;
static IpcSession *sessions[16];
static IpcTransferMemory transfer = {0x100000, 0, 0};
static int expecting_fatal;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
    exit(expecting_fatal ? 42 : 70);
}
void rt_log(const char *fmt, ...) { (void)fmt; }
void ipc_backtrace(void) {}
void ipc_unimplemented(const IpcSession *s, const IpcRequest *q) {
    rt_fatal("%s: unknown command %u", s->name, q->cmd);
}
void plat_mutex_init(PlatMutex *m) { m->p = NULL; }
void plat_mutex_lock(PlatMutex *m) { CHECK(!m->p); m->p = (void *)1; }
void plat_mutex_unlock(PlatMutex *m) { CHECK(m->p); m->p = NULL; }
uint64_t plat_now_ns(void) { static uint64_t t; t += 250000; return t; }
void *ipc_guest(uint64_t a, uint64_t n) {
    CHECK(a && n && n <= 16 * 1024 * 1024); return (void *)(uintptr_t)a;
}
void *kern_handle_get(uint32_t h, int k) { return h == 100 && k == KERN_TMEM ? &transfer : NULL; }
void ipc_out(IpcResponse *r, const void *p, uint32_t n) {
    CHECK(r->data_size + n <= sizeof(r->data)); memcpy(r->data + r->data_size, p, n); r->data_size += n;
}
const void *ipc_arg(const IpcRequest *r, uint32_t o, uint32_t n) {
    if (o > r->data_size || n > r->data_size - o) rt_fatal("hwopus: short argument");
    return r->data + o;
}
uint32_t ipc_session_new(const char *name, IpcHandler h, void *state, void (*close)(void *)) {
    for (unsigned i = 1; i < 16; i++) if (!sessions[i]) {
        sessions[i] = calloc(1, sizeof(IpcSession)); CHECK(sessions[i]);
        *sessions[i] = (IpcSession){.name=name, .handler=h, .state=state, .close=close, .refs=1};
        live++; return i;
    }
    rt_fatal("session table full");
}
static void close_session(uint32_t h) {
    CHECK(h && h < 16 && sessions[h]); sessions[h]->close(sessions[h]->state);
    free(sessions[h]); sessions[h] = NULL; live--;
}
static IpcSession manager = {.name="hwopus", .handler=hwopus_manager_handler};
static OpusParameters parameters(int rate) {
    OpusParameters p = {.rate=rate, .channels=6, .streams=4, .coupled=2, .mapping={0,4,1,2,3,5}};
    return p;
}
static IpcResponse manager_call(unsigned cmd, OpusParameters *p, uint64_t supplied, uint32_t handle) {
    uint8_t old[0x110]; memcpy(old, p, 16); memcpy(old + 16, p->mapping, 256);
    int extended = cmd == 6 || cmd == 9;
    IpcRequest q = {.type=6, .cmd=cmd, .nx=1, .data=(const uint8_t *)&supplied, .data_size=8,
                    .x={{(uint64_t)(uintptr_t)(extended ? (void *)p : old), extended ? 0x118 : 0x110, 0}}};
    if (cmd == 2 || cmd == 6) { q.ncopy=1; q.copy[0]=handle; }
    IpcResponse r={0}; hwopus_manager_handler(&manager, &q, &r); return r;
}
static uint32_t work_size(OpusParameters *p, int extended) {
    IpcResponse r = manager_call(extended ? 9 : 3, p, 0, 0);
    CHECK(!r.result && r.data_size == 4 && !r.nmove && !r.ncopy);
    uint32_t n; memcpy(&n, r.data, 4); CHECK(n > 0); return n;
}
static uint32_t open_decoder(OpusParameters *p, int extended) {
    transfer.size = (work_size(p, extended) + 4095u) & ~4095u;
    IpcResponse r = manager_call(extended ? 6 : 2, p, transfer.size, 100);
    CHECK(!r.result && r.nmove == 1 && !r.ncopy && !r.data_size);
    return r.move[0];
}
static void put_be32(uint8_t *p, uint32_t n) { p[0]=n>>24; p[1]=n>>16; p[2]=n>>8; p[3]=n; }
static IpcResponse decode(uint32_t h, unsigned cmd, uint8_t reset, const uint8_t *input, size_t ni,
                          int16_t *output, size_t no) {
    IpcRequest q = {.type=6, .cmd=cmd, .data=&reset, .data_size=1, .na=1, .nb=1,
                   .a={{(uint64_t)(uintptr_t)input,ni,0}}, .b={{(uint64_t)(uintptr_t)output,no,0}}};
    IpcResponse r={0}; sessions[h]->handler(sessions[h], &q, &r); return r;
}
static void positive(void) {
    const int rates[]={8000,12000,16000,24000,48000};
    for (unsigned rate_i=0; rate_i<5; rate_i++) {
        OpusParameters p=parameters(rates[rate_i]);
        int state_size=opus_multistream_decoder_get_size(p.streams,p.coupled);
        uint32_t bytes=work_size(&p,1);
        CHECK(bytes == sizeof(HardwareOpus)+2u*state_size+(p.rate*120/1000)*p.channels*2u);
        CHECK(bytes == work_size(&p,0));
        int error=0, frames=p.rate/50;
        OpusMSEncoder *encoder=opus_multistream_encoder_create(p.rate,p.channels,p.streams,p.coupled,
                                                              p.mapping,OPUS_APPLICATION_AUDIO,&error);
        CHECK(encoder && error == OPUS_OK);
        int16_t pcm[960*6], expected[960*6], output[960*6+8];
        for (int i=0; i<frames*6; i++) pcm[i]=(int16_t)((i*719+33*(i%6))%20000-10000);
        uint8_t packet[32768];
        int n=opus_multistream_encode(encoder,pcm,frames,packet+8,sizeof(packet)-16);
        CHECK(n > 0); put_be32(packet,(unsigned)n);
        opus_uint32 range=0;
        CHECK(opus_multistream_encoder_ctl(encoder,OPUS_GET_FINAL_RANGE(&range))==OPUS_OK);
        CHECK(range); put_be32(packet+4,range);
        OpusMSDecoder *reference=opus_multistream_decoder_create(p.rate,p.channels,p.streams,p.coupled,p.mapping,&error);
        CHECK(reference && error==OPUS_OK);
        CHECK(opus_multistream_decode(reference,packet+8,n,expected,frames,0)==frames);
        uint32_t h=open_decoder(&p,1);
        // Size failure and checksum failure cannot alter output OR predictor.
        memset(output,0x55,sizeof(output));
        IpcResponse r=decode(h,9,0,packet,n+8,output,frames*12-1);
        CHECK(r.result==HWOPUS_INVALID_DATA && !r.data_size);
        CHECK((uint16_t)output[0]==0x5555);
        put_be32(packet+4,range^1u);
        r=decode(h,9,0,packet,n+8,output,sizeof(output));
        CHECK(r.result==HWOPUS_INVALID_DATA && !r.data_size && (uint16_t)output[0]==0x5555);
        put_be32(packet+4,range);
        // Extra bytes do not make this consume more than one Nintendo frame.
        r=decode(h,9,0,packet,n+16,output,sizeof(output));
        CHECK(!r.result && r.data_size==16);
        int32_t consumed=0, samples=0; uint64_t us=0;
        memcpy(&consumed,r.data,4); memcpy(&samples,r.data+4,4); memcpy(&us,r.data+8,8);
        CHECK(consumed==n+8 && samples==frames && us==250);
        CHECK(!memcmp(output,expected,frames*12)); CHECK((uint16_t)output[frames*6]==0x5555);
        // Reset restores a fresh predictor and works for both modern commands.
        memset(output,0,sizeof(output)); r=decode(h,7,1,packet,n+8,output,sizeof(output));
        CHECK(!r.result && !memcmp(output,expected,frames*12));
        r=decode(h,9,1,packet,n+8,output,sizeof(output));
        CHECK(!r.result && !memcmp(output,expected,frames*12));
        close_session(h);
        for (int old=0; old<2; old++) {
            h=open_decoder(&p,0); r=decode(h,old ? 5 : 2,0,packet,n+8,output,sizeof(output));
            CHECK(!r.result && r.data_size==(old ? 16u : 8u) && !memcmp(output,expected,frames*12));
            close_session(h);
        }
        // Real channel remapping, including libopus's explicit silent channel.
        uint8_t temp=p.mapping[0]; p.mapping[0]=p.mapping[5]; p.mapping[5]=temp; p.mapping[2]=255;
        h=open_decoder(&p,1); r=decode(h,9,0,packet,n+8,output,sizeof(output));
        CHECK(!r.result);
        for (int f=0; f<frames; f++) {
            CHECK(output[f*6]==expected[f*6+5] && output[f*6+5]==expected[f*6]);
            CHECK(output[f*6+2]==0);
        }
        close_session(h); opus_multistream_decoder_destroy(reference); opus_multistream_encoder_destroy(encoder);
    }
    OpusParameters p=parameters(48000); uint32_t h=open_decoder(&p,1);
    uint8_t junk[16]={0}; int16_t out[32]; memset(out,0x55,sizeof(out));
    for (unsigned n=0; n<9; n++) CHECK(decode(h,9,0,junk,n,out,sizeof(out)).result==HWOPUS_INVALID_DATA);
    put_be32(junk,100); CHECK(decode(h,9,0,junk,sizeof(junk),out,sizeof(out)).result==HWOPUS_INVALID_DATA);
    put_be32(junk,1); junk[8]=0xff; CHECK(decode(h,9,0,junk,9,out,sizeof(out)).result==HWOPUS_INVALID_DATA);
    CHECK((uint16_t)out[0]==0x5555); close_session(h);
    CHECK(!live);
    printf("hwopus: %u checks passed, no live sessions; real libopus multistream PCM\n",checks);
}
static void negative(const char *name) {
    expecting_fatal=1; OpusParameters p=parameters(48000);
    if (!strcmp(name,"option")) p.option=1;
    else if (!strcmp(name,"reserved")) p.reserved=1;
    else if (!strcmp(name,"rate")) p.rate=44100;
    else if (!strcmp(name,"channels")) p.channels=256;
    else if (!strcmp(name,"streams")) p.streams=0;
    else if (!strcmp(name,"coupled")) p.coupled=5;
    else if (!strcmp(name,"mapping")) { p.mapping[0]=8; open_decoder(&p,1); }
    else if (!strcmp(name,"short-tmem")) { transfer.size=4096; manager_call(6,&p,4096,100); }
    else if (!strcmp(name,"bad-handle")) { transfer.size=0x100000; manager_call(6,&p,transfer.size,101); }
    else if (!strcmp(name,"reset")) {
        uint32_t h=open_decoder(&p,1); uint8_t b[16]={0}; int16_t pcm[10]; decode(h,9,2,b,sizeof(b),pcm,sizeof(pcm));
    } else if (!strcmp(name,"shape")) {
        IpcRequest q={.type=6,.cmd=9}; IpcResponse r={0}; hwopus_manager_handler(&manager,&q,&r);
    } else if (!strcmp(name,"unknown-manager")) {
        IpcRequest q={.type=6,.cmd=10}; IpcResponse r={0}; hwopus_manager_handler(&manager,&q,&r);
    } else if (!strcmp(name,"unknown-decoder")) {
        uint32_t h=open_decoder(&p,1); IpcRequest q={.type=6,.cmd=3}; IpcResponse r={0};
        sessions[h]->handler(sessions[h],&q,&r);
    } else rt_fatal("test name not recognized");
    work_size(&p,1); fprintf(stderr,"expected visible rejection did not happen\n"); exit(1);
}
int main(int argc,char **argv) {
    if (argc!=2) return 2;
    if (!strcmp(argv[1],"positive")) positive(); else negative(argv[1]);
    return 0;
}
