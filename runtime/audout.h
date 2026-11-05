// Original host AudioOut adapter. Wire ABI follows public libnx audout.c/h.
#pragma once
#include "ipc.h"
void audout_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);
