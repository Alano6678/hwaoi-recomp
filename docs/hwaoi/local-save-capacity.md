# Local save capacity and commit entry

The supplied dump has no NACP save quota. The host backend stores saves in
ordinary directories and already uses staged copies, durable renames and
backups. `GetSaveDataSize` now connects to that same backend rather than
entering an uninitialized SDK filesystem service.

The local policy reports the host volume's total capacity available to the
caller as logical save capacity, and zero reserved console journal bytes.
On Windows this uses GetDiskFreeSpaceExW; the Linux implementation uses statvfs.
This is neither free-space reporting nor a reservation. Actual file writes can
still fail when the host disk is full. This does not emulate the console's
per-title quota or reserved journal image. Missing user, invalid arguments,
platform query failures and values outside the signed API range fail without
writing output values.

The pinned SDK exports Commit at +0xac540 and CommitSaveData at +0xac6d0.
Both branch to +0xac550 with only a different diagnostic function name.
Main's import slot +0x285a408 names CommitSaveData. That alias now uses the
existing transactional host commit implementation. The render-26 abort
(result 0x35f202) was caused by the missing alias entering the SDK's own mount
registry after host-side file writes.

Tests:

- `tests/hwaoi/test_save_capacity.py` runs the actual query and ABI wrapper at
  O0/O2, including unchanged outputs on failures and a real Windows volume query.
- `python tools/hwaoi_probe.py probe-storage --out work/reports/<fresh> --timeout 60`
  exercises only a temporary host profile, without reading a ROM or using the
  game profile's existing saves. storage-01 passed 103 checks: both commit
  names, capacity/user validation, persisted bytes, rollback, six interrupted
  transaction steps, backup retention and normal file/directory operations.
  Its temporary profile was removed by the probe. Linux execution is unverified.

Render-26 reached the capacity queries before exposing the missing commit alias.
This filesystem progress alone establishes neither a rendered scene nor
playability.
