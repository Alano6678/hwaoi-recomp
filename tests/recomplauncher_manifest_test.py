import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RecompLauncherManifestTests(unittest.TestCase):
    def test_manifest_contract(self):
        manifest = json.loads((ROOT / "packaging/recomplauncher.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["format"], 1)
        self.assertIn("nup", {item["id"] for item in manifest["inputs"]})
        self.assertEqual(manifest["prepare"]["program"], manifest["start"]["program"])
        self.assertEqual(manifest["prepare"]["args"][:2], ["cli", "prepare"])
        self.assertEqual(manifest["prepare"]["done"], ["{install}/game/hwaoi-compiler.json"])
        self.assertNotIn("timeout", manifest["prepare"])
        self.assertNotIn("version", manifest["prepare"])
        self.assertEqual(manifest["start"]["args"][:2], ["play", "--game-dir"])


if __name__ == "__main__":
    unittest.main()
