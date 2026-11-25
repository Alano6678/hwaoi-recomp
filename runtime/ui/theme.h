// The in-game menu's "Seal of the Imprisoned" colours and fonts.
#pragma once
#include "imgui.h"

namespace theme {
constexpr ImU32 bg = IM_COL32(11, 13, 17, 255);
constexpr ImU32 surface1 = IM_COL32(15, 17, 21, 255);
constexpr ImU32 surface2 = IM_COL32(23, 26, 33, 255);
constexpr ImU32 surface3 = IM_COL32(32, 36, 46, 255);
constexpr ImU32 surface4 = IM_COL32(42, 47, 59, 255);
constexpr ImU32 line = IM_COL32(46, 52, 66, 255);
constexpr ImU32 line_strong = IM_COL32(61, 69, 86, 255);
constexpr ImU32 text = IM_COL32(239, 230, 210, 255);
constexpr ImU32 text_dim = IM_COL32(185, 176, 157, 255);
constexpr ImU32 text_faint = IM_COL32(138, 132, 118, 255);
constexpr ImU32 focus = IM_COL32(63, 198, 255, 255);
constexpr ImU32 focus_soft = IM_COL32(63, 198, 255, 41);
constexpr ImU32 gold = IM_COL32(231, 183, 90, 255);
constexpr ImU32 gold_strong = IM_COL32(244, 204, 119, 255);
constexpr ImU32 gold_ink = IM_COL32(27, 20, 8, 255);
constexpr ImU32 ok = IM_COL32(79, 209, 139, 255);
constexpr ImU32 warn = IM_COL32(255, 181, 71, 255);
constexpr ImU32 warn_soft = IM_COL32(255, 181, 71, 36);
constexpr ImU32 err = IM_COL32(255, 90, 79, 255);
constexpr ImU32 knob_on = IM_COL32(6, 32, 44, 255);

struct Fonts { ImFont *text, *small, *title, *brand; };
const Fonts &fonts();
bool button(const char *label, ImVec2 size, float scale, bool primary);

inline ImVec4 colour(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
inline ImU32 alpha(ImU32 c, float a) {
    return (c & 0x00ffffffu) | ((ImU32)(a * 255.0f) << 24);
}
}
