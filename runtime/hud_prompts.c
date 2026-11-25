// The game's own button pictures follow the device in use: its maker's signs and the player's bindings (hud_prompts.h).
//
// The game draws the button pictures of its hints, tutorials and most of its HUD from one texture, its glyph set:
// 1024 x 1024, BC3, one level. It is a white mask (the shape is in alpha, a letter is a hole in its disc) that the text
// shader tints. The battle HUD has two more: its own sheet, with copies of a few of those pictures (the four face
// buttons, a stick click, R and ZR), and the sheet of the skill wheel, where the four letters are part of the dark
// discs of the wheel. vk_texture.c hands us a texture's bytes on their way to the GPU. When they are one of the three
// (known by its SHA-256, so another build's is left alone), the pictures that are wrong for the player are drawn again in
// place, in the same style: an Xbox pad gets its own letters where the buttons are (B for the game's A, on the right),
// LB and RT, a PlayStation pad its four signs, a remapped button the button it is on, the keyboard its keys. A Switch
// controller with the game's own buttons changes nothing.
//
// The game also shows the face buttons by position: four dots with the one to press filled, the skill wheel, the
// combo list. The default layout keeps every button where a Switch controller has it (input_map.h), so those need no
// change; with A at the bottom (pad_layout = xbox) the dots are drawn again with the right one filled.
//
// Every picture here is drawn by this file: discs, rounded boxes and strokes from distance functions, letters of
// Roboto Medium (hud_prompts_font.c). Nothing is copied from the game or from a controller's maker. The game's bytes
// in guest memory are never touched: the copy that goes to the GPU is.
//
// The pictures are redrawn by group, so that one style is never mixed inside a group: the four face buttons, the four
// shoulder buttons, plus and minus, each stick click, the directions, the four arrow discs (the character wheel).
#include "hud_prompts.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "input_map.h"
#include "sha256.h"
#include "platform/platform.h"

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // host.h, as declared there

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#include "imstb_truetype.h"
#pragma clang diagnostic pop

// ---- the textures of game version 196608 with button pictures (positions are texels). The first three were decoded
// and measured in frame dumps; the other two were found on 2026-11-23 by looking for the four letters in every texture
// of the game's interface files (work/s16/tex_scan.py): small wheels of three discs with their letters, one of them
// the four buttons next to "Preparativi" in the camp, and a second sheet of the skill wheel's discs.
// A first look that costs next to nothing: FNV-1a of one kilobyte in a row of pictures. Only a texture that passes it
// is hashed whole, so the game's other textures of these sizes upload as fast as before.
enum { ATLAS = 1024, PROBE_SIZE = 1024 };
enum { SHEET_ATLAS, SHEET_HUD, SHEET_WHEEL, SHEET_TRIO, SHEET_WHEEL2, N_SHEETS };
static const char *const sheet_names[N_SHEETS] = {"glyph set", "battle HUD sheet", "skill wheel sheet",
                                                  "three-disc wheels", "second skill wheel sheet"};
static const struct { uint16_t w, h; uint32_t probe_at; uint64_t probe; uint8_t sha[32]; } sheets[N_SHEETS] = {
    [SHEET_ATLAS] = {ATLAS, ATLAS, 0x86000, 0x3de56ef1ba61f545ull,
                     {0xe4, 0x31, 0xe9, 0x6e, 0xbb, 0x07, 0x20, 0x92, 0x3c, 0x38, 0xa2, 0xf4, 0x5e, 0xc9, 0x32, 0x9b,
                      0x21, 0x6a, 0xb9, 0xc7, 0x44, 0x48, 0x36, 0xcf, 0x2a, 0xf5, 0x95, 0x9f, 0xa4, 0x23, 0x61, 0xc6}},
    [SHEET_HUD] = {2048, 1024, 0x110f00, 0xb3085e007e7903ffull,
                   {0xbd, 0xf2, 0x78, 0x9b, 0x38, 0xe1, 0xc1, 0x3c, 0x46, 0x7a, 0xc8, 0xc1, 0x85, 0x32, 0x0e, 0x92,
                    0x4b, 0xe1, 0x57, 0x27, 0x63, 0x0d, 0xfd, 0x1c, 0x06, 0x16, 0xa2, 0xa8, 0x7d, 0x1b, 0x94, 0x34}},
    [SHEET_WHEEL] = {2048, 512, 0x58000, 0x6f39678c554ebcd6ull,
                     {0x41, 0xea, 0xd3, 0x72, 0xc5, 0xf1, 0x52, 0x02, 0xb1, 0x1f, 0x47, 0x6d, 0x1f, 0x7f, 0xd8, 0x27,
                      0x6a, 0x4e, 0x81, 0xaf, 0x5b, 0x47, 0x1c, 0x07, 0xad, 0x3b, 0xf0, 0x65, 0x77, 0xb7, 0x8e, 0xb8}},
    // entry 0xb01ff6ec texture 19 of the UI pack 0x6df2ba2a.fdata (BC3, linear in the file)
    [SHEET_TRIO] = {2048, 1024, 0x103cc0, 0xeb4b28a1ea19c432ull,
                    {0x80, 0x16, 0x6b, 0x78, 0xd6, 0x66, 0x79, 0xf9, 0xff, 0xb9, 0x13, 0x09, 0x72, 0xeb, 0xa1, 0xe5,
                     0x6d, 0xe0, 0x6c, 0x89, 0x65, 0x2d, 0xb0, 0x41, 0xd7, 0x8f, 0xf4, 0x46, 0xfd, 0x3a, 0xfc, 0xcb}},
    // entry 0x975b7775 texture 0 of 0xcc3ddbb3.fdata (BC3, tiled in the file)
    [SHEET_WHEEL2] = {2048, 1024, 0x15a4c0, 0x2b5d803d8e3af035ull,
                      {0xf0, 0x0d, 0x4f, 0xa2, 0xcb, 0x82, 0x51, 0xe6, 0x8b, 0xc7, 0x4f, 0xd8, 0x50, 0x1d, 0xfc, 0xf7,
                       0x0b, 0x70, 0x4a, 0x8b, 0x90, 0x52, 0xe3, 0x90, 0x00, 0xb6, 0x25, 0x6e, 0x8a, 0xd4, 0x88, 0x2a}},
};

