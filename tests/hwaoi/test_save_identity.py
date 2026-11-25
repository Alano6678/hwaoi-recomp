"""Which account a profile folder's save belongs to when account.uid does not lead to it (runtime/save_identity.c).

The real file is compiled with a fixture that gives it the two platform calls it makes. No game and no game data: the
"saves" are folders with a file of a few bytes.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
A = "f8446d9c58768dda4be842255fcfa4e1"   # account folder names: 32 hex digits, the high half first
B = "77611bb49e812b9d7dfa769546b08600"


def compiler():
    named = os.environ.get("AOD_TEST_CC")
    if named:
        return named
    own = ROOT.parent.parent / ".cache/toolchain/windows/llvm/bin/clang.exe"   # the project's toolchain (Windows)
    return str(own) if os.name == "nt" and own.exists() else shutil.which("clang")


class SaveIdentity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="aod-save-identity-")
        cls.exe = Path(cls.tmp.name) / ("fixture.exe" if os.name == "nt" else "fixture")
        cc = compiler()
        if not cc:
            raise RuntimeError("Set AOD_TEST_CC to clang")
        subprocess.run([cc, "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "runtime"),
                        str(ROOT / "runtime/save_identity.c"), str(ROOT / "tests/hwaoi/save_identity_fixture.c"),
                        "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix="aod-profile-")
        self.profile = Path(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def put(self, relative, data=b"save"):
        path = self.profile / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def ask(self, account=None):
        out = subprocess.check_output([str(self.exe), str(self.profile)] + ([account] if account else []), text=True)
        answer = {}
        for line in out.splitlines():
            words = line.split()
            answer[words[0]] = words[1:]
        return answer

    def saves(self):
        """(number of accounts with a save, the folder name given back, low half, high half)"""
        words = self.ask()["saves"]
        return int(words[0]), words[2], words[4], words[6]

    def test_an_empty_profile_has_no_save(self):
        self.assertEqual(self.saves(), (0, "untouched", "0000000000001111", "0000000000002222"))
        (self.profile / "save").mkdir()
        self.assertEqual(self.saves()[0], 0)
        self.assertEqual(self.ask()["pending"], ["0"])

    def test_the_only_save_names_its_account(self):
        self.put(f"save/{A}/slot0/game.bin")
        self.assertEqual(self.saves(), (1, A, A[16:], A[:16]))

    def test_the_folder_each_start_makes_is_not_a_save(self):
        (self.profile / "save" / A / "empty" / "deeper").mkdir(parents=True)
        self.assertEqual(self.saves()[0], 0)
        self.put(f"save/{B}/game.bin")
        self.assertEqual(self.saves()[:2], (1, B))   # the empty account folder beside it does not count

    def test_two_saves_name_nobody(self):
        self.put(f"save/{A}/game.bin")
        self.put(f"save/{B}/game.bin")
        self.assertEqual(self.saves(), (2, "untouched", "0000000000001111", "0000000000002222"))

    def test_other_names_are_not_accounts(self):
        for name in (A[:31], A + "0", A.upper(), "g" + A[1:], "0" * 32, A + ".partial"):
            self.put(f"save/{name}/game.bin")
        self.put(f"save/{B}", b"a file named like an account")
        self.assertEqual(self.saves()[0], 0)

    def test_an_account_has_a_save_when_its_folder_holds_a_file(self):
        (self.profile / "save" / A).mkdir(parents=True)
        self.assertEqual(self.ask(A)["has"], ["0"])
        self.assertEqual(self.ask(B)["has"], ["0"])   # no folder at all
        self.put(f"save/{A}/a/b/c/game.bin")
        self.assertEqual(self.ask(A)["has"], ["1"])
        self.assertEqual(self.ask(B)["has"], ["0"])

    def test_a_commit_that_waits_is_pending(self):
        for folder in ("work", "ready-", "ready-1x", "readyness", "other"):   # not commits
            self.put(f"save-tx/{folder}/game.bin")
        (self.profile / "save-tx" / "ready-0000000004").mkdir()   # a commit folder without a file
        (self.profile / "save-tx" / "new").mkdir()
        self.put("save-tx/old", b"a file, not the folder of a replaced save")
        self.assertEqual(self.ask()["pending"], ["0"])
        for folder in ("ready-0000000007", "new", "old"):
            with self.subTest(folder=folder):
                path = self.profile / "save-tx" / folder
                if path.is_file():
                    path.unlink()
                self.put(f"save-tx/{folder}/slot/game.bin")
                self.assertEqual(self.ask()["pending"], ["1"])
                shutil.rmtree(path)
                self.assertEqual(self.ask()["pending"], ["0"])

    @unittest.skipIf(os.name == "nt", "symbolic links need a privilege on Windows")
    def test_links_are_not_followed(self):
        outside = Path(self.dir.name + "-outside")
        try:
            (outside / "data").mkdir(parents=True)
            (outside / "data" / "game.bin").write_bytes(b"save")
            (self.profile / "save" / A).mkdir(parents=True)
            os.symlink(outside / "data", self.profile / "save" / A / "linked")   # a link inside an account folder
            os.symlink(outside / "data", self.profile / "save" / B)              # a link named like an account
            self.assertEqual(self.saves()[0], 0)
            self.assertEqual(self.ask(A)["has"], ["0"])
        finally:
            shutil.rmtree(outside, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
