// Host input sampling (runtime/input.c, with input_win.c or input_sdl.c): SDL3 gamepad and the keyboard as one Switch npad.
#pragma once
#include <stdint.h>

// Bits of libnx HidNpadButton (A=0 B=1 X=2 Y=3 StickL=4 StickR=5 L=6 R=7 ZL=8 ZR=9 Plus=10 Minus=11 Left=12 Up=13 Right=14
// Down=15) and the stick directions (StickLLeft..Down 16-19, StickRLeft..Down 20-23, derived from the sticks).
// Sticks are -32767..32767, up is +y. TOTK_INPUT=none reports no input. Callable from any thread.
void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
// Players (co-op): each player's controller reads only the devices assigned to it (TOTK_PLAYER<n>_DEVICE: auto, keyboard,
// a gamepad, none), so one player's input never reaches another. input_poll is player 1. Returns 1 when the player has
// a device (player 1 always). Blocked like input_poll while a text entry or the menu is open.
enum { INPUT_PLAYERS = 2 };
int input_poll_player(int player, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
int input_player_connected(int player);
// The connected gamepads as player device choices for the menu: the TOTK_PLAYER<n>_DEVICE value and a display name.
typedef struct { char value[64]; char name[80]; } InputDeviceChoice;
int input_device_choices(InputDeviceChoice *out, int max);
// The same for host UI (the text entry): never blocked. input_block(1) while a text entry is open: input_poll then
// reports nothing; input_block(0) when it closes: nothing until all buttons and stick directions are released.
void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
void input_block(int on);
// The in-game menu (ui/menu.h): input_menu(1) when it opens (input_poll reports nothing, the cursor is released), (0) when
// it closes (nothing until every button and stick direction is released). Independent of input_block.
void input_menu(int open);
// The gamepad alone, for the menu: Switch button bits and the left stick (up +y). 0 without a connected gamepad.
// Minus+Plus combo and the neutral pad while the menu is open (input_map.c, window-system free): *held is the caller's memory
// of the combo, sticks are lx, ly, rx, ry. Returns 1 in the poll where the combo goes down (toggle the menu once).
int input_menu_gate(int *held, int menu_open, uint64_t *buttons, int32_t sticks[4]);
int input_overlay_gate(int *held, uint64_t *buttons);   // L3+R3: 1 in the poll both go down
int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly);
// The gamepad layout in effect (PAD_LAYOUT_*, input_map.h; TOTK_PAD_LAYOUT, live); -1 without a gamepad. Any thread.
int input_pad_layout(void);
// The label on a face button of the open gamepad (input_map.h PAD_SOUTH..PAD_NORTH, as SDL reports it); PAD_LABEL_NONE for another
// button, an unknown label or no gamepad. Any thread.
enum { PAD_LABEL_NONE, PAD_LABEL_A, PAD_LABEL_B, PAD_LABEL_X, PAD_LABEL_Y, PAD_LABEL_CROSS, PAD_LABEL_CIRCLE,
       PAD_LABEL_SQUARE, PAD_LABEL_TRIANGLE };
