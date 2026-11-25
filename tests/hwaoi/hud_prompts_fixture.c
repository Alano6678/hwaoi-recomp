// Native fixture for runtime/hud_prompts.c: the calls it makes outside itself, a state of the devices built from the
// command line, and the texture it drew into as a PGM for tests/hwaoi/test_hud_prompts.py. No game data: the texture
// starts empty (or is a file of BC3 blocks a developer names).
//   hud_prompts_fixture <font.ttf> <mode> <out.pgm> [keyboard] [family=xbox|playstation|nintendo|generic]
//                       [layout=nintendo|xbox] [labels] [bind:<Switch button>=<pad input>]... [sheet=atlas|hud|wheel|trio|wheel2]
//                       [in=<blocks>] [out=<blocks>]
// sheet: which of the game's three textures to draw into (the default is the atlas). The PGM is the texture's alpha;
// for the skill wheel's sheet, whose pictures are colour in opaque blocks, the brightness of its colour.
// Prints "drawn <n>" and "patch <0|1>" (hud_prompts_patch on the same bytes before drawing).
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "hud_prompts.h"
#include "input_map.h"
#include "platform/platform.h"

static const char *mode = "auto";
static InputPromptState state;
static unsigned char *font_data;
static size_t font_size;

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
const char *cfg_value(const char *name) { (void)name; return mode; }
uint64_t plat_now_ns(void) { static uint64_t t; return t += 1000000000ull; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
uint32_t input_key_char(int key) { (void)key; return 0; }
void input_prompt_state(InputPromptState *out) { *out = state; }
const unsigned char *hud_prompts_font(size_t *size) { *size = font_size; return font_data; }

static const char *const actions[16] = {"A", "B", "X", "Y", "LCLICK", "RCLICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
                                        "LEFT", "UP", "RIGHT", "DOWN"};
static const char *const pads[PAD_COUNT] = {
    "none", "south", "east", "west", "north", "lstick", "rstick", "l", "r", "start", "back",
    "dpad_left", "dpad_up", "dpad_right", "dpad_down", "zl", "zr", "lpaddle1", "rpaddle1", "lpaddle2", "rpaddle2"};

static int find(const char *const *names, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], name)) return i;
    fprintf(stderr, "unknown name %s\n", name);
    exit(2);
}

// The first input of each Switch button in the runtime's default tables (input_map.c).
static void defaults(int layout) {
    static const uint8_t common[16] = {
        [B_STICK_L] = PAD_LSTICK, [B_STICK_R] = PAD_RSTICK, [B_L] = PAD_L, [B_R] = PAD_R, [B_ZL] = PAD_ZL, [B_ZR] = PAD_ZR,
        [B_PLUS] = PAD_START, [B_MINUS] = PAD_BACK, [B_LEFT] = PAD_DPAD_LEFT, [B_UP] = PAD_DPAD_UP,
        [B_RIGHT] = PAD_DPAD_RIGHT, [B_DOWN] = PAD_DPAD_DOWN};
    static const uint8_t keys[24] = {
        [B_A] = KEY_E, [B_B] = KEY_SPACE, [B_X] = KEY_MOUSE_R, [B_Y] = KEY_MOUSE_L, [B_STICK_L] = KEY_LCTRL,
        [B_STICK_R] = KEY_MOUSE_M, [B_L] = KEY_Q, [B_R] = KEY_R, [B_ZL] = KEY_LSHIFT, [B_ZR] = KEY_F, [B_PLUS] = KEY_TAB,
        [B_MINUS] = KEY_M, [B_LEFT] = KEY_1, [B_UP] = KEY_2, [B_RIGHT] = KEY_3, [B_DOWN] = KEY_4,
        [B_SL_LEFT] = KEY_A, [B_SL_UP] = KEY_W, [B_SL_RIGHT] = KEY_D, [B_SL_DOWN] = KEY_S,
        [B_SR_LEFT] = KEY_LEFT, [B_SR_UP] = KEY_UP, [B_SR_RIGHT] = KEY_RIGHT, [B_SR_DOWN] = KEY_DOWN};
    memcpy(state.pad, common, sizeof common);
    memcpy(state.key, keys, sizeof keys);
    int xbox = layout == PAD_LAYOUT_XBOX;
    state.pad[B_A] = xbox ? PAD_SOUTH : PAD_EAST, state.pad[B_B] = xbox ? PAD_EAST : PAD_SOUTH;
    state.pad[B_X] = xbox ? PAD_WEST : PAD_NORTH, state.pad[B_Y] = xbox ? PAD_NORTH : PAD_WEST;
}

static void alpha_of(const uint8_t *block, uint8_t out[16]) {
    unsigned a0 = block[0], a1 = block[1];
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)block[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) {
        unsigned k = (unsigned)(bits >> (3 * i)) & 7;
        out[i] = (uint8_t)(k == 0 ? a0 : k == 1 ? a1 : a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7
                         : k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : k == 6 ? 0 : 255);
    }
}

// The brightness of a block's colour texels (BC1 with four colours, as in a BC3 block): green, 0..255.
static void colour_of(const uint8_t *block, uint8_t out[16]) {
    unsigned c[4];
    for (int i = 0; i < 2; i++) c[i] = ((unsigned)(block[8 + 2 * i] | block[9 + 2 * i] << 8) >> 5 & 63) * 255 / 63;
    c[2] = (2 * c[0] + c[1]) / 3, c[3] = (c[0] + 2 * c[1]) / 3;
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)c[block[12 + i / 4] >> (2 * (i % 4)) & 3];
}

