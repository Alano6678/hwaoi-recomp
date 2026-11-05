// Original HWAoI host multistream Opus service, implemented with libopus.
// Legacy IPC layout: switchbrew/libnx nx/{source,include/switch}/services/hwopus.{c,h}
// (libnx Authors, ISC; consulted for protocol facts, not copied implementation).
// Extended layout independently checked against the profile-locked SDK:
// 2d25ec/2d250c: manager 9/6, 0x118-byte parameters, mapping at +24;
// 2d2b90/2d3224: decoder 9, reset byte, A input/B output, {s32,s32,u64} reply.
// 2d56c0 multiplies the returned duration by 1000 to form nanoseconds: the IPC
// performance field is microseconds. Here it measures host decode, not DSP time.
// Host state is separate from guest transfer memory. The advertised reservation
// accounts for both transactional decoder states and bounded 120-ms PCM scratch;
// it is not a claim about Nintendo hardware work-buffer size or DSP timing.
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <opus/opus_multistream.h>
#include "hwopus.h"
#include "platform/platform.h"

// SDK DecodeOpusMultiStreamInterleavedWithHardware maps this service result to
// its public invalid-data result (sdk+0x2d5698). Malformed/unsupported IPC is fatal.
#define HWOPUS_INVALID_DATA 0x226fu
typedef struct {
    int32_t rate, channels, streams, coupled;
    uint32_t option, reserved;
    uint8_t mapping[256];
} OpusParameters;
_Static_assert(sizeof(OpusParameters) == 0x118, "extended multistream parameters");
typedef struct {
    PlatMutex lock;
    OpusMSDecoder *decoder, *trial;
    int16_t *pcm;
    int rate, channels, state_size, max_samples;
} HardwareOpus;

static void opus_need(int ok, const char *what) {
    if (!ok) { ipc_backtrace(); rt_fatal("hwopus: %s", what); }
}
static void opus_shape(const IpcRequest *rq) {
    opus_need((rq->type == 4 || rq->type == 6) && !rq->has_pid && !rq->nmove &&
              !rq->nw && !rq->nc, "unsupported request transport/handles/buffers");
}
static OpusParameters opus_parameters(const IpcRequest *rq, int extended) {
    opus_need(rq->nx == 1 && !rq->na && !rq->nb &&
              rq->x[0].size == (extended ? 0x118u : 0x110u), "multistream parameter buffer shape");
    OpusParameters p = {0};
    const uint8_t *wire = ipc_guest(rq->x[0].addr, rq->x[0].size);
    memcpy(&p, wire, 16);
    if (extended) memcpy(&p.option, wire + 16, 8);
    memcpy(p.mapping, wire + (extended ? 24 : 16), 256);
    opus_need(!p.option && !p.reserved, "unsupported multistream option/reserved fields");
    opus_need(p.rate == 8000 || p.rate == 12000 || p.rate == 16000 || p.rate == 24000 || p.rate == 48000,
              "unsupported sample rate");
    opus_need(p.channels >= 1 && p.channels <= 255 && p.streams >= 1 && p.streams <= 255 &&
              p.coupled >= 0 && p.coupled <= p.streams && p.streams + p.coupled <= 255,
              "invalid multistream channel/stream counts");
    return p;
}
static uint32_t opus_work_size(const OpusParameters *p, int *state_size) {
    int n = opus_multistream_decoder_get_size(p->streams, p->coupled);
    opus_need(n > 0, "libopus rejected decoder state size");
    uint64_t size = sizeof(HardwareOpus) + 2ull * (unsigned)n +
                    (uint64_t)(p->rate * 120 / 1000) * (unsigned)p->channels * sizeof(int16_t);
    opus_need(size <= INT_MAX - 4095u, "decoder reservation overflow");
    *state_size = n;
    return (uint32_t)size;
}
static void opus_close(void *state) {
    HardwareOpus *d = state;
    // IPC's final owner release cannot overlap a request holding a session ref.
    free(d->decoder);
    free(d->trial);
    free(d->pcm);
    free(d);
}
static uint32_t opus_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void opus_decoder_handler(IpcSession *session, const IpcRequest *rq, IpcResponse *rs) {
    HardwareOpus *d = session->state;
    opus_shape(rq);
    if (rq->cmd != 2 && rq->cmd != 5 && rq->cmd != 7 && rq->cmd != 9)
        ipc_unimplemented(session, rq);
    opus_need(d && !rq->ncopy && !rq->nx && rq->na == 1 && rq->nb == 1,
              "multistream decode buffer shape");
    unsigned reset = 0;
    if (rq->cmd == 7 || rq->cmd == 9) {
        reset = IPC_ARG(rq, uint8_t, 0);
        opus_need(reset <= 1, "invalid reset flag");
    }
    const IpcBuffer *input = rq->a, *output = rq->b;
    if (input->size < 9 || output->size < (uint64_t)d->channels * 2) {
        rs->result = HWOPUS_INVALID_DATA; return;
    }
    const uint8_t *in = ipc_guest(input->addr, input->size);
    uint32_t length = opus_be32(in), range = opus_be32(in + 4);
    if (!length || length > input->size - 8 || length > INT_MAX - 8u) {
        rs->result = HWOPUS_INVALID_DATA; return;
    }
    int samples = opus_packet_get_nb_samples(in + 8, (opus_int32)length, d->rate);
    if (samples <= 0 || samples > d->max_samples ||
        (uint64_t)samples * (unsigned)d->channels * 2 > output->size) {
        rs->result = HWOPUS_INVALID_DATA; return;
    }
    void *out = ipc_guest(output->addr, (uint64_t)samples * (unsigned)d->channels * 2);
    plat_mutex_lock(&d->lock);
    // libopus explicitly documents MS state as position-independent/copyable.
    // Rejected packets/range checks must not publish partial PCM or poison the
    // persistent predictor. Reset is committed only with a successful decode.
    memcpy(d->trial, d->decoder, (size_t)d->state_size);
    uint64_t start = plat_now_ns();
    int got = reset ? opus_multistream_decoder_ctl(d->trial, OPUS_RESET_STATE) : OPUS_OK;
    if (got == OPUS_OK)
        got = opus_multistream_decode(d->trial, in + 8, (opus_int32)length, d->pcm, samples, 0);
    opus_uint32 final = 0;
    if (got > 0 && opus_multistream_decoder_ctl(d->trial, OPUS_GET_FINAL_RANGE(&final)) != OPUS_OK)
        got = OPUS_INTERNAL_ERROR;
    uint64_t elapsed = plat_now_ns() - start;
    if (got != samples || (range && final != range)) {
        plat_mutex_unlock(&d->lock);
        rt_log("hwopus: rejected packet (libopus %d, final-range %#x vs %#x)", got, final, range);
        rs->result = HWOPUS_INVALID_DATA;
        return;
    }
    memcpy(d->decoder, d->trial, (size_t)d->state_size);
    memcpy(out, d->pcm, (size_t)samples * (unsigned)d->channels * 2);
    plat_mutex_unlock(&d->lock);
    struct { int32_t consumed, samples; uint64_t microseconds; } reply = {
        (int32_t)(length + 8), samples, elapsed / 1000
    };
    ipc_out(rs, &reply, rq->cmd == 2 ? 8 : sizeof(reply));
}

