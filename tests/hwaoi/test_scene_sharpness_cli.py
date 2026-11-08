"""Quality-option bounds; no game payloads or process execution."""
import argparse
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from hwaoi import scene_sharpness


class SceneSharpnessCliTests(unittest.TestCase):
    def test_strength_and_original_game_mode(self):
        for value in ("0", "40", "100", "game"):
            self.assertEqual(scene_sharpness(value), value)
        self.assertEqual(scene_sharpness("040"), "40")

    def test_invalid_values_cannot_reach_launch(self):
        for value in ("", "-1", "101", "0.4", "nan", "Game", "４０", "40 extra"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                scene_sharpness(value)


if __name__ == "__main__":
    unittest.main()
