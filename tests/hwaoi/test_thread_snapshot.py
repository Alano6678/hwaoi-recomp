"""Read-only Windows snapshot against a controlled sleeping-thread executable."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("hwaoi_threads", ROOT / "tools/hwaoi_threads.py")
SNAP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SNAP)


@unittest.skipUnless(os.name == "nt" and SNAP.COMPILER.is_file(), "Existing Windows Clang required")
class ThreadSnapshotTests(unittest.TestCase):
    def test_identity_failures_sleeping_threads_and_resume(self):
        (ROOT / "work/build").mkdir(parents=True, exist_ok=True)
        (ROOT / "work/reports").mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="thread-fixture-", dir=ROOT / "work/build") as directory, \
             tempfile.TemporaryDirectory(prefix="thread-fixture-", dir=ROOT / "work/reports") as reports:
            directory, reports = Path(directory), Path(reports)
            source = directory / "fixture.c"
            source.write_text(r'''
#include <windows.h>
#include <stdio.h>
static volatile LONG done;
__declspec(noinline) DWORD WINAPI sleeping_worker(void *p) {
    (void)p;
    while (!InterlockedCompareExchange(&done, 0, 0)) Sleep(10);
    return 0;
}
int main(void) {
    HANDLE worker = CreateThread(NULL, 0, sleeping_worker, NULL, 0, NULL);
    if (!worker) return 2;
    puts("ready"); fflush(stdout);
    getchar();
    InterlockedExchange(&done, 1);
    DWORD status = WaitForSingleObject(worker, 2000);
    CloseHandle(worker);
    return status == WAIT_OBJECT_0 ? 0 : 3;
}
''', encoding="utf-8")
            exe = directory / "fixture.exe"
            subprocess.run([str(SNAP.COMPILER), "-std=c11", "-g", "-O0", str(source), "-o", str(exe)],
                           check=True, capture_output=True, timeout=60)
            proc = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                self.assertEqual(proc.stdout.readline().strip(), "ready")
                out = reports / "threads.json"
                with self.assertRaisesRegex(ValueError, "SHA-256"):
                    SNAP.capture(proc.pid, out, exe, "0" * 64)
                with self.assertRaisesRegex(ValueError, "executable path mismatch"):
                    SNAP.capture(proc.pid, out, source)
                with self.assertRaisesRegex(ValueError, "path must be below"):
                    SNAP.capture(proc.pid, directory / "wrong.json", exe)
                with self.assertRaises(ValueError):
                    SNAP.capture(os.getpid(), out)
                helper = SNAP.build_helper()
                refused = subprocess.run([str(helper), str(proc.pid), str(source)], capture_output=True, text=True, timeout=10)
                self.assertEqual(refused.returncode, 2)
                self.assertIn("identity/architecture mismatch", refused.stderr)
                refused = subprocess.run([str(helper), "0", str(exe)], capture_output=True, text=True, timeout=10)
                self.assertEqual(refused.returncode, 2)
                report = SNAP.capture(proc.pid, out, exe, SNAP.digest(exe))
                self.assertGreaterEqual(len(report["threads"]), 2)
                self.assertFalse(report["game_memory_written"])
                self.assertEqual(report["module_inventory_error"], 0)
                self.assertEqual(report["thread_inventory_error"], 0)
                self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["sha256"], SNAP.digest(exe))
                for thread in report["threads"]:
                    self.assertNotIn("error", thread)
                    self.assertEqual(thread["context_error"], 0)
                    self.assertEqual(thread["resume_error"], 0)
                    self.assertEqual(thread["previous_suspend_count"], 0)
                    self.assertGreater(thread["stack_bytes"], 0)
                    self.assertTrue(thread["frames"])
                frames = [f for t in report["threads"] for f in t["frames"]]
                self.assertTrue(any("sleeping_worker" in str(f.get("symbol")) for f in frames), frames)
                with self.assertRaisesRegex(ValueError, "overwrite"):
                    SNAP.capture(proc.pid, out, exe)
                # A second snapshot and an orderly join prove matching resumes.
                second = SNAP.capture(proc.pid, reports / "threads-02.json", exe)
                self.assertEqual(second["creation_filetime"], report["creation_filetime"])
                proc.communicate("q", timeout=5)
                self.assertEqual(proc.returncode, 0)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
