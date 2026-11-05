# GameChat offline adapter

The target is sdk SHA-256
`4617761033cab45951a19fa7797fa79ad691c46f324048f832484877d3df034c`.
All offsets below are relative to that module, not host addresses.

## Evidence and boundaries

- Existing runtime capture `work/reports/boot-01/stderr.log` observes nnMain+0x7c
  requesting chat:u through `CreateUserServiceByHipc`.
- Local NSO/Capstone inspection observes `Initialize` at 0x633ea0 branching into
  `InitializeUserService` (0x636580), which acquires a scoped interface, requests
  an event and attaches it as a SystemEvent. `IsChatting` (0x633ec0) polls this
  event and masks the result to one bit. These are static observations, not a
  console runtime trace.
- `SetScreenSharingMode` at 0x633fd0 forwards the input w0 through the user
  interface at vtable slot 0x70. The complete enum range is unknown.
- [Switchbrew Chat services](https://switchbrew.org/wiki/Switch_2:_Chat_services#chat:u)
  identifies user command 0/scoped command 1 and SetScreenSharingMode command 23.
  Its event description agrees with the target SDK. It is reverse-engineering
  documentation, not Nintendo's API contract.
- [Eglantine friend.cpp](https://github.com/Djihads80/Eglantine/blob/30604547544e6f2c766262850b88a31a2404c538/src/core/hle/service/friend/friend.cpp)
  supplies a comparable event lifetime model. Its chat code is explicitly marked
  guessed/stubbed; its unknown-command auto-success behavior is NOT adopted here.
- REA open_binary rejected the NSO format. The assembly observations above came
  from the local parser/Capstone fallback, not REA retained Evidence.

## Implemented policy

No networking, microphone, camera, peer discovery or screen capture occurs.
Opening command 0 creates a scoped object owning one writable kernel event.
Scoped command 1 gives the client a new readable handle for the same event.
The event stays unsignalled because no chat is active. The last session reference
closes the writable handle; client copies retain the event independently.
Command 23 stores the requested u32 mode locally, without claiming active sharing.
Unknown operations, including the undocumented meaning of user command 10,
remain fatal with their command ID. Domain requests are not implicitly faked.

This is an explicitly limited offline model, not full GameChat emulation or
proof of behavior on a real console. The guest's own initialization and state
management remain in use; no whole-function Initialize override is installed.
