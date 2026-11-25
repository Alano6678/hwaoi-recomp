// Settings pages: cards group rows of settings with toggle, choice, slider and button controls. A detail strip below
// the rows shows the description and state of the selected or hovered row.
//
// The left / right stepping, hold speed-up and selected-row description follow SavageWindRecomp's
// runtime/ui/option_rows.mm (https://github.com/Revivedx/SavageWindRecomp @
// 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai). The row drawing is Age of Decomp's own.
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
// SetKeyOwner, SetFocusID, NavCursorVisible
#include "imgui_internal.h"
#include "ui/option_rows.h"
#include "ui/theme.h"

extern "C" {
#include "cfg.h"
#include "platform/platform.h"
// host.h pulls in cpu.h, which is C only: the one function needed from it, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

// g_bottom: opt_reserve_bottom, kept free for the detail strip and action bar.
static float g_scale = 1, g_bottom;
// opt_focus_first: requested / this page's first row takes it; g_held: left / right held a while (steppers go faster).
// opt_disable_next: the next row is greyed out with g_disable_note.
static bool g_focus_req, g_take_first, g_held, g_disable_next;
// opt_focus_key: the row that takes the cursor when it is next drawn.
static char g_focus_key[64];
static UiTextId g_disable_note;
static ImDrawListSplitter g_channels;
static bool g_card;
static ImVec2 g_card_start;
static float g_card_width, g_card_last_y, g_page_x, g_page_width;

// The detail strip: this frame's hovered and selected rows, the page's first row, and the last one shown.
struct Desc {
    char text[UI_DESC_BYTES], state[256], def[256];
    bool warning;
};
static Desc g_hover, g_focus, g_first, g_shown;
static bool g_restart_next, g_restart;   // opt_select: the row applies after restart whatever its keys say
// opt_info: the next row's label and description are these strings, not catalogue texts (a device's own name)
static const char *g_label_text, *g_desc_text;
static const char *g_desc_more;   // opt_describe_next: a line after the next row's description

// disabled: the note of a row opt_disable_next greyed out, else NULL. g_restart: the row applies after a restart
// whatever its key's own record says (opt_select).
static void desc_set(Desc &d, UiTextId desc, const char *key, const CfgMeta *m, const char *disabled,
                     const char *def = nullptr, const char *note = nullptr) {
    snprintf(d.text, sizeof d.text, "%s", g_desc_text ? g_desc_text : menu_text(desc));
    if (g_desc_more && *g_desc_more) {
        size_t n = strlen(d.text);
        snprintf(d.text + n, sizeof d.text - n, "\n%s", g_desc_more);
    }
    // e.g. what moved here after a binding change, after the description
    if (note && *note) {
        size_t n = strlen(d.text);
        snprintf(d.text + n, sizeof d.text - n, "\n%s", note);
    }
    d.state[0] = d.def[0] = 0;
    char env[96];
    bool env_fixed = cfg_env_name(key, env, sizeof env);
    bool restart = (m && !m->live) || g_restart;
    d.warning = disabled || env_fixed || restart;
    if (disabled) snprintf(d.state, sizeof d.state, "%s", disabled);
    else if (env_fixed) snprintf(d.state, sizeof d.state, "%s %s", menu_text(UI_MENU_SET_BY_ENV), env);
    else if (m || restart)
        snprintf(d.state, sizeof d.state, "%s", menu_text(restart ? UI_MENU_APPLIES_RESTART : UI_MENU_APPLIES_NOW));
    if (def && *def) snprintf(d.def, sizeof d.def, menu_text(UI_MENU_DEFAULT_IS), def);
}

static void card_end() {
    if (!g_card) return;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 end(g_card_start.x + g_card_width, g_card_last_y);
    g_channels.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(g_card_start, end, theme::surface2, 14 * g_scale);
    dl->AddRect(g_card_start, end, theme::line, 14 * g_scale);
    g_channels.SetCurrentChannel(dl, 1);
    g_card = false;
}

static void card_row(ImVec2 p, float w, float h) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (!g_card) {
        g_card = true;
        g_card_start = p;
        g_card_width = w;
    } else {
        dl->AddLine(ImVec2(p.x + 16 * g_scale, p.y), ImVec2(p.x + w - 16 * g_scale, p.y), theme::line);
    }
    g_card_last_y = p.y + h;
}

void opt_focus_first(void) { g_focus_req = true; }
void opt_focus_key(const char *key) { snprintf(g_focus_key, sizeof g_focus_key, "%s", key); }
void opt_reserve_bottom(float h) { g_bottom = h; }
void opt_disable_next(UiTextId note) { g_disable_next = true; g_disable_note = note; }
void opt_describe_next(const char *more) { g_desc_more = more; }

