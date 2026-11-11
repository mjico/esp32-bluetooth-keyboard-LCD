#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_TEXT_WIDTH   320
#define LCD_TEXT_HEIGHT  240
#define LCD_TEXT_CHAR_W  8
#define LCD_TEXT_CHAR_H  16
#define LCD_TEXT_COLS    (LCD_TEXT_WIDTH / LCD_TEXT_CHAR_W)
#define LCD_TEXT_ROWS    (LCD_TEXT_HEIGHT / LCD_TEXT_CHAR_H)

/* Bring up the ST7789 and keep the panel inside this module. */
esp_err_t lcd_text_start(void);
bool lcd_text_ready(void);

/* Draw one text row. row is 0..14 from the top. Only the first 40 characters are used.
 * Characters past the end of the string are cleared. Other rows are left as they are. */
esp_err_t lcd_text_write_row(int row, const char *text);

/* Append one line, like a terminal. The first 15 lines fill rows 0..14.
 * After that, every line scrolls the screen up and is drawn on the last row. */
esp_err_t lcd_text_append_line(const char *text);
void lcd_text_set_screen_lines(int screen_line);

/* Draw the line being edited on the next free row. Later edits rewrite that row.
 * A following append keeps this line under the new text. */
esp_err_t lcd_text_set_draft(const char *text);
void lcd_text_dismiss_draft(void);

/* Change glyph color and redraw the rows already on screen. color is RGB565. */
esp_err_t lcd_text_set_fg_color(uint16_t color);

/* Redraw the rows already stored, using the current font. */
void lcd_text_redraw(void);

/* Draw the boot picture, one row every 500 ms, through trace. */
esp_err_t lcd_text_show_splash(void);

void lcd_text_register_commands(void);

#ifdef __cplusplus
}
#endif
