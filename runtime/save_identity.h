// Which account the save data of a profile folder belongs to, for a start whose account.uid does not lead to it
// (ov_system.c load_uid). The layout is fs_host.c's: <profile>/save/<uid high half><uid low half>/ is an account's
// committed save, <profile>/save-tx/ready-<n>, new and old hold a save commit that waits or was interrupted.
// Everything here only reads; a link is never followed.
#pragma once
#include <stdint.h>

// 1 when the account uid (low half, high half) has a committed save: a file at any depth under its folder. The empty
// folder every start makes for the account is not one.
int save_identity_has(const char *profile, const uint64_t uid[2]);

// 1 when a file lies under save-tx/ready-<n>, save-tx/new or save-tx/old: save data of the account account.uid names
// that the next start moves into its folder. The working copy (save-tx/work) is not counted: a start drops it.
int save_identity_pending(const char *profile);

// How many accounts under <profile>/save have a committed save. With exactly one, its id goes to uid (low half, high
// half) and its folder name (32 hex digits) to name; otherwise both are left alone.
int save_identity_saves(const char *profile, uint64_t uid[2], char name[33]);
