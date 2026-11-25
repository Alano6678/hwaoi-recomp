// The colour profiles of the in-game menu (ui/menu.cpp) and of the launcher's "Colour" setting, which writes only the
// profile's name to hwaoi.ini. The picture (vk_color.c) takes a named profile's four values from this table; only
// "custom" uses COLOR_BRIGHTNESS/CONTRAST/SATURATION/GAMMA as the player set them.
#pragma once
#include <stddef.h>
#include <string.h>

typedef struct {
    const char *name, *brightness, *contrast, *saturation, *gamma;
} ColorPreset;

static const ColorPreset color_presets[] = {
    {"default", "0", "100", "100", "1"},
    {"vivid", "0", "115", "135", "0.95"},
    {"clarity", "2", "110", "115", "1"},
    // for OLED and mini-LED screens, whose blacks are black: deeper shadows that keep their detail (the curve, not
    // more contrast, darkens them) and richer colour
    {"oled", "0", "105", "120", "1.1"},
};

// The named profile, or NULL for "custom" and any other name: the four values are then the player's own.
static inline const ColorPreset *color_preset_find(const char *name) {
    for (size_t i = 0; name && i < sizeof color_presets / sizeof *color_presets; i++)
        if (!strcmp(name, color_presets[i].name)) return &color_presets[i];
    return NULL;
}
