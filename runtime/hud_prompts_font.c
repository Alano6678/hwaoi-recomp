// The font of the letters in the button pictures hud_prompts.c draws: Roboto Medium, unmodified (Apache-2.0,
// ui/fonts/LICENSE-Roboto.txt). Its weight is close to the letters of the game's own pictures. A file of its own, so
// that hud_prompts.c compiles where #embed does not (the tests give it a font file instead).
#include "hud_prompts.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"   // #embed before C23
static const unsigned char roboto_medium[] = {
#embed "ui/fonts/Roboto-Medium.ttf"
};
#pragma clang diagnostic pop

const unsigned char *hud_prompts_font(size_t *size) {
    *size = sizeof roboto_medium;
    return roboto_medium;
}