void opt_begin(const char *id, float scale) {
    g_scale = scale;
    g_take_first = g_focus_req;
    g_focus_req = false;
    // This frame's move goes to the first row instead.
    if (g_take_first) ImGui::NavMoveRequestCancel();
    g_hover = {}; g_focus = {}; g_first = {};
    g_page_x = ImGui::GetCursorScreenPos().x;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    g_page_width = avail.x;
    avail.y = fmaxf(1, avail.y - g_bottom);
    ImGui::PushID(id);
    // Rows must be a direct child of the menu so D-pad navigation reaches the action bar without landing on clipped rows.
    ImGui::BeginChild("##rows", avail, ImGuiChildFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    g_channels.Split(dl, 2);
    g_channels.SetCurrentChannel(dl, 1);
    g_card = false;
}

static void description_lines(ImDrawList *dl, ImVec2 p, float w, const char *text) {
    ImFont *font = theme::fonts().small;
    float fs = font->FontSize;
    const char *end = text + strlen(text), *s = text;
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colour(theme::text));
    for (int line = 0; line < 3 && s < end; line++) {
        const char *br = strchr(s, '\n');
        const char *limit = br ? br : end;
        const char *next = font->CalcWordWrapPositionA(1.0f, s, limit, w);
        if (next <= s) next = limit;
        bool more = next < end;
        if (line == 2 && more) {
            ImGui::RenderTextEllipsis(dl, p, ImVec2(p.x + w, p.y + fs), p.x + w, p.x + w, s, end, nullptr);
            break;
        }
        dl->AddText(font, fs, p, theme::text, s, next);
        s = next;
        while (s < end && (*s == ' ' || *s == '\n')) s++;
        p.y += fs * 1.13f;
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void opt_end(void) {
    card_end();
    g_channels.Merge(ImGui::GetWindowDrawList());
    ImGui::PopStyleVar();
    ImGui::EndChild();
    // The hovered row (mouse) wins, else the selected one (controller), else the last shown, else the first row.
    if (g_hover.text[0]) g_shown = g_hover;
    else if (g_focus.text[0]) g_shown = g_focus;
    else if (!g_shown.text[0]) g_shown = g_first;
    ImVec2 p(g_page_x, ImGui::GetItemRectMax().y + 8 * g_scale);
    float w = g_page_width;
    ImGui::SetCursorScreenPos(p);
    float h = 3 * theme::fonts().small->FontSize * 1.13f + 16 * g_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::surface2, 6 * g_scale);
    dl->AddRectFilled(p, ImVec2(p.x + 3 * g_scale, p.y + h), theme::focus);
    ImGui::PushFont(theme::fonts().small);
    float fs = ImGui::GetFontSize();
    ImVec2 state_end(p.x + w - 12 * g_scale, p.y + 8 * g_scale);
    float sw = ImGui::CalcTextSize(g_shown.state).x;
    float dw = ImGui::CalcTextSize(g_shown.def).x;
    float right = fminf(w * .45f, fmaxf(g_shown.state[0] ? sw + 16 * g_scale : 0, dw));
    description_lines(dl, ImVec2(p.x + 14 * g_scale, p.y + 8 * g_scale), w - right - 30 * g_scale, g_shown.text);
    int state_lines = 0;
    if (g_shown.state[0]) {
        const char *s = g_shown.state, *end = s + strlen(s);
        float available = fmaxf(1, right - 16 * g_scale);
        while (s < end && state_lines < 2) {
            const char *next = state_lines == 0 ? ImGui::GetFont()->CalcWordWrapPositionA(1.0f, s, end, available) : end;
            if (next <= s) {
                unsigned c;
                next = s + ImTextCharFromUtf8(&c, s, end);
                if (next <= s) break;
            }
            const char *line_end = next;
            while (line_end > s && line_end[-1] == ' ') line_end--;
            float width = ImGui::CalcTextSize(s, line_end).x;
            float y = state_end.y + state_lines * fs * 1.13f;
            float x = state_end.x - width;
            if (!state_lines)
                dl->AddCircleFilled(ImVec2(x - 9 * g_scale, y + fs * .5f), 3 * g_scale,
                                    g_shown.warning ? theme::warn : theme::ok);
            dl->PushClipRect(ImVec2(state_end.x - available, y), ImVec2(state_end.x, y + fs * 1.13f), true);
            dl->AddText(ImVec2(x, y), g_shown.warning ? theme::warn : theme::text, s, line_end);
            dl->PopClipRect();
            s = next;
            while (s < end && (*s == ' ' || *s == '\n')) s++;
            state_lines++;
        }
    }
    if (g_shown.def[0]) {
        float x = fmaxf(state_end.x - right, state_end.x - dw);
        float y = state_end.y + state_lines * fs * 1.13f;
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colour(theme::text_faint));
        ImGui::RenderTextEllipsis(dl, ImVec2(x, y), ImVec2(state_end.x, y + fs),
                                  state_end.x, state_end.x,
                                  g_shown.def, nullptr, nullptr);
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
}

enum RowKind { ROW_TOGGLE, ROW_SEGMENTS, ROW_CYCLE, ROW_SLIDER, ROW_BUTTON };
struct RowView {
    RowKind kind;
    const char *value, *def, *note;
    bool is_default;
    const char *const *names;
    int n, current;
    float fraction;
};
static float g_drag_fraction;

static void focus_ring(ImDrawList *dl, ImVec2 a, ImVec2 b, float radius) {
    dl->AddRect(a, b, theme::focus_soft, radius, 0, 6 * g_scale);
    dl->AddRect(a, b, theme::focus, radius, 0, 2 * g_scale);
}

static void clipped_text(ImDrawList *dl, ImVec2 a, ImVec2 b, const char *text, ImU32 colour) {
    if (b.x <= a.x) return;
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colour(colour));
    ImGui::RenderTextEllipsis(dl, a, b, b.x, b.x, text, nullptr, nullptr);
    ImGui::PopStyleColor();
}

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right),
// 100 + i (segment i), 3 (slider track click / drag), or 0; always 0 for a row the environment fixes (greyed out,
// still selectable so its description shows).
static int row(const char *key, UiTextId label, UiTextId desc, const RowView &v) {
    const CfgMeta *m = cfg_meta(key);
    const char *disabled = g_disable_next ? menu_text(g_disable_note) : nullptr;
    g_disable_next = false;
    g_restart = g_restart_next;
    g_restart_next = false;
    struct MoreOnce { ~MoreOnce() { g_desc_more = nullptr; } } more_once;   // for this row only
    bool off = disabled || cfg_from_env(key);
    ImGui::PushID(key);
    ImVec2 p = ImGui::GetCursorScreenPos();
    // h whole pixels: ImGui truncates cursor positions, which rounds a fractional height the other way above the window's
    // top (negative y), so the page grew a pixel per row scrolled out and a row scrolled to the bottom was cut off.
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = floorf(fs * 2.4f);
    card_row(p, w, h);
    // The row's own border shows the selection.
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool to_key = g_focus_key[0] && !strcmp(key, g_focus_key);
    if (g_take_first || to_key) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        // The page's top: text and a heading above the first row show.
        if (g_take_first) ImGui::SetScrollY(0);
        // As a cursor move would, keeping the last row whole.
        else ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
        g_take_first = false;
        if (to_key) g_focus_key[0] = 0;
    }
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    int step = clicked && !off ? 2 : 0;
    g_held = false;
    if (foc && !off) {
        // The left stick arrives as the D-pad (menu.cpp pad_events).
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step = 1;
        static const ImGuiKey keys[] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow,
                                        ImGuiKey_RightArrow};
        for (ImGuiKey k : keys) {
            if (ImGui::GetKeyData(k)->DownDuration > 1.0f) g_held = true;
            // Left and right belong to the row; navigation would otherwise move sideways instead of changing its value.
            ImGui::SetKeyOwner(k, ImGui::GetItemID());
        }
    }
    if (foc) desc_set(g_focus, desc, key, m, disabled, v.def);
    if (hov) desc_set(g_hover, desc, key, m, disabled, v.def);
    if (!g_first.text[0]) desc_set(g_first, desc, key, m, disabled, v.def);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hov || foc) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::surface3, 10 * g_scale);
    if (foc) focus_ring(dl, ImVec2(p.x + 2 * g_scale, p.y + 2 * g_scale),
                        ImVec2(p.x + w - 2 * g_scale, p.y + h - 2 * g_scale), 8 * g_scale);
    ImU32 fg = off ? theme::alpha(theme::text, .45f) : theme::text;
    ImU32 dim = off ? theme::alpha(theme::text_dim, .45f) : theme::text_dim;
    float pad = fs * .8f, right = p.x + w - pad, cy = p.y + h * .5f;
    float control_left = right;
    float box_h = fs * 1.55f, top = cy - box_h * .5f;
    bool mouse_click = clicked && ImGui::IsMouseReleased(0);
    auto control = [off](ImU32 c) { return off ? theme::alpha(c, .45f) : c; };
    float widths[4] = {}, total = fs * .32f;
    RowKind kind = v.kind;
    if (kind == ROW_SEGMENTS) {
        for (int i = 0; i < v.n; i++) { widths[i] = ImGui::CalcTextSize(v.names[i]).x + fs * 1.1f; total += widths[i]; }
        if (total > w * .55f) kind = ROW_CYCLE;
    }

    if (kind == ROW_TOGGLE) {
        float sw = fs * 2.1f, sh = fs * 1.15f;
        control_left = right - sw;
        bool on = !strcmp(v.value, "1");
        ImVec2 a(control_left, cy - sh * .5f), b(right, cy + sh * .5f);
        dl->AddRectFilled(a, b, control(on ? theme::focus : theme::surface1), sh * .5f);
        dl->AddRect(a, b, control(theme::line_strong), sh * .5f);
        dl->AddCircleFilled(ImVec2(on ? right - sh * .5f : control_left + sh * .5f, cy), sh * .35f,
                            on ? control(theme::knob_on) : fg);
    } else if (kind == ROW_SEGMENTS) {
        control_left = right - total;
        dl->AddRectFilled(ImVec2(control_left, top), ImVec2(right, top + box_h), control(theme::surface1), 10 * g_scale);
        dl->AddRect(ImVec2(control_left, top), ImVec2(right, top + box_h), control(theme::line), 10 * g_scale);
        float x = control_left + fs * .16f;
        for (int i = 0; i < v.n; i++) {
            ImVec2 a(x, top + fs * .16f), b(x + widths[i], top + box_h - fs * .16f);
            bool hot = ImGui::IsMouseHoveringRect(a, b) && hov;
            if (i == v.current || hot)
                dl->AddRectFilled(a, b, control(i == v.current ? theme::surface4 : theme::surface3), 6 * g_scale);
            if (i == v.current) dl->AddRect(a, b, control(theme::focus), 6 * g_scale);
            float tw = ImGui::CalcTextSize(v.names[i]).x;
            dl->AddText(ImVec2(x + (widths[i] - tw) * .5f, cy - fs * .5f), i == v.current ? fg : dim, v.names[i]);
            if (mouse_click && !off && hot) step = 100 + i;
            x += widths[i];
        }
    } else if (kind == ROW_CYCLE) {
        float width = fminf(w * .55f, fmaxf(fs * 11, ImGui::CalcTextSize(v.value).x + fs * 4));
        control_left = right - width;
        dl->AddRectFilled(ImVec2(control_left, top), ImVec2(right, top + box_h), control(theme::surface1), 10 * g_scale);
        dl->AddRect(ImVec2(control_left, top), ImVec2(right, top + box_h), control(theme::line), 10 * g_scale);
        float side = fs * 1.3f, inset = (box_h - side) * .5f;
        for (int i = 0; i < 2; i++) {
            float x = i ? right - side - inset : control_left + inset;
            ImVec2 a(x, top + inset), b(x + side, top + inset + side);
            bool hot = hov && ImGui::IsMouseHoveringRect(a, b);
            dl->AddRectFilled(a, b, control(hot ? theme::surface4 : theme::surface3), 6 * g_scale);
            float cx = x + side * .5f, dx = fs * .17f, dy = fs * .25f;
            dl->AddLine(ImVec2(cx + (i ? -dx : dx), cy - dy), ImVec2(cx + (i ? dx : -dx), cy), fg, 2 * g_scale);
            dl->AddLine(ImVec2(cx + (i ? dx : -dx), cy), ImVec2(cx + (i ? -dx : dx), cy + dy), fg, 2 * g_scale);
            if (mouse_click && !off && hot) step = i ? 1 : -1;
        }
        float tx0 = control_left + side + inset * 2, tx1 = right - side - inset * 2;
        float tw = ImGui::CalcTextSize(v.value).x;
        clipped_text(dl, ImVec2(fmaxf(tx0, (tx0 + tx1 - tw) * .5f), cy - fs * .5f),
                     ImVec2(tx1, cy + fs * .5f), v.value, fg);
    } else if (kind == ROW_SLIDER) {
        float value_w = fmaxf(fs * 3.2f, ImGui::CalcTextSize(v.value).x);
        float track_w = fs * 9, gap = fs * .7f;
        control_left = right - value_w - gap - track_w;
        float track_right = control_left + track_w, track_h = fs * .25f;
        dl->AddRectFilled(ImVec2(control_left, cy - track_h * .5f), ImVec2(track_right, cy + track_h * .5f),
                          control(theme::surface4), track_h * .5f);
        float x = control_left + track_w * v.fraction;
        dl->AddRectFilled(ImVec2(control_left, cy - track_h * .5f), ImVec2(x, cy + track_h * .5f),
                          control(theme::focus), track_h * .5f);
        dl->AddCircle(ImVec2(x, cy), fs * .375f + 3 * g_scale, control(theme::focus), 0, 3 * g_scale);
        dl->AddCircleFilled(ImVec2(x, cy), fs * .375f, fg);
        float tw = ImGui::CalcTextSize(v.value).x;
        dl->AddText(ImVec2(right - tw, cy - fs * .5f), fg, v.value);
        float origin = ImGui::GetIO().MouseClickedPos[0].x;
        if (!off && origin >= control_left - fs * .4f && origin <= track_right + fs * .4f &&
            ((ImGui::IsItemActive() && ImGui::IsMouseDown(0)) || mouse_click)) {
            g_drag_fraction = fminf(1, fmaxf(0, (ImGui::GetIO().MousePos.x - control_left) / track_w));
            step = 3;
        }
    } else {
        float tw = ImGui::CalcTextSize(v.value).x;
        control_left = right - tw - fs;
        dl->AddText(ImVec2(control_left, cy - fs * .5f), dim, v.value);
        float cx = right - fs * .25f, dx = fs * .14f, dy = fs * .23f;
        dl->AddLine(ImVec2(cx - dx, cy - dy), ImVec2(cx + dx, cy), dim, 2 * g_scale);
        dl->AddLine(ImVec2(cx + dx, cy), ImVec2(cx - dx, cy + dy), dim, 2 * g_scale);
    }

    float label_end = control_left - fs * .6f;
    if (v.note && *v.note) {
        ImGui::PushFont(theme::fonts().small);
        float sf = ImGui::GetFontSize(), nw = fminf(label_end - p.x - pad, ImGui::CalcTextSize(v.note).x + sf);
        if (nw > sf) {
            float x = label_end - nw;
            dl->AddRectFilled(ImVec2(x, cy - sf * .65f), ImVec2(label_end, cy + sf * .65f), theme::warn_soft, sf * .65f);
            clipped_text(dl, ImVec2(x + sf * .5f, cy - sf * .5f), ImVec2(label_end - sf * .3f, cy + sf * .5f),
                         v.note, theme::warn);
            label_end = x - fs * .4f;
        }
        ImGui::PopFont();
    }
    const char *title = g_label_text ? g_label_text : menu_text(label);
    float x = p.x + pad, badge_w = 0;
    if ((m && !m->live) || g_restart) {
        ImGui::PushFont(theme::fonts().small);
        badge_w = ImGui::CalcTextSize(menu_text(UI_MENU_BADGE_RESTART)).x + ImGui::GetFontSize() * .8f;
        ImGui::PopFont();
    }
    float dot_w = m && !v.is_default ? fs * .8f : 0;
    float label_limit = fmaxf(x, label_end - badge_w - dot_w - fs * .4f);
    clipped_text(dl, ImVec2(x, cy - fs * .5f), ImVec2(label_limit, cy + fs * .5f), title, fg);
    x += fminf(ImGui::CalcTextSize(title).x, label_limit - x) + fs * .4f;
    if (badge_w && x + badge_w <= label_end) {
        ImGui::PushFont(theme::fonts().small);
        float sf = ImGui::GetFontSize();
        dl->AddRectFilled(ImVec2(x, cy - sf * .65f), ImVec2(x + badge_w, cy + sf * .65f), theme::warn_soft, sf * .65f);
        dl->AddText(ImVec2(x + sf * .4f, cy - sf * .5f), theme::warn, menu_text(UI_MENU_BADGE_RESTART));
        ImGui::PopFont();
        x += badge_w + fs * .4f;
    }
    if (dot_w && x + dot_w <= label_end) dl->AddCircleFilled(ImVec2(x + fs * .15f, cy), fs * .15f, theme::focus);
    ImGui::PopID();
    return off ? 0 : step;
}