void hwopus_manager_handler(IpcSession *session, const IpcRequest *rq, IpcResponse *rs) {
    opus_shape(rq);
    int opening = rq->cmd == 2 || rq->cmd == 6;
    if (!opening && rq->cmd != 3 && rq->cmd != 9) ipc_unimplemented(session, rq);
    opus_need(rq->ncopy == (unsigned)opening, "unexpected transfer-memory handle count");
    OpusParameters p = opus_parameters(rq, rq->cmd == 6 || rq->cmd == 9);
    int state_size;
    uint32_t work_size = opus_work_size(&p, &state_size);
    if (!opening) { IPC_OUT(rs, work_size); return; }
    uint64_t supplied = rq->cmd == 6 ? IPC_ARG(rq, uint32_t, 0) : IPC_ARG(rq, uint64_t, 0);
    IpcTransferMemory *tmem = kern_handle_get(rq->copy[0], KERN_TMEM);
    opus_need(tmem && !tmem->perm && tmem->addr && !(tmem->addr & 4095) &&
              !(tmem->size & 4095) && supplied >= work_size && supplied <= tmem->size,
              "invalid or undersized transfer memory");
    for (int i = 0; i < p.channels; i++)
        opus_need(p.mapping[i] == 255 || p.mapping[i] < p.streams + p.coupled, "invalid channel mapping");
    HardwareOpus *d = calloc(1, sizeof(*d));
    opus_need(d != NULL, "decoder allocation");
    d->rate = p.rate; d->channels = p.channels; d->state_size = state_size;
    d->max_samples = p.rate * 120 / 1000;
    d->decoder = malloc((size_t)state_size);
    d->trial = malloc((size_t)state_size);
    d->pcm = malloc((size_t)d->max_samples * (unsigned)d->channels * sizeof(int16_t));
    if (!d->decoder || !d->trial || !d->pcm) { opus_close(d); opus_need(0, "decoder buffers allocation"); }
    int result = opus_multistream_decoder_init(d->decoder, p.rate, p.channels, p.streams, p.coupled, p.mapping);
    if (result != OPUS_OK) { opus_close(d); opus_need(0, "libopus rejected multistream initialization"); }
    plat_mutex_init(&d->lock);
    rs->move[rs->nmove++] = ipc_session_new("IHardwareOpusMultiStreamDecoder", opus_decoder_handler, d, opus_close);
    rt_log("hwopus: opened libopus %d Hz x%d, %d streams/%d coupled, reservation %u",
           p.rate, p.channels, p.streams, p.coupled, work_size);
}
