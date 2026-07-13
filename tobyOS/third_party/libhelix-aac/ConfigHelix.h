/* ConfigHelix.h -- minimal freestanding config for the Helix AAC decoder
 * on tobyOS (replaces the Arduino-libhelix wrapper's version). Enables
 * AAC-LC + SBR (HE-AAC) with all tables in normal RAM (no PROGMEM). */
#pragma once

/* Feature set: full AAC-LC + SBR + MPEG-4 syntax. */
#define HELIX_FEATURE_AUDIO_CODEC_AAC_SBR
#define AAC_ENABLE_SBR
#define AAC_ENABLE_MPEG

/* Tables live in normal memory; the PROGMEM read helpers are plain
 * dereferences (see utils/helix_pgm.h). */
#include "utils/helix_pgm.h"