// A picture of one Switch button in the atlas: the box the game's own picture fills, the gamepad input the game draws
// there, and the group it is redrawn with. PLAIN: the game draws the sign alone, without a disc.
enum { G_FACE = 1, G_SHOULDER, G_MENU, G_LCLICK, G_RCLICK, G_DPAD, G_DIRECTIONS, G_ARROWS };
enum { PLAIN = 1 };
typedef struct { uint16_t x, y, w, h; uint8_t button, native, group, flags; } Cell;
// The first cells by name, for the other two textures' pictures of the same buttons.
enum { C_X, C_Y, C_A, C_B, C_L, C_R, C_ZL, C_ZR, C_PLUS_PLAIN, C_MINUS_PLAIN, C_PLUS, C_MINUS, C_LCLICK, C_RCLICK };
static const Cell cells[] = {
    {5, 515, 42, 42, B_X, PAD_NORTH, G_FACE, 0},        {59, 515, 42, 42, B_Y, PAD_WEST, G_FACE, 0},
    {923, 479, 42, 42, B_A, PAD_EAST, G_FACE, 0},       {977, 479, 42, 42, B_B, PAD_SOUTH, G_FACE, 0},
    {493, 574, 40, 32, B_L, PAD_L, G_SHOULDER, 0},      {545, 574, 40, 32, B_R, PAD_R, G_SHOULDER, 0},
    {601, 572, 38, 36, B_ZL, PAD_ZL, G_SHOULDER, 0},    {655, 572, 38, 36, B_ZR, PAD_ZR, G_SHOULDER, 0},
    {9, 627, 34, 34, B_PLUS, PAD_START, G_MENU, PLAIN}, {63, 627, 34, 34, B_MINUS, PAD_BACK, G_MENU, PLAIN},
    {118, 628, 33, 33, B_PLUS, PAD_START, G_MENU, 0},   {172, 628, 33, 33, B_MINUS, PAD_BACK, G_MENU, 0},
    {59, 567, 41, 46, B_STICK_L, PAD_LSTICK, G_LCLICK, 0},
    {113, 567, 41, 46, B_STICK_R, PAD_RSTICK, G_RCLICK, 0},
    {697, 619, 34, 34, B_DOWN, PAD_DPAD_DOWN, G_DPAD, 0},    {735, 619, 34, 34, B_LEFT, PAD_DPAD_LEFT, G_DPAD, 0},
    {773, 619, 34, 34, B_RIGHT, PAD_DPAD_RIGHT, G_DPAD, 0},  {811, 619, 34, 34, B_UP, PAD_DPAD_UP, G_DPAD, 0},
    // the four direction buttons of a left Joy-Con
    {761, 515, 42, 44, B_DOWN, PAD_DPAD_DOWN, G_DIRECTIONS, 0},  {814, 515, 43, 42, B_LEFT, PAD_DPAD_LEFT, G_DIRECTIONS, 0},
    {869, 515, 44, 42, B_RIGHT, PAD_DPAD_RIGHT, G_DIRECTIONS, 0}, {923, 531, 42, 44, B_UP, PAD_DPAD_UP, G_DIRECTIONS, 0},
    // the d-pad as discs with an arrow: the character wheel ("Cambio") draws the direction of each character with one;
    // a pad's d-pad keeps them, the keyboard gets its keys there (1 to 4 by default)
    {222, 624, 40, 40, B_RIGHT, PAD_DPAD_RIGHT, G_ARROWS, 0}, {276, 624, 40, 40, B_DOWN, PAD_DPAD_DOWN, G_ARROWS, 0},
    {330, 624, 40, 40, B_UP, PAD_DPAD_UP, G_ARROWS, 0},       {384, 624, 40, 40, B_LEFT, PAD_DPAD_LEFT, G_ARROWS, 0},
};
enum { N_CELLS = sizeof cells / sizeof *cells };

// The four face buttons as dots, with the ones to press filled: `buttons` are the Switch buttons the game fills.
typedef struct { uint16_t x, y, w, h; uint16_t buttons; } Diamond;
static const Diamond diamonds[] = {
    {220, 515, 44, 42, 1 << B_Y | 1 << B_A}, {275, 514, 42, 44, 1 << B_X | 1 << B_B},
    {329, 515, 42, 42, 1 << B_B},            {383, 515, 42, 42, 1 << B_Y},
    {437, 515, 42, 42, 1 << B_A},            {491, 515, 42, 42, 1 << B_X},
};
enum { N_DIAMONDS = sizeof diamonds / sizeof *diamonds };

// The left and the right stick as the game draws them where it says "move" and "look around". On the keyboard they
// are the four keys that move and the mouse.
static const struct { uint16_t x, y, w, h; } sticks[2] = {{977, 536, 41, 37}, {5, 572, 41, 37}};

// The battle HUD's sheet: its own copies of some pictures, the same white masks (the discs of the skill wheel's
// buttons that have a skill, the stick click that changes the character, R and ZR). `cell`: the atlas picture of the
// same button, which they follow.
static const struct { uint16_t x, y, w, h; uint8_t cell; } hud_copies[] = {
    {994, 546, 40, 40, C_B},       {1048, 546, 40, 40, C_X}, {1102, 546, 40, 40, C_Y},  {1490, 542, 40, 40, C_A},
    {1156, 544, 39, 43, C_LCLICK}, {1210, 550, 39, 32, C_R}, {1782, 587, 36, 34, C_ZR},
};
enum { N_HUD_COPIES = sizeof hud_copies / sizeof *hud_copies };

// The skill wheel's sheet: dark discs, one for each face button where a Switch controller has it (X at the top, Y on
// the left, A on the right, B at the bottom), each with its letter in a light colour, or in grey for a wheel that is
// off. Fourteen wheels in a row and one below it, WHEEL_PITCH apart, each with the discs of some of the buttons: the
// game puts two of them together, the buttons with a skill and those without. A disc's centre is WHEEL_ARM from its
// wheel's. Unlike the other two this texture has colour, and a letter is colour in an opaque disc: it is drawn again
// in the colour blocks alone, in the letter's and the disc's own colours.
enum { WHEEL_X = 212, WHEEL_Y = 212, WHEEL_PITCH = 134, WHEEL_ROW = 14, WHEEL_ARM = 36, WHEEL_ZONE = 28 };
enum { LOBE_X = 1, LOBE_Y = 2, LOBE_A = 4, LOBE_B = 8, LOBE_ALL = 15 };
static const struct { uint8_t lobes, grey; } wheels[] = {
    {LOBE_X | LOBE_Y | LOBE_B, 0}, {LOBE_X | LOBE_Y, 0},          {LOBE_Y, 0},          {LOBE_X, 0},
    {LOBE_ALL, LOBE_ALL},          {LOBE_Y | LOBE_B, 0},          {LOBE_B, 0},          {LOBE_X | LOBE_B, 0},
    {LOBE_X | LOBE_Y | LOBE_A, 0}, {LOBE_Y | LOBE_A, 0},          {LOBE_A, LOBE_A},     {LOBE_X | LOBE_A, 0},
    {LOBE_Y | LOBE_A | LOBE_B, 0}, {LOBE_A | LOBE_B, 0},
    {LOBE_X | LOBE_A | LOBE_B, 0},   // the one below the first
};
enum { N_WHEELS = sizeof wheels / sizeof *wheels };
static const struct { int8_t dx, dy; uint8_t cell; } lobes[4] = {   // in the order of the LOBE_ bits
    {0, -WHEEL_ARM, C_X}, {-WHEEL_ARM, 0, C_Y}, {WHEEL_ARM, 0, C_A}, {0, WHEEL_ARM, C_B}};
static const uint16_t wheel_light = 0xfffc, wheel_grey = 0x5289;   // the letters' colours, RGB 5:6:5