// ---- writing a setting: at once in memory (live settings follow), to totk.ini a moment later (opt_save_tick)
// g_save_at: plat_now_ns deadline of the pending write; 0 none.
static uint64_t g_save_at, g_saved_at;
static bool g_save_logged;

// The value each setting read only at start had when the game started (NULL for a live one), taken before the menu
// changes the first one: only the menu writes settings while the game runs.
#define START_MAX 160
static char *g_start_value[START_MAX];
static int g_start_count = -1;

static void start_values(void) {
    if (g_start_count >= 0) return;
    g_start_count = 0;
    for (const CfgMeta *m; g_start_count < START_MAX && (m = cfg_meta_at(g_start_count)); g_start_count++) {
        if (m->live) continue;
        char name[80];
        snprintf(name, sizeof name, "TOTK_%s", m->key);
        g_start_value[g_start_count] = strdup(cfg_value(name));
    }
}

bool opt_restart_needed(void) {
    start_values();
    for (int i = 0; i < g_start_count; i++) {
        const CfgMeta *m = cfg_meta_at(i);
        if (!m || !g_start_value[i]) continue;
        char name[80];
        snprintf(name, sizeof name, "TOTK_%s", m->key);
        if (strcmp(cfg_value(name), g_start_value[i])) return true;
    }
    return false;
}

