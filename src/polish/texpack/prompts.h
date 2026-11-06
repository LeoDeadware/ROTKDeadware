#pragma once
// Xbox Series button prompts (prompts.mm): redraws the game's Black / White /
// trigger / stick / Back / Start glyph textures. Used by the texture hook.
#include "hle/d3d8/d3d8.h"

// Reads ROTK_PROMPTS / ROTK_PROMPTS_DUMP; true when glyphs are replaced.
bool rotk_polish_prompts_init();
// True (and the replacement in *w2 x *h2 *rgba) for a known glyph texture.
bool rotk_polish_prompt_glyph(const RotkTextureInfo& info, uint32_t* w2, uint32_t* h2, const uint8_t** rgba);