// The other two sheets' wheels, each at its own place: a wheel's discs are WHEEL_ARM from its centre as on the skill
// wheel's sheet, with the letters a quieter grey (two shades: RGB 152,152,136 on the dark blue discs, 119,117,103 on
// the black ones). The three-disc wheels leave out the disc where the game puts a white one (the HUD sheet's).
typedef struct { uint16_t x, y; uint8_t lobes; uint16_t ink; } WheelAt;
enum { INK_GREY = 0x9cd1, INK_DARK = 0x73ac };
static const WheelAt trio_wheels[] = {
    {1872, 382, LOBE_X | LOBE_Y | LOBE_B, INK_DARK}, {1872, 516, LOBE_X | LOBE_Y | LOBE_A, INK_GREY},
    {1872, 650, LOBE_Y | LOBE_A | LOBE_B, INK_GREY}, {66, 698, LOBE_X | LOBE_A | LOBE_B, INK_GREY},
};
// The second skill wheel sheet: a row with letters (its other wheels are discs without letters) and a row below it.
static const WheelAt wheel2_wheels[] = {
    {355, 694, LOBE_X | LOBE_Y | LOBE_B, INK_GREY},  {488, 694, LOBE_X | LOBE_Y | LOBE_A, INK_GREY},
    {621, 694, LOBE_Y | LOBE_A | LOBE_B, INK_GREY},  {754, 694, LOBE_X | LOBE_A | LOBE_B, INK_GREY},
    {887, 694, LOBE_X | LOBE_Y | LOBE_B, INK_GREY},  {1020, 694, LOBE_X | LOBE_Y, INK_GREY},
    {355, 827, LOBE_ALL, INK_GREY},                  {488, 827, LOBE_Y | LOBE_B, INK_GREY},
    {621, 827, LOBE_B, INK_GREY},                    {754, 827, LOBE_X | LOBE_B, INK_GREY},
    {887, 827, LOBE_X | LOBE_Y | LOBE_A, INK_GREY},  {1020, 827, LOBE_Y | LOBE_A, INK_GREY},
    {1153, 827, LOBE_A, INK_GREY},                   {1286, 827, LOBE_X | LOBE_A, INK_GREY},
    {1419, 827, LOBE_Y | LOBE_A | LOBE_B, INK_GREY}, {1552, 827, LOBE_A | LOBE_B, INK_GREY},
    {1685, 827, LOBE_X | LOBE_A | LOBE_B, INK_GREY}, {1818, 827, LOBE_X, INK_GREY},
};

// ---- what a picture shows
enum { P_KEEP, P_DISC, P_BOX, P_KEY, P_MOUSE, P_DPAD, P_MOVE_KEYS };   // P_KEEP: the game's own picture stays
enum { S_TEXT, S_CROSS, S_CIRCLE, S_SQUARE, S_TRIANGLE, S_PLUS, S_MINUS, S_LINES, S_VIEW, S_SHARE,
       S_SPACE, S_SHIFT, S_ENTER, S_UP, S_DOWN, S_LEFT, S_RIGHT };
typedef struct { uint8_t shape, sign; char text[6]; } Pic;   // P_MOUSE: sign is the button (0 left, 1 right, 2 middle,
                                                             // 3 none: the mouse itself)

typedef struct {
    Pic cell[N_CELLS], diamond_pic[N_DIAMONDS], stick[2];
    uint8_t diamond_dots[N_DIAMONDS];   // 0: keep; else the dots to fill, bit pad - PAD_SOUTH
    uint8_t device;                     // D_*: whose pictures these are, for the log
} Plan;
enum { D_GAME, D_KEYBOARD, D_XBOX, D_PLAYSTATION, D_NINTENDO };

static Pic pic_text(int shape, const char *text) {
    Pic p;
    memset(&p, 0, sizeof p);
    p.shape = (uint8_t)shape, p.sign = S_TEXT;
    snprintf(p.text, sizeof p.text, "%s", text);
    return p;
}

static Pic pic_sign(int shape, int sign) {
    Pic p;
    memset(&p, 0, sizeof p);
    p.shape = (uint8_t)shape, p.sign = (uint8_t)sign;
    return p;
}

// The picture of a gamepad input on a pad of that family. labels: what the pad's maker prints on its south, east, west
// and north buttons, when the pad says (NULL: as the family prints them).
static Pic pic_pad(int pad, int family, const int labels[4]) {
    int ps = family == PAD_FAMILY_PLAYSTATION, nin = family == PAD_FAMILY_NINTENDO;
    switch (pad) {
    case PAD_SOUTH: case PAD_EAST: case PAD_WEST: case PAD_NORTH: {
        static const uint8_t printed[3][4] = {
            {PAD_LABEL_A, PAD_LABEL_B, PAD_LABEL_X, PAD_LABEL_Y},
            {PAD_LABEL_CROSS, PAD_LABEL_CIRCLE, PAD_LABEL_SQUARE, PAD_LABEL_TRIANGLE},
            {PAD_LABEL_B, PAD_LABEL_A, PAD_LABEL_Y, PAD_LABEL_X}};
        int label = labels ? labels[pad - PAD_SOUTH] : PAD_LABEL_NONE;
        if (label == PAD_LABEL_NONE) label = printed[ps ? 1 : nin ? 2 : 0][pad - PAD_SOUTH];
        switch (label) {
        case PAD_LABEL_A: return pic_text(P_DISC, "A");
        case PAD_LABEL_B: return pic_text(P_DISC, "B");
        case PAD_LABEL_X: return pic_text(P_DISC, "X");
        case PAD_LABEL_Y: return pic_text(P_DISC, "Y");
        case PAD_LABEL_CROSS: return pic_sign(P_DISC, S_CROSS);
        case PAD_LABEL_CIRCLE: return pic_sign(P_DISC, S_CIRCLE);
        case PAD_LABEL_SQUARE: return pic_sign(P_DISC, S_SQUARE);
        default: return pic_sign(P_DISC, S_TRIANGLE);
        }
    }
    case PAD_L: return pic_text(P_BOX, ps ? "L1" : nin ? "L" : "LB");
    case PAD_R: return pic_text(P_BOX, ps ? "R1" : nin ? "R" : "RB");
    case PAD_ZL: return pic_text(P_BOX, ps ? "L2" : nin ? "ZL" : "LT");
    case PAD_ZR: return pic_text(P_BOX, ps ? "R2" : nin ? "ZR" : "RT");
    case PAD_LSTICK: return pic_text(P_BOX, ps ? "L3" : "LS");
    case PAD_RSTICK: return pic_text(P_BOX, ps ? "R3" : "RS");
    case PAD_START: return pic_sign(P_DISC, nin ? S_PLUS : S_LINES);
    case PAD_BACK: return pic_sign(P_DISC, nin ? S_MINUS : ps ? S_SHARE : S_VIEW);
    case PAD_DPAD_UP: return pic_sign(P_DPAD, S_UP);
    case PAD_DPAD_DOWN: return pic_sign(P_DPAD, S_DOWN);
    case PAD_DPAD_LEFT: return pic_sign(P_DPAD, S_LEFT);
    case PAD_DPAD_RIGHT: return pic_sign(P_DPAD, S_RIGHT);
    case PAD_LPADDLE1: return pic_text(P_BOX, "P1");
    case PAD_RPADDLE1: return pic_text(P_BOX, "P2");
    case PAD_LPADDLE2: return pic_text(P_BOX, "P3");
    case PAD_RPADDLE2: return pic_text(P_BOX, "P4");
    }
    return pic_sign(P_KEEP, 0);
}