static void apply(const char *key, const char *value) {
    start_values();
    if (*value) cfg_set(key, value);
    // The automatic choice: the key unset, its line removed from totk.ini.
    else { const char *k = key; cfg_reset(&k, 1); }
    opt_save_soon();
}

void opt_save_soon(void) { g_save_at = plat_now_ns() + 400000000ull; }

int opt_save_tick(int force) {
    if (!g_save_at || (!force && plat_now_ns() < g_save_at)) return g_save_at != 0;
    g_save_at = 0;
    if (cfg_save()) g_saved_at = plat_now_ns();
    else if (!g_save_logged) {
        g_save_logged = true;
        rt_log("menu: could not write totk.ini; the changed settings hold for this session");
    }
    return 0;
}

int opt_status(void) {
    if (!cfg_last_save_ok()) return 3;
    if (opt_restart_needed()) return 2;
    if (g_saved_at && plat_now_ns() - g_saved_at < 4000000000ull) return 1;
    return 0;
}

bool opt_toggle(const char *key, UiTextId label, UiTextId desc, bool inverted, const char *note) {
    const CfgMeta *m = cfg_meta(key);
    bool set = cfg_bool(key) != 0, def = m && strcmp(m->def, "0") != 0, on = set != inverted;
    RowView v = {ROW_TOGGLE, on ? "1" : "0", menu_text((def != inverted) ? UI_MENU_ON : UI_MENU_OFF), note,
                 set == def, nullptr, 0, 0, 0};
    int s = row(key, label, desc, v);
    if (!s) return false;
    // A / click flips it; left / right select off / on.
    bool next = s == 2 ? !on : s > 0;
    if (next == on) return false;
    apply(key, (next != inverted) ? "1" : "0");
    return true;
}

