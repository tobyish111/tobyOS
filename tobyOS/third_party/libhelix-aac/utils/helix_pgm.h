/* helix_pgm.h -- freestanding PROGMEM shim for tobyOS (no Arduino flash
 * memory; tables are plain RAM). */
#pragma once
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#endif
#ifndef pgm_read_word
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
#endif
