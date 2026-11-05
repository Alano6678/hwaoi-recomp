// HWAoI offline GameChat user-service subset. See docs/hwaoi/chat-service.md.
#pragma once
#include "ipc.h"
void chat_user_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response);
uint32_t chat_requested_sharing_mode(void);
int chat_requested_audio_sharing(void);