bool opt_button(const char *id, UiTextId label, UiTextId desc, const char *value) {
    RowView v = {ROW_BUTTON, value ? value : "", nullptr, nullptr, true, nullptr, 0, 0, 0};
    // A or a click; left and right do nothing.
    return row(id, label, desc, v) == 2;
}

bool opt_info(const char *id, const char *label, const char *value, const char *desc) {
    g_label_text = label, g_desc_text = desc;
    RowView v = {ROW_BUTTON, value ? value : "", nullptr, nullptr, true, nullptr, 0, 0, 0};
    bool pressed = row(id, UI_MENU_TITLE, UI_MENU_TITLE, v) == 2;
    g_label_text = g_desc_text = nullptr;
    return pressed;
}

void opt_group(UiTextId title) {
    card_end();
    ImGui::Dummy(ImVec2(0, 22 * g_scale));
    ImGui::PushFont(theme::fonts().small);
    ImGui::TextColored(theme::colour(theme::text_dim), "%s", menu_text(title));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 8 * g_scale));
}

void opt_text(UiTextId text, bool warn) {
    card_end();
    ImGui::PushFont(theme::fonts().small);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colour(warn ? theme::warn : theme::text_dim));
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (warn) {
        ImGui::Indent(12 * g_scale);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(menu_text(text));
        ImGui::PopTextWrapPos();
        ImGui::Unindent(12 * g_scale);
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 3 * g_scale, ImGui::GetCursorScreenPos().y), theme::warn);
    } else {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(menu_text(text));
        ImGui::PopTextWrapPos();
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 8 * g_scale));
}