// The picture of a key or a mouse button: a key cap with what the key says.
static Pic pic_key(int key) {
    static const char *const named[] = {
        [KEY_TAB] = "Tab",     [KEY_LCTRL] = "Ctrl",    [KEY_RCTRL] = "Ctrl",     [KEY_LALT] = "Alt",
        [KEY_BACKSPACE] = "Bksp", [KEY_CAPSLOCK] = "Caps", [KEY_INSERT] = "Ins",  [KEY_DELETE] = "Del",
        [KEY_HOME] = "Home",   [KEY_END] = "End",       [KEY_PAGEUP] = "PgUp",    [KEY_PAGEDOWN] = "PgDn",
        [KEY_F2] = "F2",       [KEY_F3] = "F3",         [KEY_F4] = "F4",          [KEY_F5] = "F5",
        [KEY_F6] = "F6",       [KEY_F7] = "F7",         [KEY_F9] = "F9",          [KEY_F12] = "F12",
        [KEY_NUM_ADD] = "N+",  [KEY_NUM_SUB] = "N-",    [KEY_NUM_MUL] = "N*",     [KEY_NUM_DIV] = "N/",
        [KEY_NUM_DECIMAL] = "N.", [KEY_MOUSE_X1] = "M4", [KEY_MOUSE_X2] = "M5",   [KEY_ESCAPE] = "Esc",
    };
    static const char us[] = "-=[];',./\\`";   // KEY_MINUS..KEY_GRAVE on a US keyboard
    char t[6] = "";
    if (key <= KEY_NONE || key >= KEY_COUNT) return pic_sign(P_KEEP, 0);
    if (key >= KEY_A && key <= KEY_Z) t[0] = (char)('A' + key - KEY_A);
    else if (key >= KEY_0 && key <= KEY_9) t[0] = (char)('0' + key - KEY_0);
    else if (key >= KEY_NUM0 && key <= KEY_NUM9) t[0] = 'N', t[1] = (char)('0' + key - KEY_NUM0);
    else if (key >= KEY_MINUS && key <= KEY_GRAVE) {
        uint32_t c = input_key_char(key);   // the character on the key in the keyboard layout in use
        t[0] = c > 0x20 && c < 0x7f ? (char)c : us[key - KEY_MINUS];
    } else if (key == KEY_MOUSE_L || key == KEY_MOUSE_R || key == KEY_MOUSE_M) return pic_sign(P_MOUSE, key - KEY_MOUSE_L);
    else if (key == KEY_SPACE) return pic_sign(P_KEY, S_SPACE);
    else if (key == KEY_LSHIFT || key == KEY_RSHIFT) return pic_sign(P_KEY, S_SHIFT);
    else if (key == KEY_ENTER) return pic_sign(P_KEY, S_ENTER);
    else if (key == KEY_UP) return pic_sign(P_KEY, S_UP);
    else if (key == KEY_DOWN) return pic_sign(P_KEY, S_DOWN);
    else if (key == KEY_LEFT) return pic_sign(P_KEY, S_LEFT);
    else if (key == KEY_RIGHT) return pic_sign(P_KEY, S_RIGHT);
    else if (named[key]) return pic_text(P_KEY, named[key]);
    else return pic_sign(P_KEEP, 0);
    if (t[0] >= 'a' && t[0] <= 'z') t[0] = (char)(t[0] - 'a' + 'A');
    return pic_text(P_KEY, t);
}

static int pic_same(const Pic *a, const Pic *b) { return !memcmp(a, b, sizeof *a); }

// The pictures to draw for the devices as they are. mode: TOTK_BUTTON_PROMPTS.
static void plan_make(Plan *p, const InputPromptState *in, const char *mode) {
    memset(p, 0, sizeof *p);
    if (!mode || !strcasecmp(mode, "game")) return;
    int forced = !strcasecmp(mode, "xbox") ? PAD_FAMILY_XBOX : !strcasecmp(mode, "playstation") ? PAD_FAMILY_PLAYSTATION
               : !strcasecmp(mode, "nintendo") ? PAD_FAMILY_NINTENDO : -1;
    int keyboard = forced < 0 && in->keyboard, family = forced >= 0 ? forced : in->family;
    const int *labels = forced >= 0 ? NULL : in->labels;
    if (!keyboard && in->layout < 0) return;   // a maker's pictures were asked for and no gamepad is connected
    p->device = keyboard ? D_KEYBOARD : family == PAD_FAMILY_PLAYSTATION ? D_PLAYSTATION
              : family == PAD_FAMILY_NINTENDO ? D_NINTENDO : D_XBOX;
    Pic want[N_CELLS];
    uint32_t redraw = 0;   // by group
    for (int i = 0; i < N_CELLS; i++) {
        const Cell *c = &cells[i];
        // the game's own picture: a stick click and a direction look the same on every maker's pad
        Pic own = pic_pad(c->native, c->group >= G_LCLICK ? family : PAD_FAMILY_NINTENDO, NULL);
        want[i] = keyboard ? pic_key(in->key[c->button]) : pic_pad(in->pad[c->button], family, labels);
        if (want[i].shape != P_KEEP && !pic_same(&want[i], &own)) redraw |= 1u << c->group;
    }
    for (int i = 0; i < N_CELLS; i++)
        if (redraw >> cells[i].group & 1) p->cell[i] = want[i];
    if (keyboard) {
        int moves = 1;
        for (int a = B_SL_LEFT; a <= B_SL_DOWN; a++) moves &= in->key[a] != KEY_NONE;
        if (moves) p->stick[0] = pic_sign(P_MOVE_KEYS, 0);
        p->stick[1] = pic_sign(P_MOUSE, 3);
    }
    for (int i = 0; i < N_DIAMONDS; i++) {
        const Diamond *d = &diamonds[i];
        static const uint8_t own_dot[4] = {1 << (PAD_EAST - PAD_SOUTH), 1 << (PAD_SOUTH - PAD_SOUTH),   // A, B
                                           1 << (PAD_NORTH - PAD_SOUTH), 1 << (PAD_WEST - PAD_SOUTH)};  // X, Y
        int dots = 0, own = 0, on_face = !keyboard, count = 0, one = 0;
        for (int b = B_A; b <= B_Y; b++) {
            if (!(d->buttons >> b & 1)) continue;
            count++, one = b;
            own |= own_dot[b];
            int pad = in->pad[b];
            if (pad >= PAD_SOUTH && pad <= PAD_NORTH) dots |= 1 << (pad - PAD_SOUTH);
            else on_face = 0;
        }
        if (on_face) {
            if (dots != own) p->diamond_dots[i] = (uint8_t)dots;
        } else if (count == 1) {   // one button, and it is not a face button: its own picture
            p->diamond_pic[i] = keyboard ? pic_key(in->key[one]) : pic_pad(in->pad[one], family, labels);
        }
    }
}

// ---- drawing: coverage of white, 0..1, in a canvas over one cell
enum { CELL_MAX = 56, SS = 4, BIG = CELL_MAX * SS };   // the tallest picture, a stick click, covers 52 rows of blocks
typedef struct {
    int w, h;          // texels, multiples of 4, CELL_MAX at most
    float ox, oy;      // the atlas position of its corner
    float a[CELL_MAX * CELL_MAX];
} Canvas;

static float cover(float d) { return d <= -.5f ? 1 : d >= .5f ? 0 : .5f - d; }   // d: distance to the edge, negative inside

static void put(Canvas *c, int x, int y, float v, int cut) {
    float *p = &c->a[y * c->w + x];
    *p = cut ? fminf(*p, 1 - v) : fmaxf(*p, v);
}

#define EACH(c) \
    for (int y = 0; y < (c)->h; y++) \
        for (int x = 0; x < (c)->w; x++)
#define PX(c) ((c)->ox + (float)x + .5f)
#define PY(c) ((c)->oy + (float)y + .5f)

static void disc(Canvas *c, int cut, float cx, float cy, float r) {
    EACH(c) put(c, x, y, cover(hypotf(PX(c) - cx, PY(c) - cy) - r), cut);
}

static void ring(Canvas *c, int cut, float cx, float cy, float r, float stroke) {
    EACH(c) put(c, x, y, cover(fabsf(hypotf(PX(c) - cx, PY(c) - cy) - r) - stroke * .5f), cut);
}

