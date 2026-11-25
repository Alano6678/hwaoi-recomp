"""Memory the machine can still give to new processes, for sizing worker pools (stdlib only; Windows and Linux)."""
import ctypes
import ctypes.wintypes as wt
import os
import sys

JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
LIMIT_PROCESS_MEMORY = 0x100
LIMIT_JOB_MEMORY = 0x200


class _MemStatus(ctypes.Structure):
    _fields_ = [('dwLength', wt.DWORD), ('dwMemoryLoad', wt.DWORD)] + \
               [(n, ctypes.c_ulonglong) for n in ('total', 'avail', 'tpf', 'apf', 'tv', 'av', 'aev')]


class _JobLimits(ctypes.Structure):
    _fields_ = [('PerProcessUserTimeLimit', ctypes.c_longlong), ('PerJobUserTimeLimit', ctypes.c_longlong),
                ('LimitFlags', wt.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wt.DWORD),
                ('Affinity', ctypes.c_size_t), ('PriorityClass', wt.DWORD), ('SchedulingClass', wt.DWORD),
                ('r_ops', ctypes.c_ulonglong), ('w_ops', ctypes.c_ulonglong), ('o_ops', ctypes.c_ulonglong),
                ('r_bytes', ctypes.c_ulonglong), ('w_bytes', ctypes.c_ulonglong), ('o_bytes', ctypes.c_ulonglong),
                ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]


def _job_headroom():
    """Bytes left under the memory limit of the job object this process runs in, or None without such a limit."""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.QueryInformationJobObject.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p]
    info = _JobLimits()
    # A null handle asks about the job of the calling process.
    if not k32.QueryInformationJobObject(None, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION, ctypes.byref(info),
                                         ctypes.sizeof(info), None):
        return None
    if info.LimitFlags & LIMIT_JOB_MEMORY:
        return max(0, info.JobMemoryLimit - info.PeakJobMemoryUsed)
    if info.LimitFlags & LIMIT_PROCESS_MEMORY:
        return info.ProcessMemoryLimit
    return None


def _meminfo():
    """/proc/meminfo in bytes (Linux), or {} when it cannot be read."""
    try:
        with open('/proc/meminfo', encoding='ascii') as f:
            rows = [line.split() for line in f]
    except OSError:
        return {}
    return {r[0].rstrip(':'): int(r[1]) * 1024 for r in rows if len(r) >= 2 and r[1].isdigit()}


def _cgroup_headroom():
    """Bytes left under the tightest cgroup v2 memory limit above this process (a container, Flatpak, systemd-run
    scope), or None without one."""
    try:
        with open('/proc/self/cgroup', encoding='ascii') as f:
            path = next((line.strip().split('::', 1)[1] for line in f if line.startswith('0::')), None)
    except OSError:
        return None
    best = None
    while path:
        base = '/sys/fs/cgroup' + path.rstrip('/')
        try:
            with open(base + '/memory.max', encoding='ascii') as f:
                limit = f.read().strip()
            if limit != 'max':
                with open(base + '/memory.current', encoding='ascii') as f:
                    left = max(0, int(limit) - int(f.read().strip()))
                best = left if best is None else min(best, left)
        except (OSError, ValueError):
            pass
        path = path.rsplit('/', 1)[0] if path.rstrip('/') else ''
    return best


def available_ram():
    """Physical memory not in use right now, capped by the job memory limit if there is one (Linux: MemAvailable,
    capped by the cgroup memory limit). None elsewhere."""
    if sys.platform.startswith('linux'):
        free = _meminfo().get('MemAvailable')
        headroom = _cgroup_headroom()
        return free if free is None or headroom is None else min(free, headroom)
    if sys.platform != 'win32':
        return None
    status = _MemStatus()
    status.dwLength = ctypes.sizeof(status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        return None
    headroom = _job_headroom()
    return status.avail if headroom is None else min(status.avail, headroom)


def available_commit():
    """System commit headroom, capped by this process's job limit. Linux enforces a commit limit only in strict
    overcommit mode (vm.overcommit_memory=2): then CommitLimit - Committed_AS, else None, as elsewhere."""
    if sys.platform.startswith('linux'):
        try:
            with open('/proc/sys/vm/overcommit_memory', encoding='ascii') as f:
                strict = f.read().strip() == '2'
        except OSError:
            strict = False
        info = _meminfo()
        if not strict or 'CommitLimit' not in info or 'Committed_AS' not in info:
            return None
        return max(0, info['CommitLimit'] - info['Committed_AS'])
    if sys.platform != 'win32':
        return None
    status = _MemStatus()
    status.dwLength = ctypes.sizeof(status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        return None
    headroom = _job_headroom()
    return status.apf if headroom is None else min(status.apf, headroom)


def build_threads(cpus=None):
    """How many workers an automatic build starts at most: every logical processor but one in eight, and always at
    least one left for the rest of the system (20 -> 18, 16 -> 14, 8 -> 7, 4 -> 3, 2 -> 1). The setup runs the build
    below normal priority (the launcher's job object on Windows, nice on Linux), so the desktop, a game or a call still
    gets the processor first; the memory checks of each stage can lower the count further."""
    if not cpus:
        count = getattr(os, "process_cpu_count", None)   # Python 3.13+: the processors this process may use
        cpus = (count() if count else None) or os.cpu_count() or 1
    return max(1, cpus - max(1, cpus // 8))