static bool choice(const char *key, UiTextId label, const char *const *values, const char *const *names, int n,
                   UiTextId desc, int cur, const char *note, bool segmented) {
    const CfgMeta *m = cfg_meta(key);
    const char *now = cfg_value(key);
    if (cur < 0) for (int i = 0; i < n; i++) if (!strcmp(values[i], now)) cur = i;
    int def = -1;
    for (int i = 0; m && i < n; i++) if (!strcmp(values[i], m->def)) def = i;
    RowView v = {segmented && n <= 4 ? ROW_SEGMENTS : ROW_CYCLE, cur >= 0 ? names[cur] : now,
                 def >= 0 ? names[def] : nullptr, note, cur == def, names, n, cur, 0};
    int s = row(key, label, desc, v);
    if (!s || n <= 0) return false;
    // A / click on the row: the next value, round to the first; a segment click selects it; left / right stop at the ends.
    int nv = s >= 100 ? s - 100 : cur < 0 ? 0 : s == 2 ? (cur + 1) % n : cur + s;
    nv = nv < 0 ? 0 : nv >= n ? n - 1 : nv;
    if (nv == cur) return false;
    apply(key, values[nv]);
    return true;
}

bool opt_choice_list(const char *key, UiTextId label, const char *const *values, const char *const *names, int n,
                     UiTextId desc, int cur, const char *note) {
    return choice(key, label, values, names, n, desc, cur, note, false);
}