static float box_distance(float px, float py, float hx, float hy, float round) {
    float qx = fabsf(px) - (hx - round), qy = fabsf(py) - (hy - round);
    return hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - round;
}

// A box of half size hx, hy with rounded corners; stroke > 0: only its outline, that wide, inside the box.
static void box(Canvas *c, int cut, float cx, float cy, float hx, float hy, float round, float stroke) {
    EACH(c) {
        float d = box_distance(PX(c) - cx, PY(c) - cy, hx, hy, round);
        put(c, x, y, cover(stroke > 0 ? fabsf(d + stroke * .5f) - stroke * .5f : d), cut);
    }
}

static void line(Canvas *c, int cut, float ax, float ay, float bx, float by, float half) {
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    EACH(c) {
        float px = PX(c) - ax, py = PY(c) - ay;
        float t = len2 > 0 ? fminf(1, fmaxf(0, (px * dx + py * dy) / len2)) : 0;
        put(c, x, y, cover(hypotf(px - t * dx, py - t * dy) - half), cut);
    }
}

// A filled triangle; its points in either order.
static void triangle(Canvas *c, int cut, const float p[6]) {
    float area = (p[2] - p[0]) * (p[5] - p[1]) - (p[4] - p[0]) * (p[3] - p[1]), sign = area < 0 ? -1.f : 1.f;
    EACH(c) {
        float d = -1e9f;
        for (int i = 0; i < 3; i++) {
            const float *a = &p[2 * i], *b = &p[2 * ((i + 1) % 3)];
            float ex = b[0] - a[0], ey = b[1] - a[1], len = hypotf(ex, ey);
            if (len > 0) d = fmaxf(d, sign * (ex * (PY(c) - a[1]) - ey * (PX(c) - a[0])) / -len);
        }
        put(c, x, y, cover(d), cut);
    }
}

// An arrow head pointing at dir (S_UP..S_RIGHT), its tip `reach` from the centre and `size` long.
static void arrow(Canvas *c, int cut, int dir, float cx, float cy, float reach, float size) {
    float ux = dir == S_LEFT ? -1.f : dir == S_RIGHT ? 1.f : 0, uy = dir == S_UP ? -1.f : dir == S_DOWN ? 1.f : 0;
    float tx = cx + ux * reach, ty = cy + uy * reach, bx = tx - ux * size, by = ty - uy * size, w = size * .62f;
    float p[6] = {tx, ty, bx - uy * w, by + ux * w, bx + uy * w, by - ux * w};
    triangle(c, cut, p);
}

// ---- letters: hud_prompts_font() through stb_truetype, at SS times the size, a little bolder, then averaged down
static stbtt_fontinfo font;
static int font_state;      // 0 not tried, 1 ready, -1 no font
static int font_cap;        // the height of a capital, font units

static int font_ready(void) {
    if (!font_state) {
        size_t n = 0;
        const unsigned char *data = hud_prompts_font(&n);
        int x0, y0, x1, y1;
        font_state = data && n > 100 && stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0)) &&
                     stbtt_GetCodepointBox(&font, 'H', &x0, &y0, &x1, &y1) && y1 > 0 ? 1 : -1;
        if (font_state > 0) font_cap = y1;
        else rt_log("prompts: no font for the button pictures; their letters are left out");
    }
    return font_state > 0;
}

// text with capitals `cap` texels high, centred on cx, cy, made smaller when it is wider than max_w.
static void text(Canvas *c, int cut, const char *s, float cx, float cy, float cap, float max_w, float bold) {
    static uint8_t big[BIG * BIG], wide[BIG * BIG], glyph[BIG * BIG];
    if (!font_ready()) return;
    int n = (int)strlen(s), pen = 0, left = 0, right = 0;
    for (int i = 0; i < n; i++) {
        int adv, lsb, x0 = 0, y0, x1 = 0, y1;
        stbtt_GetCodepointHMetrics(&font, (unsigned char)s[i], &adv, &lsb);
        stbtt_GetCodepointBox(&font, (unsigned char)s[i], &x0, &y0, &x1, &y1);
        if (!i) left = pen + x0;
        right = pen + x1;
        pen += adv + (i + 1 < n ? stbtt_GetCodepointKernAdvance(&font, (unsigned char)s[i], (unsigned char)s[i + 1]) : 0);
    }
    float scale = cap / (float)font_cap, width = (float)(right - left) * scale + 2 * bold;
    if (width > max_w) scale *= max_w / width, cap *= max_w / width;
    int W = c->w * SS, H = c->h * SS;
    memset(big, 0, (size_t)W * H);
    float s4 = scale * SS, x4 = (cx - c->ox) * SS - (float)(left + right) * .5f * s4, base = (cy - c->oy) * SS + cap * SS * .5f;
    float by = floorf(base), sy = base - by;
    pen = 0;
    for (int i = 0; i < n; i++) {
        int adv, lsb, x0, y0, x1, y1, ch = (unsigned char)s[i];
        float gx = x4 + (float)pen * s4, bx = floorf(gx), sx = gx - bx;
        stbtt_GetCodepointHMetrics(&font, ch, &adv, &lsb);
        stbtt_GetCodepointBitmapBoxSubpixel(&font, ch, s4, s4, sx, sy, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw <= BIG && gh <= BIG) {
            memset(glyph, 0, (size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&font, glyph, gw, gh, gw, s4, s4, sx, sy, ch);
            for (int k = 0; k < gh; k++)
                for (int j = 0; j < gw; j++) {
                    int X = (int)bx + x0 + j, Y = (int)by + y0 + k;
                    if (X >= 0 && X < W && Y >= 0 && Y < H && glyph[k * gw + j] > big[Y * W + X]) big[Y * W + X] = glyph[k * gw + j];
                }
        }
        pen += adv + (i + 1 < n ? stbtt_GetCodepointKernAdvance(&font, ch, (unsigned char)s[i + 1]) : 0);
    }
    const uint8_t *src = big;
    int r = (int)lroundf(bold * SS);
    if (r > 0) {   // every stroke r wider on each side
        for (int Y = 0; Y < H; Y++)
            for (int X = 0; X < W; X++) {
                uint8_t m = 0;
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        int u = X + dx, v = Y + dy;
                        if (dx * dx + dy * dy <= r * r && u >= 0 && u < W && v >= 0 && v < H && big[v * W + u] > m) m = big[v * W + u];
                    }
                wide[Y * W + X] = m;
            }
        src = wide;
    }
    EACH(c) {
        unsigned sum = 0;
        for (int k = 0; k < SS; k++)
            for (int j = 0; j < SS; j++) sum += src[(y * SS + k) * W + x * SS + j];
        put(c, x, y, (float)sum / (255.f * SS * SS), cut);
    }
}

// The sign of a picture around cx, cy, for a disc of radius u.
static void sign(Canvas *c, int cut, const Pic *p, float cx, float cy, float u) {
    float half = .085f * u, thin = .072f * u;   // half a stroke
    switch (p->sign) {
    case S_TEXT: {
        size_t n = strlen(p->text);
        text(c, cut, p->text, cx, cy, (n < 2 ? .84f : n < 3 ? .70f : .56f) * u, 1.42f * u, .3f);
        break;
    }
    case S_CROSS:
        line(c, cut, cx - .37f * u, cy - .37f * u, cx + .37f * u, cy + .37f * u, thin);
        line(c, cut, cx + .37f * u, cy - .37f * u, cx - .37f * u, cy + .37f * u, thin);
        break;
    case S_CIRCLE: ring(c, cut, cx, cy, .43f * u, 2 * thin); break;
    case S_SQUARE: box(c, cut, cx, cy, .42f * u, .42f * u, .04f * u, 2 * thin); break;
    case S_TRIANGLE: {
        float top = cy - .50f * u, low = cy + .30f * u, w = .47f * u;
        line(c, cut, cx, top, cx + w, low, thin);
        line(c, cut, cx + w, low, cx - w, low, thin);
        line(c, cut, cx - w, low, cx, top, thin);
        break;
    }
    case S_PLUS: case S_MINUS:
        if (p->sign == S_PLUS) line(c, cut, cx, cy - .46f * u, cx, cy + .46f * u, .11f * u);
        line(c, cut, cx - .46f * u, cy, cx + .46f * u, cy, .11f * u);
        break;
    case S_LINES:
        for (int k = -1; k <= 1; k++) line(c, cut, cx - .40f * u, cy + (float)k * .30f * u, cx + .40f * u, cy + (float)k * .30f * u, .075f * u);
        break;
    case S_VIEW: {   // two sheets, one behind the other
        float hx = .30f * u, hy = .25f * u, d = .13f * u;
        Canvas back = *c;
        memset(back.a, 0, sizeof back.a);
        box(&back, 0, cx + d, cy - d, hx, hy, .05f * u, 1.7f * half);
        box(&back, 1, cx - d, cy + d, hx + half, hy + half, .05f * u, 0);
        box(&back, 0, cx - d, cy + d, hx, hy, .05f * u, 1.7f * half);
        EACH(c) put(c, x, y, back.a[y * c->w + x], cut);
        break;
    }
    case S_SHARE:   // three rays over a bar
        line(c, cut, cx, cy - .46f * u, cx, cy - .08f * u, half);
        line(c, cut, cx - .42f * u, cy - .30f * u, cx - .20f * u, cy - .04f * u, half);
        line(c, cut, cx + .42f * u, cy - .30f * u, cx + .20f * u, cy - .04f * u, half);
        line(c, cut, cx - .34f * u, cy + .32f * u, cx + .34f * u, cy + .32f * u, half);
        break;
    case S_SPACE:
        line(c, cut, cx - .50f * u, cy + .02f * u, cx - .50f * u, cy + .30f * u, half);
        line(c, cut, cx - .50f * u, cy + .30f * u, cx + .50f * u, cy + .30f * u, half);
        line(c, cut, cx + .50f * u, cy + .30f * u, cx + .50f * u, cy + .02f * u, half);
        break;
    case S_SHIFT:
        arrow(c, cut, S_UP, cx, cy, .50f * u, .50f * u);
        box(c, cut, cx, cy + .22f * u, .15f * u, .26f * u, .02f * u, 0);
        break;
    case S_ENTER:
        line(c, cut, cx + .40f * u, cy - .36f * u, cx + .40f * u, cy + .14f * u, half);
        line(c, cut, cx + .40f * u, cy + .14f * u, cx - .20f * u, cy + .14f * u, half);
        arrow(c, cut, S_LEFT, cx, cy + .14f * u, .48f * u, .34f * u);
        break;
    case S_UP: case S_DOWN: case S_LEFT: case S_RIGHT: arrow(c, cut, p->sign, cx, cy, .40f * u, .66f * u); break;
    }
}

// One picture in the box bw x bh around cx, cy.
static void draw_pic(Canvas *c, const Pic *p, float cx, float cy, float bw, float bh, int plain) {
    float s = fminf(bw, bh), u = s * .5f;
    switch (p->shape) {
    case P_DISC:
        if (plain && p->sign != S_TEXT) { sign(c, 0, p, cx, cy, u * 1.15f); break; }
        disc(c, 0, cx, cy, u);
        sign(c, 1, p, cx, cy, u);
        break;
    case P_BOX: {
        float h = fminf(bh, bw * .74f);
        box(c, 0, cx, cy, bw * .5f, h * .5f, h * .30f, 0);
        text(c, 1, p->text, cx, cy, h * .50f, bw * .74f, .3f);
        break;
    }
    case P_KEY:
        box(c, 0, cx, cy, u, u, s * .20f, 0);
        if (p->sign == S_TEXT) text(c, 1, p->text, cx, cy, (strlen(p->text) < 2 ? .46f : .36f) * s, s * .74f, .3f);
        else sign(c, 1, p, cx, cy, u * .92f);
        break;
    case P_MOUSE: {   // a mouse from above, the button to press hollow
        float hx = .30f * s, hy = .46f * s, split = cy - .08f * s, gap = .035f * s;
        box(c, 0, cx, cy, hx, hy, hx, 0);
        line(c, 1, cx - hx, split, cx + hx, split, gap);
        if (p->sign == 2) box(c, 1, cx, cy - .28f * s, .065f * s, .12f * s, .065f * s, 0);
        else if (p->sign == 3) line(c, 1, cx, cy - hy, cx, split, gap);
        else {
            line(c, 1, cx, cy - hy, cx, split, gap);
            float side = p->sign ? 1.f : -1.f;
            Canvas half = *c;   // the button: the top of the mouse on its side of the middle
            memset(half.a, 0, sizeof half.a);
            box(&half, 0, cx, cy, hx - 2.2f * gap, hy - 2.2f * gap, hx - 2.2f * gap, 0);
            box(&half, 1, cx - side * hx, cy, hx + 1.6f * gap, 2 * hy, 0, 0);
            box(&half, 1, cx, split + hy, 2 * hx, hy + 1.6f * gap, 0, 0);
            EACH(c) put(c, x, y, half.a[y * c->w + x], 1);
        }
        break;
    }
    case P_DPAD:
        box(c, 0, cx, cy, u, .34f * u, .08f * u, 0);
        box(c, 0, cx, cy, .34f * u, u, .08f * u, 0);
        arrow(c, 1, p->sign, cx, cy, .84f * u, .36f * u);
        break;
    case P_MOVE_KEYS: {   // four keys as W, A, S and D lie
        float k = fminf(bw / 3.2f, bh / 2.1f), side = .45f * k;
        box(c, 0, cx, cy - .53f * k, side, side, .22f * k, 0);
        for (int i = -1; i <= 1; i++) box(c, 0, cx + (float)i * 1.06f * k, cy + .53f * k, side, side, .22f * k, 0);
        break;
    }
    }
}

// The four face buttons as dots in a box of side s, those of `dots` filled (bit pad - PAD_SOUTH).
static void draw_diamond(Canvas *c, int dots, float cx, float cy, float s) {
    static const float dx[4] = {0, 1, -1, 0}, dy[4] = {1, 0, 0, -1};   // south, east, west, north
    float k = s / 42.f;
    for (int i = 0; i < 4; i++) {
        float x = cx + dx[i] * 12.5f * k, y = cy + dy[i] * 12.5f * k;
        if (dots >> i & 1) disc(c, 0, x, y, 8.3f * k);
        else ring(c, 0, x, y, 6.25f * k, 2.9f * k);
    }
}

