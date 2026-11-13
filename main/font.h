#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Active glyph is 16 rows, bit 7 is the left pixel. Unknown characters use space. */
const uint8_t *font_glyph(char ch);

/* 5 is the original width. 6 is the wider set. */
int font_width(void);
void font_set_width(int width);

/* argv is the text after "system font". */
void font_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif
