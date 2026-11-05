// Original offline GameChat service adapter, not a network implementation.
// Protocol sources and target SDK evidence are recorded in docs/hwaoi/chat-service.md.
// Only opening the scoped object, its IsChatting event and setting the local
// sharing policy are implemented. Unknown operations remain named failures.
#include <stdlib.h>
#include "chat.h"

typedef struct { uint32_t chatting_event; } ChatScoped;
static uint32_t requested_sharing_mode;
static uint32_t requested_audio_sharing;

uint32_t chat_requested_sharing_mode(void) {
    return __atomic_load_n(&requested_sharing_mode, __ATOMIC_ACQUIRE);
}

int chat_requested_audio_sharing(void) {
    return (int)__atomic_load_n(&requested_audio_sharing, __ATOMIC_ACQUIRE);
}

// This bool policy setter is called by the real SetScreenSharingMode wrapper
// before chat:u command 23. Keep the requested policy without starting capture.
static void set_rtc_screen_sharing_audio(Cpu *c) {
    uint32_t enabled = (uint32_t)arg_x(c, 0) & 1u;
    __atomic_store_n(&requested_audio_sharing, enabled, __ATOMIC_RELEASE);
    rt_log("chat: requested screen-sharing audio %u retained locally (offline)", enabled);
}
HOST_OVERRIDE("_ZN2nn6applet31SetRtcScreenSharingAudioEnabledEb", set_rtc_screen_sharing_audio);

static void scoped_close(void *state) {
    ChatScoped *scoped = state;
    // The writable end belongs to this object. Client readable copies retain
    // their own kernel references and remain valid until the client closes them.
    kern_handle_close(scoped->chatting_event);
    free(scoped);
}

static int no_descriptors(const IpcRequest *request) {
    return !(request->nx || request->na || request->nb || request->nw || request->nc ||
             request->ncopy || request->nmove);
}

static void scoped_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    ChatScoped *state = session->state;
    if (!no_descriptors(request)) ipc_unimplemented(session, request);
    switch (request->cmd) {
    case 1:
        // SDK Initialize attaches this event with manual-clear semantics;
        // IsChatting polls it. Offline state never enters an active chat.
        response->copy[response->ncopy++] = kern_event_readable(state->chatting_event);
        rt_log("chat: attached inactive offline chatting event");
        return;
    default:
        ipc_unimplemented(session, request);
    }
}

void chat_user_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    if (!no_descriptors(request)) ipc_unimplemented(session, request);
    switch (request->cmd) {
    case 0: {
        ChatScoped *scoped = calloc(1, sizeof *scoped);
        if (!scoped) rt_fatal("chat: scoped object allocation failed");
        scoped->chatting_event = kern_event_new();  // unsignalled, owned writable endpoint
        response->move[response->nmove++] = ipc_session_new("chat:u IUserShimScopedObject", scoped_handler,
                                                           scoped, scoped_close);
        rt_log("chat: offline scoped object opened; no chat, microphone, camera or network session");
        return;
    }
    case 23: {
        // The target SDK forwards a u32 ScreenSharingMode. Its complete enum
        // range is not established: retain the requested policy verbatim. There
        // is no peer or capture backend, so this cannot start screen sharing.
        uint32_t mode = IPC_ARG(request, uint32_t, 0);
        __atomic_store_n(&requested_sharing_mode, mode, __ATOMIC_RELEASE);
        rt_log("chat: requested screen-sharing mode %u retained locally (offline, no capture)", mode);
        return;
    }
    default:
        ipc_unimplemented(session, request);
    }
}