// ---- BC3: white texels with the canvas as alpha, over the blocks the box touches
// 0 for a box the canvas cannot hold or that leaves the texture of tw x th texels: nothing is drawn then.
static int canvas_over(Canvas *c, int x, int y, int w, int h, int tw, int th) {
    int x0 = x & ~3, y0 = y & ~3;
    c->ox = (float)x0, c->oy = (float)y0;
    c->w = ((x + w + 3) & ~3) - x0, c->h = ((y + h + 3) & ~3) - y0;
    memset(c->a, 0, sizeof c->a);
    return x >= 0 && y >= 0 && c->w <= CELL_MAX && c->h <= CELL_MAX && x0 + c->w <= tw && y0 + c->h <= th;
}

static void canvas_store(const Canvas *c, uint8_t *blocks, uint32_t atlas_w) {
    for (int by = 0; by < c->h / 4; by++)
        for (int bx = 0; bx < c->w / 4; bx++) {
            uint8_t a[16], lo = 255, hi = 0;
            for (int i = 0; i < 16; i++) {
                float v = c->a[(by * 4 + i / 4) * c->w + bx * 4 + i % 4];
                a[i] = (uint8_t)lroundf(fminf(1, fmaxf(0, v)) * 255);
                if (a[i] < lo) lo = a[i];
                if (a[i] > hi) hi = a[i];
            }
            uint8_t *b = blocks + (((size_t)c->oy / 4 + (size_t)by) * (atlas_w / 4) + (size_t)c->ox / 4 + (size_t)bx) * 16;
            uint64_t bits = 0;
            if (hi > lo)   // eight alphas: hi, lo, then six steps from hi down to lo
                for (int i = 0; i < 16; i++) {
                    int step = ((hi - a[i]) * 7 + (hi - lo) / 2) / (hi - lo);   // 0 = hi .. 7 = lo
                    bits |= (uint64_t)(step == 0 ? 0 : step == 7 ? 1 : step + 1) << (3 * i);
                }
            b[0] = hi, b[1] = lo;
            for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(bits >> (8 * i));
            memset(b + 8, 0xff, 4);   // both colours white
            memset(b + 12, 0, 4);
        }
}

// The canvas as the colour of opaque blocks: `ink` (RGB 5:6:5) where the canvas is white, over each block's own
// darker colour. The blocks' alpha stays as it is.
static void canvas_store_colour(const Canvas *c, uint8_t *blocks, uint32_t tex_w, uint16_t ink) {
    for (int by = 0; by < c->h / 4; by++)
        for (int bx = 0; bx < c->w / 4; bx++) {
            uint8_t *b = blocks + (((size_t)c->oy / 4 + (size_t)by) * (tex_w / 4) + (size_t)c->ox / 4 + (size_t)bx) * 16;
            // the disc's colour: the darker of the block's two (the other one is the game's letter, where there is one)
            uint16_t c0 = (uint16_t)(b[8] | b[9] << 8), c1 = (uint16_t)(b[10] | b[11] << 8);
            unsigned l0 = (c0 >> 11) * 2 + (c0 >> 5 & 63) + (c0 & 31) * 2, l1 = (c1 >> 11) * 2 + (c1 >> 5 & 63) + (c1 & 31) * 2;
            uint16_t back = l0 < l1 ? c0 : c1;
            uint32_t bits = 0;   // four colours: 0 ink, 1 back, 2 two thirds ink, 3 one third
            for (int i = 0; i < 16; i++) {
                float v = c->a[(by * 4 + i / 4) * c->w + bx * 4 + i % 4];
                bits |= (uint32_t)(v >= 5.f / 6 ? 0 : v >= .5f ? 2 : v >= 1.f / 6 ? 3 : 1) << (2 * i);
            }
            b[8] = (uint8_t)ink, b[9] = (uint8_t)(ink >> 8), b[10] = (uint8_t)back, b[11] = (uint8_t)(back >> 8);
            for (int i = 0; i < 4; i++) b[12 + i] = (uint8_t)(bits >> (8 * i));
        }
}

// The atlas: every picture of the plan in its cell.
static int plan_draw(uint8_t *blocks, uint32_t w, const Plan *p) {
    static Canvas c;
    int n = 0;
    for (int i = 0; i < N_CELLS; i++) {
        const Cell *k = &cells[i];
        if (p->cell[i].shape == P_KEEP) continue;
        if (!canvas_over(&c, k->x, k->y, k->w, k->h, ATLAS, ATLAS)) continue;
        draw_pic(&c, &p->cell[i], (float)k->x + (float)k->w * .5f, (float)k->y + (float)k->h * .5f, (float)k->w, (float)k->h,
                 k->flags & PLAIN);
        canvas_store(&c, blocks, w);
        n++;
    }
    for (int i = 0; i < 2; i++) {
        if (p->stick[i].shape == P_KEEP) continue;
        if (!canvas_over(&c, sticks[i].x, sticks[i].y, sticks[i].w, sticks[i].h, ATLAS, ATLAS)) continue;
        draw_pic(&c, &p->stick[i], (float)sticks[i].x + (float)sticks[i].w * .5f, (float)sticks[i].y + (float)sticks[i].h * .5f,
                 (float)sticks[i].w, (float)sticks[i].h, 0);
        canvas_store(&c, blocks, w);
        n++;
    }
    for (int i = 0; i < N_DIAMONDS; i++) {
        const Diamond *d = &diamonds[i];
        float cx = (float)d->x + (float)d->w * .5f, cy = (float)d->y + (float)d->h * .5f;
        if (!p->diamond_dots[i] && p->diamond_pic[i].shape == P_KEEP) continue;
        if (!canvas_over(&c, d->x, d->y, d->w, d->h, ATLAS, ATLAS)) continue;
        if (p->diamond_dots[i]) draw_diamond(&c, p->diamond_dots[i], cx, cy, 42);
        else draw_pic(&c, &p->diamond_pic[i], cx, cy, 42, 42, 0);
        canvas_store(&c, blocks, w);
        n++;
    }
    return n;
}

// The battle HUD's sheet: its copies get the pictures of their atlas cells.
static int plan_draw_hud(uint8_t *blocks, const Plan *p) {
    static Canvas c;
    int n = 0;
    for (int i = 0; i < N_HUD_COPIES; i++) {
        const Pic *pic = &p->cell[hud_copies[i].cell];
        int x = hud_copies[i].x, y = hud_copies[i].y, w = hud_copies[i].w, h = hud_copies[i].h;
        if (pic->shape == P_KEEP || !canvas_over(&c, x, y, w, h, sheets[SHEET_HUD].w, sheets[SHEET_HUD].h)) continue;
        draw_pic(&c, pic, (float)x + (float)w * .5f, (float)y + (float)h * .5f, (float)w, (float)h, 0);
        canvas_store(&c, blocks, sheets[SHEET_HUD].w);
        n++;
    }
    return n;
}

// The skill wheel's sheet: the letter of every disc is the picture of that face button, without its disc (the wheel's
// own is around it): a letter or a sign as tall as the game's letter; a key or the mouse as a small picture.
static int plan_draw_wheel(uint8_t *blocks, const Plan *p) {
    static Canvas c;
    int n = 0;
    for (int i = 0; i < N_WHEELS; i++) {
        int wx = i < WHEEL_ROW ? WHEEL_X + i * WHEEL_PITCH : WHEEL_X, wy = i < WHEEL_ROW ? WHEEL_Y : WHEEL_Y + WHEEL_PITCH;
        for (int k = 0; k < 4; k++) {
            const Pic *pic = &p->cell[lobes[k].cell];
            int cx = wx + lobes[k].dx, cy = wy + lobes[k].dy;
            if (!(wheels[i].lobes >> k & 1) || pic->shape == P_KEEP) continue;
            if (!canvas_over(&c, cx - WHEEL_ZONE / 2, cy - WHEEL_ZONE / 2, WHEEL_ZONE, WHEEL_ZONE, sheets[SHEET_WHEEL].w,
                             sheets[SHEET_WHEEL].h))
                continue;
            if (pic->shape == P_DISC || pic->shape == P_BOX) sign(&c, 0, pic, (float)cx, (float)cy, 23.8f);   // a capital 20 texels high
            else draw_pic(&c, pic, (float)cx, (float)cy, WHEEL_ZONE - 2, WHEEL_ZONE - 2, 0);
            canvas_store_colour(&c, blocks, sheets[SHEET_WHEEL].w, wheels[i].grey >> k & 1 ? wheel_grey : wheel_light);
            n++;
        }
    }
    return n;
}

// The other two sheets: as the skill wheel's, wheel by wheel at the places listed.
static int plan_draw_wheels(uint8_t *blocks, int sheet, const WheelAt *list, int count, const Plan *p) {
    static Canvas c;
    int n = 0;
    for (int i = 0; i < count; i++)
        for (int k = 0; k < 4; k++) {
            const Pic *pic = &p->cell[lobes[k].cell];
            int cx = list[i].x + lobes[k].dx, cy = list[i].y + lobes[k].dy;
            if (!(list[i].lobes >> k & 1) || pic->shape == P_KEEP) continue;
            if (!canvas_over(&c, cx - WHEEL_ZONE / 2, cy - WHEEL_ZONE / 2, WHEEL_ZONE, WHEEL_ZONE, sheets[sheet].w,
                             sheets[sheet].h))
                continue;
            if (pic->shape == P_DISC || pic->shape == P_BOX) sign(&c, 0, pic, (float)cx, (float)cy, 23.8f);
            else draw_pic(&c, pic, (float)cx, (float)cy, WHEEL_ZONE - 2, WHEEL_ZONE - 2, 0);
            canvas_store_colour(&c, blocks, sheets[sheet].w, list[i].ink);
            n++;
        }
    return n;
}

static int sheet_draw(int sheet, uint8_t *blocks, const Plan *p) {
    switch (sheet) {
    case SHEET_ATLAS: return plan_draw(blocks, ATLAS, p);
    case SHEET_HUD: return plan_draw_hud(blocks, p);
    case SHEET_WHEEL: return plan_draw_wheel(blocks, p);
    case SHEET_TRIO: return plan_draw_wheels(blocks, sheet, trio_wheels, sizeof trio_wheels / sizeof *trio_wheels, p);
    default: return plan_draw_wheels(blocks, sheet, wheel2_wheels, sizeof wheel2_wheels / sizeof *wheel2_wheels, p);
    }
}

// ---- the state in effect
static PlatMutex lock = PLAT_MUTEX_INIT;
static Plan plan;             // the pictures in effect
static Plan waiting;          // other pictures, as the last look wanted them
static int waiting_seen;
static uint32_t plan_gen = 1;
static uint64_t next_look_ns;
static unsigned sheets_seen;   // bit SHEET_*: that texture went to the GPU
enum { LOOK_NS = 250000000, LOGGED_CHANGES = 24 };

// Under lock: the pictures for the devices as they are. at_once: an upload takes them as they are now. Otherwise a
// change counts when two looks in a row want it: a mouse nudged while a gamepad is played is the device used last for
// one poll only, and the pictures should not flash to the keyboard's for it.
static void look(int at_once) {
    static const char *const whose[] = {"the game's own", "the keyboard and mouse", "an Xbox gamepad", "a PlayStation gamepad",
                                        "a Nintendo gamepad"};
    static int logged;
    InputPromptState in;
    Plan now;
    input_prompt_state(&in);
    plan_make(&now, &in, cfg_value("TOTK_BUTTON_PROMPTS"));
    if (!memcmp(&now, &plan, sizeof now)) {
        waiting_seen = 0;
        return;
    }
    if (!at_once && !(waiting_seen && !memcmp(&now, &waiting, sizeof now))) {
        waiting = now, waiting_seen = 1;
        return;
    }
    int n = 0;
    for (int i = 0; i < N_CELLS; i++) n += now.cell[i].shape != P_KEEP;
    for (int i = 0; i < N_DIAMONDS; i++) n += now.diamond_dots[i] || now.diamond_pic[i].shape != P_KEEP;
    for (int i = 0; i < 2; i++) n += now.stick[i].shape != P_KEEP;
    plan = now;
    waiting_seen = 0;
    plan_gen++;
    if (sheets_seen && logged < LOGGED_CHANGES)
        rt_log("prompts: the game's button pictures are for %s now, %d drawn again%s", whose[now.device], n,
               ++logged == LOGGED_CHANGES ? " (later changes are not logged)" : "");
}

uint32_t hud_prompts_gen(void) {
    plat_mutex_lock(&lock);
    uint64_t now = plat_now_ns();
    if (now >= next_look_ns) {
        next_look_ns = now + LOOK_NS;
        look(0);
    }
    uint32_t gen = plan_gen;
    plat_mutex_unlock(&lock);
    return gen;
}

// The first texture of this size that the game has pictures in, after `after`: SHEET_*, or -1.
static int sheet_sized(uint32_t w, uint32_t h, int after) {
    for (int s = after + 1; s < N_SHEETS; s++)
        if (w == sheets[s].w && h == sheets[s].h) return s;
    return -1;
}

int hud_prompts_patch(uint8_t *blocks, size_t size, uint32_t w, uint32_t h, uint32_t *gen) {
    uint8_t sum[32];
    int sheet = -1;
    if (size != (size_t)w * h) return 0;
    while ((sheet = sheet_sized(w, h, sheet)) >= 0) {   // two sheets share 2048 x 1024: the probe tells them apart
        uint64_t probe = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < PROBE_SIZE; i++) probe = (probe ^ blocks[sheets[sheet].probe_at + i]) * 0x100000001b3ull;
        if (probe == sheets[sheet].probe) break;
    }
    if (sheet < 0) return 0;
    sha256(blocks, size, sum);
    if (memcmp(sum, sheets[sheet].sha, sizeof sum)) return 0;
    plat_mutex_lock(&lock);
    if (!sheets_seen)
        rt_log("prompts: the game's button pictures follow the device in use (button_prompts = %s)", cfg_value("TOTK_BUTTON_PROMPTS"));
    if (!(sheets_seen >> sheet & 1)) rt_log("prompts: the game's %s went to the GPU", sheet_names[sheet]);
    sheets_seen |= 1u << sheet;
    look(1);
    next_look_ns = plat_now_ns() + LOOK_NS;
    sheet_draw(sheet, blocks, &plan);
    *gen = plan_gen;
    plat_mutex_unlock(&lock);
    return 1;
}

int hud_prompts_draw(uint8_t *blocks, uint32_t w, uint32_t h, const InputPromptState *in, const char *mode) {
    return hud_prompts_draw_sheet(sheet_sized(w, h, -1), blocks, in, mode);
}

int hud_prompts_draw_sheet(int sheet, uint8_t *blocks, const InputPromptState *in, const char *mode) {
    Plan p;
    if (sheet < 0 || sheet >= N_SHEETS) return 0;
    plan_make(&p, in, mode);
    plat_mutex_lock(&lock);
    int n = sheet_draw(sheet, blocks, &p);
    plat_mutex_unlock(&lock);
    return n;
}
