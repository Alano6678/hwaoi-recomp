// The game's own button pictures follow the device in use: its maker's signs and the player's bindings (hud_prompts.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "input.h"

// vk_texture.c upload(): blocks is level 0 of a BC3 texture of w x h texels on its way to the GPU, 16 bytes a block in
// row order. When it is one of the game's three textures with button pictures (its atlas, the battle HUD's sheet, the
// skill wheel's), the pictures of the buttons the player presses otherwise than the game draws them are drawn again
// in it. Returns 1 for such a texture, with *gen the hud_prompts_gen() it was drawn for: the caller uploads the
// texture again when that number changes.
int hud_prompts_patch(uint8_t *blocks, size_t size, uint32_t w, uint32_t h, uint32_t *gen);
// Changes when the pictures do: another device in use, another layout or binding, the setting. Any thread; it looks at
// the devices a few times a second.
uint32_t hud_prompts_gen(void);

// The font of the pictures' letters, a TrueType file in memory that stays there (hud_prompts_font.c embeds Roboto
// Medium; a test gives its own). NULL: the letters are left out.
const unsigned char *hud_prompts_font(size_t *size);

// For the tests: the pictures of one state of the devices drawn into blocks, a texture of the size of one of the
// sheets, whatever it holds (1024 x 1024 the atlas, 2048 x 1024 the HUD's sheet, 2048 x 512 the skill wheel's). mode is
// a TOTK_BUTTON_PROMPTS value. Returns how many pictures it drew.
int hud_prompts_draw(uint8_t *blocks, uint32_t w, uint32_t h, const InputPromptState *in, const char *mode);
// The same for a sheet by its number: 0 the atlas, 1 the HUD's sheet, 2 the skill wheel's, 3 the three-disc wheels,
// 4 the second skill wheel sheet (the last two are 2048 x 1024 as the HUD's).
int hud_prompts_draw_sheet(int sheet, uint8_t *blocks, const InputPromptState *in, const char *mode);