int input_pad_label(int pad);
int input_pad_type(void); // SDL type of the gamepad the menu shows (the one used last, else player 1's); 0 without one.
// The connected devices, for the menu's Controls page: what each gamepad is and has, as SDL reports it, and whose
// it is. Nothing here is guessed: a field the pad does not report is 0 or -1.
enum { PAD_FAMILY_GENERIC, PAD_FAMILY_XBOX, PAD_FAMILY_PLAYSTATION, PAD_FAMILY_NINTENDO };
typedef struct {
    char name[80];             // SDL's name; "#2" after it when two of one model are connected
    char value[64];            // the TOTK_PLAYER<n>_DEVICE value that names it
    int type;                  // SDL_GamepadType
    int family;                // PAD_FAMILY_*
    int switch2;               // a Switch 2 controller (Pro Controller 2, Joy-Con 2, the NSO GameCube pad)
    int player;                // the player it feeds (0, 1), -1 none
    int wired;                 // 1 cable, 0 wireless, -1 not reported
    int battery;               // percent, -1 not reported
    int rumble, trigger_rumble, motion, light, back_buttons;   // what it has
    int layout;                // PAD_LAYOUT_* in effect for it
    int labels[4];             // its south, east, west, north labels (PAD_LABEL_*)
} InputPadInfo;
int input_pads(InputPadInfo *out, int max);   // in connection order; returns how many
// The gamepad that feeds player (its first one); 0 when the player has none.
int input_player_pad(int player, InputPadInfo *out);
int input_keyboard_player(void);              // the player the keyboard and mouse feed, -1 none
// A short rumble on gamepad index of input_pads, to tell which one it is; 0 when it has no motors.
int input_pad_identify(int index);
// The menu's rumble rows: a short pulse on every connected gamepad that rumbles, through the rumble strength, curve
// and trigger settings as they are now, so a change is felt at once. 1 if one rumbled.
int input_rumble_preview(void);
// The device used last: 1 while it is the keyboard or the mouse (also with gamepads connected), 0 a gamepad.
// input_note_keyboard: the window saw a key or the mouse (the menu's own events, which never pass the game's poll).
int input_keyboard_active(void);
void input_note_keyboard(void);
// What the game's own button pictures should show (hud_prompts.c): the device used last, as the menu's hints follow
// it, and for each Switch button (input_map.h B_A..B_DOWN, the bit numbers) the first input bound to it; the keys also
// for the stick directions (B_SL_LEFT..B_SR_DOWN). Any thread; before the input has started it answers as with the
// keyboard alone.
typedef struct {
    int keyboard;             // the keyboard or the mouse was used after every gamepad, or no gamepad is connected
    int family;               // PAD_FAMILY_* of the gamepad used last, else player 1's
    int layout;               // its PAD_LAYOUT_* in effect, -1 without a gamepad
    int labels[4];            // its south, east, west, north labels (PAD_LABEL_*), PAD_LABEL_NONE when not known
    uint8_t pad[16], key[24]; // input_map.h PAD_* and KEY_* ids; 0 (PAD_NONE, KEY_NONE): nothing bound
} InputPromptState;
void input_prompt_state(InputPromptState *out);
// 1 while the game's menus confirm with Switch B, the bottom button, and go back with Switch A, the right one (the
// generated patch set hwaoi_confirm, which ov_input.c switches at every controller read; battle keeps every button).
// TOTK_CONFIRM_BUTTON: auto with a gamepad of another maker than Nintendo in use (the one the pictures follow), bottom
// with any gamepad in use, right never (the game's own way). The keyboard and the mouse keep the game's keys. Any thread.
int input_confirm_bottom(void);
// The menu's remapping rows (Controls tab). input_map_snapshot copies the tables in effect (after any changed TOTK_BIND_* line).
// input_capture polls the keys, the mouse buttons and the gamepad for a press: reset 1 takes what is held now as the
// start (a held key counts once released and pressed again) and returns NONE; then it returns KEY or PAD with *id the
// key or pad id (input_map.h) pressed since the last call, or CANCEL for Esc. Keys count only while the game window is in front.
enum { INPUT_CAPTURE_NONE, INPUT_CAPTURE_KEY, INPUT_CAPTURE_PAD, INPUT_CAPTURE_CANCEL };
struct InputMap;   // input_map.h (not included here: swkbd_win.c has button names of its own)
void input_map_snapshot(struct InputMap *m);
void input_map_snapshot_player(int player, struct InputMap *m);   // that player's tables (co-op)
int input_capture(int reset, int *id);
// The character on a punctuation key (KEY_MINUS..KEY_GRAVE) in the keyboard layout in use, 0 for another key.
uint32_t input_key_char(int key);
// F8, the flight recorder's key (no KEY_ id), held while the game window is in front.
int input_flight_key(void);

// Mouse camera and cursor (input_win.c, Win32 window thread). The window procedure calls input_window_msg first for raw
// mouse input, cursor hiding and confinement; it returns 1 with *res set when the message is handled. input_cursor_suspend(1)
// releases the cursor around an F11 window change, (0) captures it again once the client area has its new size.
void input_cursor_suspend(int on);
int input_cursor_released(void);   // Esc or a non-client activation released the cursor (probe)

// Rumble: SDL_RumbleGamepad on the open gamepad, low (heavy, left) and high (light, right) motor 0..65535 for ms
// milliseconds. 1 if a gamepad with rumble took it. Callable from any thread.
int input_rumble(uint16_t low, uint16_t high, uint32_t ms);
int input_rumble_player(int player, uint16_t low, uint16_t high, uint32_t ms);   // that player's gamepads
// Motor value for a normalised amplitude: pow(a, curve) * strength, saturated to 0..65535. Pure.
uint16_t rumble_motor(float a, float curve, float strength);
// TOTK_RUMBLE_TRIGGERS=1: input_rumble also drives the impulse triggers (Xbox) with the high motor's value.
int input_rumble_triggers_enabled(void);

// Motion (TOTK_MOTION=0 turns it off, live). input_motion_start starts the sensor thread once: every 4 ms it reads the
// gamepad's gyro and accelerometer (when it has them) and feeds motion_global(). Callable from any thread.
void input_motion_start(void);
// TOTK_MOTION is not "0" (a live setting: read again when cfg_version changes). Off: the sensor reads at rest.
int input_motion_enabled(void);
// SDL sensor values (accelerometer m/s^2, gyro rad/s, SDL gamepad axes) to the Switch controller frame motion_feed
// takes (acceleration in G as gravity reads, rotation rad/s). The mapping is documented in input_win.c.
void input_motion_axes(const float sdl_acc[3], const float sdl_gyro[3], double acc_g[3], double gyro[3]);

// motion_fusion.c: nn::hid::SixAxisSensorState and the sensor fusion that produces it.
typedef struct {
    int64_t delta_ns, sampling;
    float acc[3], gyro[3], angle[3], dir[9];   // G, revolutions/s, revolutions, controller axes in the reference frame
    uint32_t attributes, reserved;             // bit 0 connected
} MotionState;
_Static_assert(sizeof(MotionState) == 0x60, "SixAxisSensorState ABI");
typedef struct MotionFusion MotionFusion;
MotionFusion *motion_new(void);
MotionFusion *motion_global(void);   // the gamepad's, fed by input_win.c
MotionFusion *input_motion_player(int player);   // player 1: motion_global(); fed from that player's gamepad
void motion_feed(MotionFusion *m, const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);
int motion_read(MotionFusion *m, MotionState *out, int max, uint64_t now_ns);   // newest first; 0 = none in 200 ms
void motion_restart(MotionFusion *m);   // angle and direction start over at the next sample