int main(int argc, char **argv) {
    int W = 1024, H = 1024, colour = 0, sheet = -1;   // sheet: hud_prompts_draw_sheet for the two sheets of the HUD's size
    if (argc < 4) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        font_size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        font_data = malloc(font_size);
        if (!font_data || fread(font_data, 1, font_size, f) != font_size) return 2;
        fclose(f);
    }
    mode = argv[2];
    const char *in = NULL, *out = NULL;
    int layout = -1, labels = 0;
    state.keyboard = 0, state.family = PAD_FAMILY_GENERIC;
    for (int i = 4; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "keyboard")) state.keyboard = 1;
        else if (!strcmp(a, "labels")) labels = 1;
        else if (!strncmp(a, "family=", 7)) {
            static const char *const families[] = {"generic", "xbox", "playstation", "nintendo"};
            state.family = find(families, 4, a + 7);
        } else if (!strncmp(a, "layout=", 7)) {
            layout = !strcmp(a + 7, "xbox") ? PAD_LAYOUT_XBOX : PAD_LAYOUT_NINTENDO;
            defaults(layout);
        } else if (!strncmp(a, "bind:", 5)) {
            char name[16];
            const char *eq = strchr(a + 5, '=');
            if (!eq || eq - (a + 5) >= (int)sizeof name) return 2;
            memcpy(name, a + 5, (size_t)(eq - (a + 5)));
            name[eq - (a + 5)] = 0;
            state.pad[find(actions, 16, name)] = (uint8_t)find(pads, PAD_COUNT, eq + 1);
        } else if (!strcmp(a, "sheet=atlas")) W = 1024, H = 1024, colour = 0;
        else if (!strcmp(a, "sheet=hud")) W = 2048, H = 1024, colour = 0;
        else if (!strcmp(a, "sheet=wheel")) W = 2048, H = 512, colour = 1;
        else if (!strcmp(a, "sheet=trio")) W = 2048, H = 1024, colour = 1, sheet = 3;
        else if (!strcmp(a, "sheet=wheel2")) W = 2048, H = 1024, colour = 1, sheet = 4;
        else if (!strncmp(a, "in=", 3)) in = a + 3;
        else if (!strncmp(a, "out=", 4)) out = a + 4;
        else return 2;
    }
    if (layout < 0 && state.keyboard) defaults(PAD_LAYOUT_XBOX);   // the keys; no gamepad
    state.layout = layout;
    if (labels) {   // as SDL reports them for a pad of the family
        static const int printed[4][4] = {
            {0, 0, 0, 0}, {PAD_LABEL_A, PAD_LABEL_B, PAD_LABEL_X, PAD_LABEL_Y},
            {PAD_LABEL_CROSS, PAD_LABEL_CIRCLE, PAD_LABEL_SQUARE, PAD_LABEL_TRIANGLE},
            {PAD_LABEL_B, PAD_LABEL_A, PAD_LABEL_Y, PAD_LABEL_X}};
        memcpy(state.labels, printed[state.family], sizeof state.labels);
    }
    size_t bytes = (size_t)W * (size_t)H;   // BC3: a byte a texel
    uint8_t *blocks = calloc(1, bytes), *before = malloc(bytes);
    if (!blocks || !before) return 2;
    if (in) {
        f = fopen(in, "rb");
        if (!f || fread(blocks, 1, bytes, f) != bytes) return 2;
        fclose(f);
    }
    memcpy(before, blocks, bytes);
    uint32_t gen = 0;
    int patched = hud_prompts_patch(blocks, bytes, (uint32_t)W, (uint32_t)H, &gen);
    if (!patched && memcmp(before, blocks, bytes)) return 3;   // not the game's texture: not a byte may change
    int drawn = patched ? -1 : sheet >= 0 ? hud_prompts_draw_sheet(sheet, blocks, &state, mode)
                                          : hud_prompts_draw(blocks, (uint32_t)W, (uint32_t)H, &state, mode);
    printf("patch %d\ndrawn %d\n", patched, drawn);
    if (out) {
        f = fopen(out, "wb");
        if (!f || fwrite(blocks, 1, bytes, f) != bytes) return 2;
        fclose(f);
    }
    uint8_t *seen = malloc(bytes);
    if (!seen) return 2;
    for (int by = 0; by < H / 4; by++)
        for (int bx = 0; bx < W / 4; bx++) {
            uint8_t a[16];
            const uint8_t *block = blocks + ((size_t)by * (size_t)(W / 4) + (size_t)bx) * 16;
            if (colour) colour_of(block, a);
            else alpha_of(block, a);
            for (int i = 0; i < 16; i++) seen[(size_t)(by * 4 + i / 4) * (size_t)W + (size_t)(bx * 4 + i % 4)] = a[i];
        }
    f = fopen(argv[3], "wb");
    if (!f) return 2;
    fprintf(f, "P5\n%d %d\n255\n", W, H);
    fwrite(seen, 1, bytes, f);
    fclose(f);
    return 0;
}
