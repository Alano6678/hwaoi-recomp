"""The game's own button pictures drawn again for the device in use (runtime/hud_prompts.c).

The real file is compiled with a fixture that stands in for the input, the settings and the font, and draws into an
empty atlas: no game and no game data. The fixture writes the atlas's alpha as a PGM; the boxes below are where the
game keeps each picture (the same numbers as the tables in hud_prompts.c).
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FONT = ROOT / "runtime/ui/fonts/Roboto-Medium.ttf"
SIDE = 1024
FACE = {"X": (5, 515, 42, 42), "Y": (59, 515, 42, 42), "A": (923, 479, 42, 42), "B": (977, 479, 42, 42)}
SHOULDER = {"L": (493, 574, 40, 32), "R": (545, 574, 40, 32), "ZL": (601, 572, 38, 36), "ZR": (655, 572, 38, 36)}
MENU = {"PLUS": (9, 627, 34, 34), "MINUS": (63, 627, 34, 34), "PLUS_DISC": (118, 628, 33, 33),
        "MINUS_DISC": (172, 628, 33, 33)}
CLICK = {"LCLICK": (59, 567, 41, 46), "RCLICK": (113, 567, 41, 46)}
DPAD = {"DOWN": (697, 619, 34, 34), "LEFT": (735, 619, 34, 34), "RIGHT": (773, 619, 34, 34), "UP": (811, 619, 34, 34),
        "DOWN2": (761, 515, 42, 44), "LEFT2": (814, 515, 43, 42), "RIGHT2": (869, 515, 44, 42), "UP2": (923, 531, 42, 44)}
DIAMOND = {"YA": (220, 515, 44, 42), "XB": (275, 514, 42, 44), "B": (329, 515, 42, 42), "Y": (383, 515, 42, 42),
           "A": (437, 515, 42, 42), "X": (491, 515, 42, 42)}
STICK = {"LSTICK": (977, 536, 41, 37), "RSTICK": (5, 572, 41, 37)}   # "move" and "look around"
# the d-pad as discs with an arrow, which the character wheel draws
ARROWS = {"ARROW_RIGHT": (222, 624, 40, 40), "ARROW_DOWN": (276, 624, 40, 40), "ARROW_UP": (330, 624, 40, 40),
          "ARROW_LEFT": (384, 624, 40, 40)}
EVERY = {**{"face " + k: v for k, v in FACE.items()}, **SHOULDER, **MENU, **CLICK, **DPAD, **STICK, **ARROWS,
         **{"dots " + k: v for k, v in DIAMOND.items()}}
# The battle HUD's sheet (2048 x 1024): its own copies of some pictures.
HUD_FACE = {"B": (994, 546, 40, 40), "X": (1048, 546, 40, 40), "Y": (1102, 546, 40, 40), "A": (1490, 542, 40, 40)}
HUD_SHOULDER = {"R": (1210, 550, 39, 32), "ZR": (1782, 587, 36, 34)}
HUD_CLICK = {"LCLICK": (1156, 544, 39, 43)}
HUD_EVERY = {**HUD_FACE, **HUD_SHOULDER, **HUD_CLICK}
# The skill wheel's sheet (2048 x 512): fourteen wheels in a row and one below the first, each with the discs of some
# face buttons; a disc's letter is in a box of 28 texels around the disc's centre, 36 from the wheel's.
WHEELS = ["XYB", "XY", "Y", "X", "XYAB", "YB", "B", "XB", "XYA", "YA", "A", "XA", "YAB", "AB", "XAB"]
GREY = {(4, "X"), (4, "Y"), (4, "A"), (4, "B"), (10, "A")}   # wheels that are off: their letters are grey
ARM = {"X": (0, -36), "Y": (-36, 0), "A": (36, 0), "B": (0, 36)}


def wheel_letter(wheel, button):
    cx, cy = (212 + 134 * wheel, 212) if wheel < 14 else (212, 346)
    return cx + ARM[button][0] - 14, cy + ARM[button][1] - 14, 28, 28


WHEEL_LETTERS = {(n, b): wheel_letter(n, b) for n, buttons in enumerate(WHEELS) for b in buttons}
# The two sheets of the HUD's size found on 2026-11-23 (hud_prompts.c trio_wheels, wheel2_wheels): wheels at places
# of their own, the same arm.
TRIO = [(1872, 382, "XYB"), (1872, 516, "XYA"), (1872, 650, "YAB"), (66, 698, "XAB")]
WHEEL2 = [(355, 694, "XYB"), (488, 694, "XYA"), (621, 694, "YAB"), (754, 694, "XAB"), (887, 694, "XYB"),
          (1020, 694, "XY"), (355, 827, "XYAB"), (488, 827, "YB"), (621, 827, "B"), (754, 827, "XB"),
          (887, 827, "XYA"), (1020, 827, "YA"), (1153, 827, "A"), (1286, 827, "XA"), (1419, 827, "YAB"),
          (1552, 827, "AB"), (1685, 827, "XAB"), (1818, 827, "X")]


def letters_of(wheels):
    return {(n, b): (x + ARM[b][0] - 14, y + ARM[b][1] - 14, 28, 28) for n, (x, y, bs) in enumerate(wheels) for b in bs}


def compiler():
    named = os.environ.get("AOD_TEST_CC")
    if named:
        return named
    own = ROOT.parent.parent / ".cache/toolchain/windows/llvm/bin/clang.exe"   # the project's toolchain (Windows)
    return str(own) if os.name == "nt" and own.exists() else shutil.which("clang")


def blocks_of(box):
    """The box grown to whole 4x4 blocks: what a redrawn picture may touch."""
    x, y, w, h = box
    return x & ~3, y & ~3, (x + w + 3) & ~3, (y + h + 3) & ~3


class Atlas:
    """One of the game's three textures after the fixture drew into it: its alpha, or for the skill wheel's sheet the
    brightness of its colour. `side`: its width."""

    def __init__(self, drawn, alpha, side=SIDE):
        self.drawn, self.alpha, self.side = drawn, alpha, side

    def ink(self, box, over=127):
        """Texels of the box that are more white than not."""
        x, y, w, h = box
        s = self.side
        return sum(1 for row in range(y, y + h) for a in self.alpha[row * s + x:row * s + x + w] if a > over)

    def part(self, box):
        x, y, w, h = box
        s = self.side
        return b"".join(self.alpha[row * s + x:row * s + x + w] for row in range(y, y + h))

    def outside(self, boxes):
        """Texels with any white outside the blocks of these boxes."""
        keep = bytearray(self.alpha)
        for box in boxes:
            x0, y0, x1, y1 = blocks_of(box)
            for row in range(y0, y1):
                keep[row * self.side + x0:row * self.side + x1] = bytes(x1 - x0)
        return sum(1 for a in keep if a)


class HudPrompts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="aod-hud-prompts-")
        cls.exe = Path(cls.tmp.name) / ("fixture.exe" if os.name == "nt" else "fixture")
        cc = compiler()
        if not cc:
            raise RuntimeError("Set AOD_TEST_CC to clang")
        subprocess.run([cc, "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "runtime"),
                        "-I", str(ROOT / "runtime/third_party/imgui"), str(ROOT / "runtime/hud_prompts.c"),
                        str(ROOT / "runtime/sha256.c"), str(ROOT / "tests/hwaoi/hud_prompts_fixture.c"),
                        "-o", str(cls.exe)] + ([] if os.name == "nt" else ["-lm"]), check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def draw(self, mode, *state, font=FONT):
        pgm = Path(self.tmp.name) / "alpha.pgm"
        out = subprocess.run([str(self.exe), str(font), mode, str(pgm), *state], check=True, capture_output=True,
                             text=True).stdout.split()
        self.assertEqual(out[:2], ["patch", "0"])   # an empty texture is not one of the game's
        data = pgm.read_bytes()
        magic, size, depth, texels = data.split(b"\n", 3)
        self.assertEqual((magic, depth), (b"P5", b"255"))
        width, height = (int(v) for v in size.split())
        self.assertEqual(len(texels), width * height)
        return Atlas(int(out[3]), texels, width)

    def assert_only(self, atlas, groups):
        """The pictures of these groups are there, the others are not, and nothing is outside them."""
        drawn = {name: box for group in groups for name, box in group.items()}
        for name, box in EVERY.items():
            wanted = any(box == b for b in drawn.values())
            with self.subTest(picture=name):
                self.assertEqual(atlas.ink(box) > 40, wanted)
        self.assertEqual(atlas.outside(drawn.values()), 0)
        self.assertEqual(atlas.drawn, len(drawn))

    def test_the_games_own_pictures_stay_when_asked(self):
        self.assert_only(self.draw("game", "family=playstation", "layout=xbox", "labels"), [])

    def test_a_switch_controller_with_the_games_buttons_changes_nothing(self):
        self.assert_only(self.draw("auto", "family=nintendo", "layout=nintendo", "labels"), [])

    def test_an_xbox_pad_shows_its_own_letters_where_the_switch_has_the_buttons(self):
        # the default layout: the game's A is the right button, which an Xbox pad marks B. The four letters are drawn
        # again; the dots that show where a button is are right as the game has them.
        atlas = self.draw("auto", "family=xbox", "layout=nintendo", "labels")
        self.assert_only(atlas, [{"face " + k: v for k, v in FACE.items()}, SHOULDER, MENU])
        # the same four pictures as a Switch controller with A and B, X and Y swapped by the player: B where the game
        # has A, A where it has B, Y for X and X for Y
        swapped = self.draw("auto", "family=nintendo", "layout=nintendo", "labels", "bind:A=south", "bind:B=east",
                            "bind:X=west", "bind:Y=north")
        for name, box in FACE.items():
            with self.subTest(letter=name):
                self.assertEqual(atlas.part(box), swapped.part(box))
        self.assertNotEqual(atlas.part(FACE["A"]), atlas.part(FACE["B"]))
        # a pad that names no buttons is an Xbox pad here too
        self.assertEqual(self.draw("auto", "family=generic", "layout=nintendo").alpha, atlas.alpha)

    def test_a_playstation_pad_shows_its_signs_where_the_switch_has_the_buttons(self):
        atlas = self.draw("auto", "family=playstation", "layout=nintendo", "labels")
        self.assert_only(atlas, [{"face " + k: v for k, v in FACE.items()}, SHOULDER, MENU])
        # circle is the game's A (right) and cross its B (bottom); with A at the bottom they are the other way round
        bottom = self.draw("auto", "family=playstation", "layout=xbox", "labels")
        self.assertAlmostEqual(atlas.ink(FACE["A"]), bottom.ink(FACE["B"]), delta=12)
        self.assertAlmostEqual(atlas.ink(FACE["B"]), bottom.ink(FACE["A"]), delta=12)
        self.assertAlmostEqual(atlas.ink(FACE["X"]), bottom.ink(FACE["Y"]), delta=12)
        self.assertNotEqual(atlas.part(FACE["A"]), bottom.part(FACE["A"]))

    def test_an_xbox_pad_with_a_at_the_bottom_keeps_the_games_letters(self):
        # pad_layout = xbox: south is A on the pad and in the game, so the letters are right. LB, RT, the two menu
        # buttons and the dots that show where a button is are not.
        atlas = self.draw("auto", "family=xbox", "layout=xbox", "labels")
        self.assert_only(atlas, [SHOULDER, MENU, {"dots " + k: v for k, v in DIAMOND.items()}])

    def test_a_pad_that_names_no_buttons_is_drawn_as_an_xbox_pad(self):
        named = self.draw("auto", "family=xbox", "layout=xbox", "labels")
        self.assertEqual(self.draw("auto", "family=generic", "layout=xbox").alpha, named.alpha)

    def test_a_playstation_pad_gets_its_four_signs(self):
        atlas = self.draw("auto", "family=playstation", "layout=xbox", "labels")
        self.assert_only(atlas, [{"face " + k: v for k, v in FACE.items()}, SHOULDER, MENU,
                                 {"dots " + k: v for k, v in DIAMOND.items()}])
        signs = [atlas.ink(box) for box in FACE.values()]
        self.assertEqual(len(set(signs)), 4)   # cross, circle, square and triangle cut different amounts out
        self.assertEqual(self.draw("playstation", "family=xbox", "layout=xbox", "labels").alpha, atlas.alpha)

    def test_a_maker_asked_for_without_a_gamepad_changes_nothing(self):
        self.assert_only(self.draw("playstation"), [])
        self.assert_only(self.draw("xbox", "keyboard"), [])

    def test_a_remapped_button_shows_the_button_it_is_on(self):
        plain = self.draw("auto", "family=playstation", "layout=xbox", "labels")
        moved = self.draw("auto", "family=playstation", "layout=xbox", "labels", "bind:Y=west", "bind:X=north")
        # the game's Y was the triangle and is the square now; X the other way round
        self.assertAlmostEqual(moved.ink(FACE["Y"]), plain.ink(FACE["X"]), delta=12)
        self.assertAlmostEqual(moved.ink(FACE["X"]), plain.ink(FACE["Y"]), delta=12)
        self.assertNotEqual(moved.part(FACE["Y"]), plain.part(FACE["Y"]))
        self.assertEqual(moved.part(FACE["A"]), plain.part(FACE["A"]))

    def test_one_remapped_button_redraws_its_whole_group(self):
        # a Switch controller with ZL on the A button: the four shoulder pictures are ours, the rest stays the game's
        atlas = self.draw("auto", "family=nintendo", "layout=nintendo", "labels", "bind:ZL=east")
        self.assert_only(atlas, [SHOULDER])
        # ZL is a disc with A now, ZR beside it a box with its name, in boxes of one size
        self.assertGreater(abs(atlas.ink(SHOULDER["ZL"]) - atlas.ink(SHOULDER["ZR"])), 20)

    def test_a_button_off_the_face_buttons_is_not_shown_as_a_dot(self):
        atlas = self.draw("auto", "family=nintendo", "layout=nintendo", "labels", "bind:A=lpaddle1")
        self.assert_only(atlas, [{"face " + k: v for k, v in FACE.items()}, {"dots A": DIAMOND["A"]}])

    def test_the_keyboard_gets_its_keys(self):
        atlas = self.draw("auto", "keyboard")
        singles = {"dots " + k: DIAMOND[k] for k in "BYAX"}   # two buttons at once have no key picture
        # the sticks are the four keys that move and the mouse; the character wheel's arrows are the keys 1 to 4
        self.assert_only(atlas, [{"face " + k: v for k, v in FACE.items()}, SHOULDER, MENU, CLICK, DPAD, STICK, ARROWS,
                                 singles])
        self.assertEqual(self.draw("auto", "keyboard", "family=xbox", "layout=xbox", "labels").alpha, atlas.alpha)

    def assert_hud_only(self, atlas, groups):
        drawn = {name: box for group in groups for name, box in group.items()}
        for name, box in HUD_EVERY.items():
            with self.subTest(picture=name):
                self.assertEqual(atlas.ink(box) > 40, name in drawn)
        self.assertEqual(atlas.outside(drawn.values()), 0)
        self.assertEqual(atlas.drawn, len(drawn))

    def test_the_battle_huds_copies_follow_the_atlas(self):
        # an Xbox pad where the Switch has the buttons: the HUD's four letter discs, R and ZR are ours; its stick click
        # stays (a stick click looks the same on every maker's pad)
        atlas = self.draw("auto", "sheet=hud", "family=xbox", "layout=nintendo", "labels")
        self.assertEqual(atlas.side, 2048)
        self.assert_hud_only(atlas, [HUD_FACE, HUD_SHOULDER])
        # the disc where the game has A shows the pad's B: the picture a Switch controller gets with A on its south
        swapped = self.draw("auto", "sheet=hud", "family=nintendo", "layout=nintendo", "labels", "bind:A=south",
                            "bind:B=east", "bind:X=west", "bind:Y=north")
        for name, box in HUD_FACE.items():
            with self.subTest(letter=name):
                self.assertEqual(atlas.part(box), swapped.part(box))
        self.assertNotEqual(atlas.part(HUD_FACE["A"]), atlas.part(HUD_FACE["B"]))
        # a Switch controller with the game's buttons, and the game's own pictures when asked: nothing
        self.assert_hud_only(self.draw("auto", "sheet=hud", "family=nintendo", "layout=nintendo", "labels"), [])
        self.assert_hud_only(self.draw("game", "sheet=hud", "family=xbox", "layout=nintendo", "labels"), [])
        # with A at the bottom an Xbox pad keeps the letters; R and ZR are RB and RT all the same
        self.assert_hud_only(self.draw("auto", "sheet=hud", "family=xbox", "layout=xbox", "labels"), [HUD_SHOULDER])
        # the keyboard: every picture is a key
        self.assert_hud_only(self.draw("auto", "sheet=hud", "keyboard"), [HUD_FACE, HUD_SHOULDER, HUD_CLICK])

    def test_the_skill_wheels_letters_are_the_pads(self):
        # the wheel's discs are where a Switch controller has the buttons; each letter in them is the pad's own
        atlas = self.draw("auto", "sheet=wheel", "family=xbox", "layout=nintendo", "labels")
        self.assertEqual((atlas.side, len(atlas.alpha)), (2048, 2048 * 512))
        self.assertEqual(atlas.drawn, len(WHEEL_LETTERS))
        self.assertEqual(len(WHEEL_LETTERS), 32)
        for (wheel, button), box in WHEEL_LETTERS.items():
            with self.subTest(wheel=wheel, button=button):
                grey = (wheel, button) in GREY
                self.assertGreater(atlas.ink(box, over=40), 60)                   # a letter is there
                self.assertEqual(atlas.ink(box, over=127) > 60, not grey)          # light, or grey in a wheel that is off
        self.assertEqual(atlas.outside(WHEEL_LETTERS.values()), 0)
        # the top disc (the game's X) shows Y, as the bottom of the left one would on a Switch controller with X and
        # Y swapped by the player; the same letter is the same picture in every wheel
        swapped = self.draw("auto", "sheet=wheel", "family=nintendo", "layout=nintendo", "labels", "bind:A=south",
                            "bind:B=east", "bind:X=west", "bind:Y=north")
        self.assertEqual(atlas.alpha, swapped.alpha)
        self.assertEqual(atlas.part(WHEEL_LETTERS[0, "X"]), atlas.part(WHEEL_LETTERS[14, "X"]))
        self.assertNotEqual(atlas.part(WHEEL_LETTERS[0, "X"]), atlas.part(WHEEL_LETTERS[0, "Y"]))
        # a PlayStation pad gets its four signs
        signs = self.draw("auto", "sheet=wheel", "family=playstation", "layout=nintendo", "labels")
        self.assertEqual(signs.drawn, 32)
        self.assertEqual(len({signs.part(WHEEL_LETTERS[12, b]) for b in "YAB"} | {signs.part(WHEEL_LETTERS[0, "X"])}), 4)
        # the game's letters stay for a Switch controller, for an Xbox pad with A at the bottom, and when asked
        for state in (("auto", "family=nintendo", "layout=nintendo", "labels"), ("auto", "family=xbox", "layout=xbox", "labels"),
                      ("game", "family=xbox", "layout=nintendo", "labels")):
            with self.subTest(state=state):
                kept = self.draw(state[0], "sheet=wheel", *state[1:])
                self.assertEqual((kept.drawn, kept.outside([])), (0, 0))
        # the keyboard: a key or the mouse in every disc
        self.assertEqual(self.draw("auto", "sheet=wheel", "keyboard").drawn, 32)

    def test_the_three_disc_wheels_and_the_second_wheel_sheet(self):
        # each letter in its disc, as on the skill wheel's sheet; nothing anywhere else; a Switch pad keeps the game's
        for sheet, wheels in (("trio", TRIO), ("wheel2", WHEEL2)):
            boxes = letters_of(wheels)
            with self.subTest(sheet=sheet):
                drawn = self.draw("auto", f"sheet={sheet}", "family=xbox", "layout=nintendo", "labels")
                self.assertEqual((drawn.side, len(drawn.alpha)), (2048, 2048 * 1024))
                self.assertEqual(drawn.drawn, len(boxes))
                for key, box in boxes.items():
                    self.assertGreater(drawn.ink(box, over=40), 60, key)
                self.assertEqual(drawn.outside(boxes.values()), 0)
                # the top disc (the game's X) shows the Xbox pad's top letter, Y: the left disc's letter on a Switch pad
                switch = self.draw("auto", f"sheet={sheet}", "family=nintendo", "layout=nintendo", "labels")
                self.assertEqual((switch.drawn, switch.outside([])), (0, 0))
        trio = self.draw("auto", "sheet=trio", "family=xbox", "layout=nintendo", "labels")
        letters = letters_of(TRIO)
        self.assertEqual(trio.part(letters[1, "X"]), trio.part(letters[3, "X"]))   # the same ink (the first wheel's is darker)
        self.assertNotEqual(trio.part(letters[1, "X"]), trio.part(letters[1, "Y"]))

    def test_without_a_font_the_shapes_are_still_drawn(self):
        atlas = self.draw("auto", "family=xbox", "layout=xbox", "labels", font=Path(self.tmp.name) / "missing.ttf")
        self.assert_only(atlas, [SHOULDER, MENU, {"dots " + k: v for k, v in DIAMOND.items()}])


if __name__ == "__main__":
    unittest.main()