// A selector over several keys: the caller maps the index to them. skip: a choice stepping passes over (-1 none).
int opt_select(const char *key, UiTextId label, const UiTextId *names, int n, int cur, UiTextId desc, int def, int skip,
               bool restart, const char *note) {
    enum { MAX = 8 };
    const char *shown[MAX];
    if (n > MAX) n = MAX;
    for (int i = 0; i < n; i++) shown[i] = menu_text(names[i]);
    g_restart_next = restart;
    RowView v = {n <= 4 ? ROW_SEGMENTS : ROW_CYCLE, cur >= 0 && cur < n ? shown[cur] : "",
                 def >= 0 && def < n ? shown[def] : nullptr, note, cur == def, shown, n, cur, 0};
    int step = row(key, label, desc, v);
    if (!step || n <= 0) return -1;
    int nv = cur;
    if (step >= 100) {   // a segment was clicked
        nv = step - 100;
    } else if (step == 2) {
        nv = (cur + 1) % n;
        if (nv == skip) nv = (nv + 1) % n;
    } else {
        nv = cur + step < 0 ? 0 : cur + step >= n ? n - 1 : cur + step;
        if (nv == skip) nv = nv + step < 0 || nv + step >= n ? cur : nv + step;   // left / right pass over it
    }
    return nv == cur || nv == skip || nv < 0 || nv >= n ? -1 : nv;
}

bool opt_choice(const char *key, UiTextId label, const UiTextId *names, int n, UiTextId desc, const char *note, int cur) {
    enum { MAX = 32 };
    const CfgMeta *m = cfg_meta(key);
    const char *values[MAX], *shown[MAX];
    char buf[256];
    int k = 0;
    // The automatic choice.
    if (m && !m->def[0]) values[k++] = "";
    snprintf(buf, sizeof buf, "%s", m && m->values ? m->values : "");
    for (char *s = buf, *e; *s && k < MAX; s = e) {
        e = strchr(s, '|');
        if (e) *e++ = 0; else e = s + strlen(s);
        values[k++] = s;
    }
    if (n > k) n = k;
    for (int i = 0; i < n; i++) shown[i] = menu_text(names[i]);
    return choice(key, label, values, shown, n, desc, cur < n ? cur : -1, note, true);
}

bool opt_slider(const char *key, UiTextId label, float lo, float hi, UiTextId desc, float step, const char *fmt) {
    const CfgMeta *m = cfg_meta(key);
    double value = cfg_double(key), def = m ? strtod(m->def, nullptr) : lo;
    char shown[48], default_text[48];
    snprintf(shown, sizeof shown, fmt, value);
    snprintf(default_text, sizeof default_text, fmt, def);
    RowView v = {ROW_SLIDER, shown, default_text, nullptr, fabs(value - def) < step * .01, nullptr, 0, 0,
                 (float)fmin(1, fmax(0, (value - lo) / (hi - lo)))};
    int s = row(key, label, desc, v);
    if (!s) return false;
    // Held: ten steps at a time over a long range.
    int steps = g_held && (hi - lo) > 40 * step ? 10 : 1;
    double next = s == 3 ? round((lo + (hi - lo) * g_drag_fraction) / step) * step
                         : round(value / step) * step + (s == 2 ? step : s * step * steps);
    // A / click off the track past the top goes round; left / right stop at the ends.
    if (s == 2 && next > hi + step * .01) next = lo;
    next = next < lo ? lo : next > hi ? hi : next;
    if (fabs(next - value) < step * .01) return false;
    char out[32];
    if (step >= 1) snprintf(out, sizeof out, "%d", (int)lround(next));
    else snprintf(out, sizeof out, "%g", round(next / step) * step);
    apply(key, out);
    return true;
}

