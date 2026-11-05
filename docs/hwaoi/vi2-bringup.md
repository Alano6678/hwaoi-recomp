# VI2/GameShare initialization notes

The source is the pinned sdk NSO in config/games/hwaoi-update-196608.json.
These are module-relative static observations and local runtime captures, not
real-console traces or a complete VI2 protocol contract.

## Confirmed client shape

- UserSession 301 creates display context; 305 takes version 0x300020000 in this
  build and returns a raw TIPC DisplaySession.
- Display command 3 returns a value below 8; the SDK uses bound+1 for its table.
  Command 1 returns a value below 1024, used to size a notification bitset.
- Commands 11/31 return real event handles. The local backend owns their writable
  ends and closes them at session teardown; clients own readable copies.
- Command 101 input is {u64 display-name, u32 client slot}. The first observed
  name is ASCII player1. StreamPlay preallocates 1..3 playerN resources independently
  of whether any network guest is connected.
- Command 201 is {u64 opaque client Layer tag, u32 endpointA, u32 endpointB};
  the first tag is a guest object address, NOT the display name. The SDK allocates
  the two endpoint slots before sending and reads only Result from the response.
- Command 212 is {u64 Layer tag} -> {Result,u64 availability}. Slot >=15 is rejected
  by this SDK (0x5652d0/0x566fc4). The local queue starts with no owned slots.
  This only models empty-queue metadata; submissions/presentation are not accepted
  yet, and the mask must not be used as proof of valid GPU images or released fences.
- Command 251 is {u64 Layer tag,u64 display-name}: an attached-layer registry.
- Command 111 is {u64 display-name,u64 endpoint-name,u64 selected Layer tag}.
  Current endpoint name is ASCII default. SDK 0x566c94 sends the selected list tail
  and records it as active afterwards; the reply is Result only. The backend
  verifies the layer belongs to the named display and records its selection.

Local pool bounds 7/1023 describe actual bounded metadata pools in this adapter,
not measured hardware counts or connected displays. Image queues, timestamp
recorders and vblank composition beyond initialization remain unimplemented.

## Reference boundary

Switchbrew GSV services gives the bootstrap shapes. Eglantine nx2_gsv.h at
30604547544e6f2c766262850b88a31a2404c538 includes guessed/stubbed display responses;
its unconditional all-free queue mask and automatic success are not used as
hardware evidence. This implementation records local object relationships and
rejects operations it cannot carry out. No guest pointer is dereferenced by the
server solely because it appears as an opaque tag.

The actual SDK keeps and constructs its own Display/Endpoint/Layer objects.
The adapter does not fabricate a shared zero native-window pointer for playerN.
