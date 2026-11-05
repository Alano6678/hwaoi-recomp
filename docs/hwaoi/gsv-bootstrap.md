# GSV bootstrap bridge

## Observed mismatch

boot-03 reaches the chat scoped event but then faults in SdkMutexType::Lock with
a null argument. The call originates in chat::gsv::Initialize. Static inspection
of the pinned SDK shows GetUserSession (0x412760) returning null while its
initialization field is zero. DKB's nv::InitializeGraphics override records host
state but skips the SDK BootGraphicsSystem path. HWAoI invokes chat immediately
after that override.

The real SDK BootGraphicsSystem calls gsv::BootWithRemoteMemory with literal
MemoryRegion=1. Its bootstrap requests an am graphics-supervisor handle, sends
TIPC 4, 11 and 101, and installs its own UserSession wrapper. The fork calls this
SDK function and supplies a raw IPC transport through the exported am accessor;
it does not inject a fabricated UserSession pointer into SDK memory.

## Sources and limits

- [Switchbrew GSV services](https://switchbrew.org/wiki/Switch_2:_GSV_services):
  bootstrap command shapes, user/chat session separation.
- [Switchbrew Applet Manager](https://switchbrew.org/wiki/Applet_Manager_services#GetGraphicsSupervisor):
  GetGraphicsSupervisor returns a raw TIPC handle.
- Eglantine nx2_gsv.h at commit 30604547544e6f2c766262850b88a31a2404c538:
  empirical chat subscription ID/event reply shapes; much of that source remains
  explicitly guessed/stubbed. Its auto-success and grant-all-features policy are
  not used here.
- Pinned SDK disassembly: BootGraphicsSystem 0x3f8390, BootWithRemoteMemory
  0x412520, GetGraphicsSupervisor 0x1c1aa0, chat context requests 0x639e70 and
  0x63a00c. These are local static observations, not a real-console execution trace.

Implemented state: booted -> process registered -> local context -> chat context.
Only these transitions and inactive local chat subscriptions are provided. No
remote GPU memory is allocated, no screen/camera capture occurs, and unknown
commands fail explicitly. SDK/renderer integration beyond this subset remains
unverified. The host reports zero remote-memory usage because it has no remote
memory mapping facility; this is distinct from the ordinary heap.