// ---- binding rows (the remapping rows of the Controls tab): a label and three cells, each its own nav item
//
// The label column and the three cells of a binding row of width w.
static void bind_columns(float x, float w, float cx[3], float *cw) {
    float lw = floorf(w * .34f), gap = floorf(8 * g_scale);
    *cw = floorf((w - lw - gap * 3) / 3);
    for (int c = 0; c < 3; c++) cx[c] = x + lw + gap + c * (*cw + gap);
}

void opt_bind_header(UiTextId c0, UiTextId c1, UiTextId c2) {
    card_end();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, cx[3], cw;
    bind_columns(p.x, w, cx, &cw);
    const UiTextId cols[3] = {c0, c1, c2};
    ImGui::PushFont(theme::fonts().small);
    float fs = ImGui::GetFontSize();
    for (int c = 0; c < 3; c++) {
        const char *t = menu_text(cols[c]);
        float x = fmaxf(cx[c], cx[c] + (cw - ImGui::CalcTextSize(t).x) * .5f);
        clipped_text(ImGui::GetWindowDrawList(), ImVec2(x, p.y), ImVec2(cx[c] + cw, p.y + fs), t, theme::text_faint);
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(w, fs * 1.4f));
}

int opt_bind_row(const char *key, const char *label, UiTextId desc, const char *const cells[3], const bool enabled[3],
                 const char *note) {
    ImGui::PushID(key);
    ImVec2 p = ImGui::GetCursorScreenPos();
    // h whole pixels: ImGui truncates cursor positions, which rounds a fractional height the other way above the window's
    // top (negative y), so the page grew a pixel per row scrolled out and a row scrolled to the bottom was cut off.
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = floorf(fs * 2.4f), cx[3], cw;
    bind_columns(p.x, w, cx, &cw);
    card_row(p, w, h);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    clipped_text(dl, ImVec2(p.x + fs * .8f, p.y + (h - fs) * .5f),
                 ImVec2(cx[0] - fs * .4f, p.y + (h + fs) * .5f), label, theme::text);
    int result = -1;
    bool row_foc = false, row_hov = false;
    for (int c = 0; c < 3; c++) {
        float ch = fs * 1.5f;
        ImVec2 a(cx[c], p.y + (h - ch) * .5f), b(cx[c] + cw, p.y + (h + ch) * .5f);
        const char *t = cells[c] && *cells[c] ? cells[c] : "-";
        bool hov = false, foc = false, clicked = false;
        // A disabled cell is not a nav item: the cursor skips it.
        if (enabled[c]) {
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(c);
            // The cell's own border shows the selection.
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            clicked = ImGui::InvisibleButton("##cell", ImVec2(cw, ch), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            char id[80];
            snprintf(id, sizeof id, "%s#%d", key, c);
            bool to_key = g_focus_key[0] && !strcmp(id, g_focus_key);
            if (g_take_first || to_key) {
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                if (g_take_first) ImGui::SetScrollY(0); else ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
                g_take_first = false;
                if (to_key) g_focus_key[0] = 0;
            }
            hov = ImGui::IsItemHovered();
            foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
            if (clicked) result = c;
            if (foc && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))) result = 10 + c;
            ImGui::PopID();
        }
        row_foc |= foc; row_hov |= hov;
        dl->AddRectFilled(a, b, hov ? theme::surface3 : theme::surface1, 6 * g_scale);
        dl->AddRect(a, b, theme::line_strong, 6 * g_scale);
        if (foc) focus_ring(dl, a, b, 6 * g_scale);
        float tw = ImGui::CalcTextSize(t).x;
        clipped_text(dl, ImVec2(fmaxf(a.x + 5 * g_scale, (a.x + b.x - tw) * .5f), p.y + (h - fs) * .5f),
                     ImVec2(b.x - 5 * g_scale, p.y + (h + fs) * .5f), t, enabled[c] ? theme::text : theme::text_faint);
    }
    char k[64];
    snprintf(k, sizeof k, "TOTK_%s", key);
    if (row_foc) desc_set(g_focus, desc, k, nullptr, nullptr, nullptr, note);
    if (row_hov) desc_set(g_hover, desc, k, nullptr, nullptr, nullptr, note);
    if (!g_first.text[0]) desc_set(g_first, desc, k, nullptr, nullptr);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
    return result;
}
