// Match hostfn_tracked for dispatcher observers that briefly enter host code.
// Release before executing any original guest function: guest scheduling and
// its own host-call tracking must remain active while that function runs.
#pragma once
static void *hwaoi_host_section_enter(Cpu *c) {
    void *t = kern_host_track ? c->thread : NULL;
    if (t) {
        int *depth = kern_host_depth(t);
        __atomic_store_n(depth, *depth + 1, __ATOMIC_RELAXED);
    }
    return t;
}
static void hwaoi_host_section_leave(void *t) {
    if (!t) return;
    int *depth = kern_host_depth(t), n = *depth - 1;
    __atomic_store_n(depth, n, __ATOMIC_RELAXED);
    if (!n && kern_preempt_pending(t)) kern_host_left(t);
}
