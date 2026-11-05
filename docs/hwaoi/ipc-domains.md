# CMIF domain transport

The HWAoI SDK first requests chat:u, then uses control 0 to convert the session
to a domain. The old DKB transport only handled plain CMIF/TIPC. The fork now
implements domain object routing and output interface IDs without replacing the
game's Initialize routine.

Protocol layout was checked against libnx cmif.h/service.h at commit
`b156b4774fb2df7b93402169509266da7547fc4c`. No Atmosphere implementation code was
copied. Controls 0/1/2/4 always retain plain CMIF framing; request-domain headers
and response-domain headers are separate 16-byte structures.

- Convert creates a nonzero root ID. Namespace size is bounded to 1024 entries.
- A domain clone shares the namespace. An ordinary clone or CopyFromDomain gets
  a fresh transport wrapper with a retained payload owner; converting that copy
  does not change other handles' framing mode.
- Only interface move handles become domain IDs. Native event copy handles and
  raw TIPC sessions remain native handles.
- Child close removes its ID and releases the table reference; outstanding
  copied sessions and requests keep it alive. Root payload cleanup is deferred
  until its owning transport tears down, preserving pre-conversion references.
- Unsupported input-domain-object requests and malformed/closed IDs fail
  explicitly. This is not a complete implementation of all domain operations.

The synthetic native fixture compiles the actual IPC/chat/GSV source, checks
reply layouts, object/event cleanup, cloning and error paths. Its mutex mocks
are single-threaded; it does not establish scheduler/concurrency correctness.
The GSV mutable states additionally use real per-object locks in the runtime.
