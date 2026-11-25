// Settings pages (ui/option_rows.cpp): cards of option rows and a detail strip for the selected or hovered row.
// Each row shows a cfg setting and changes it through cfg_set. A or a click steps it (toggle, next value), left and
// right step it. A row says when it applies after a restart and is greyed out with a note when an environment variable
// fixes it; it stays focusable so its description shows. A segment or slider track can be clicked directly.
//
//   opt_begin("##screen", scale);
//   opt_toggle("TOTK_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
//   opt_end();
//
// key: the setting's TOTK_ name, which must have a cfg_meta row. Present path only (ImGui).
#pragma once
#include "imgui.h"

extern "C" {
#include "ui_text.h"
}

void opt_begin(const char *id, float scale);
void opt_end(void);
// The height the next page leaves free for its detail strip and the action bar.
void opt_reserve_bottom(float h);
// The next page's first row takes the controller's cursor (opening the menu, switching tabs).
void opt_focus_first(void);
// The row of key (or button id) takes the cursor, scrolled into view, when it is next drawn.
void opt_focus_key(const char *key);
// The next row is greyed out like one the environment fixes, with note under its label and in its description.
void opt_disable_next(UiTextId note);
// The next row's description gets this line after it (which connected devices the setting is for); the string must
// stay valid until that row is drawn.
void opt_describe_next(const char *more);

// Page text that is not a row: a group heading above a card or a wrapped paragraph (warn: amber).
void opt_group(UiTextId title);
void opt_text(UiTextId text, bool warn = false);

// The return value of every row: true when it changed the setting this frame.
// inverted: the row shows "on" while the key is 0 (a setting whose key names the opposite). note: as opt_choice's.
bool opt_toggle(const char *key, UiTextId label, UiTextId desc, bool inverted = false, const char *note = nullptr);
// One name per value of the key's cfg_meta values list, in its order. When the key has no fixed default (an empty
// def), names[0] is the automatic choice (the key unset) and names[1..] follow the values. note: a short line under
// the value in place of the "default" mark (NULL: none). cur: the choice in effect when the key may hold a value the
// row does not show (an older name of one of its choices); -1: the key's value is looked up among the n shown.
bool opt_choice(const char *key, UiTextId label, const UiTextId *names, int n, UiTextId desc,
                const char *note = nullptr, int cur = -1);
// A choice over a list the caller builds (devices, window sizes): values[i] is written, names[i] shown; "" means the key
// unset. cur: the index in effect, -1 to find cfg_value(key) in values (no match: the value shown as it is).
bool opt_choice_list(const char *key, UiTextId label, const char *const *values, const char *const *names, int n,
                     UiTextId desc, int cur = -1, const char *note = nullptr);
// A choice mapped to several keys by the caller (cur: the index shown, def: the one marked default). Returns the index
// A / click / left / right picked (the caller writes the keys, e.g. with cfg_set and opt_save_soon), else -1. skip: an
// index that cannot be picked (unavailable). restart: the row shows "after restart".
int opt_select(const char *key, UiTextId label, const UiTextId *names, int n, int cur, UiTextId desc, int def, int skip,
               bool restart, const char *note = nullptr);
// A row that runs an action: true on A or a click. id: unique on the page and not a setting ("##copy_info"); value:
// shown on the right ("" none).
bool opt_button(const char *id, UiTextId label, UiTextId desc, const char *value = "");
// The same with texts of the caller's own (a connected device: its name, what it is, what it has): label on the
// left, value on the right, desc in the detail strip. true on A or a click.
bool opt_info(const char *id, const char *label, const char *value, const char *desc);
// lo..hi in steps of step (left and right held for a second go ten times as fast); fmt formats the value ("%.0f %%").
bool opt_slider(const char *key, UiTextId label, float lo, float hi, UiTextId desc, float step = 1,
                const char *fmt = "%g");

// Binding rows (remapping on the Controls tab): a label and three cells, each selectable on its own (left / right move between them).
// opt_bind_header: the column titles above them. opt_bind_row: key is the setting's name without TOTK_ (BIND_A; the
// description names it when the environment sets it); a disabled cell shows its text greyed and takes no cursor; note
// (NULL none) follows the description. Returns the cell A / Enter / a click activated (0..2), 10 + the cell Delete,
// Backspace or X cleared, or -1. opt_focus_key("<key>#<cell>") puts the cursor on a cell.
void opt_bind_header(UiTextId c0, UiTextId c1, UiTextId c2);
int opt_bind_row(const char *key, const char *label, UiTextId desc, const char *const cells[3], const bool enabled[3],
                 const char *note = nullptr);

// totk.ini is written a moment after the last change (a held slider would rewrite it every frame) and when the menu
// closes. opt_save_tick: on every frame the menu lays out; force: write now if anything is pending. 1 while a write is
// pending.
int opt_save_tick(int force);
// A change made outside a row (Reset, F10): totk.ini is written as after a row's change.
void opt_save_soon(void);
// 0 none, 1 saved recently, 2 restart needed, 3 save failed.
int opt_status(void);
// A setting read only at start (CfgMeta live 0) holds another value than it had when the game started. Back to that
// value, no restart is needed any more.
bool opt_restart_needed(void);
