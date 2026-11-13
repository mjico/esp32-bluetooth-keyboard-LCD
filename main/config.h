#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load the saved settings. Missing or bad data keeps the defaults. */
void config_start(void);

uint16_t config_color(void);
int config_font(void);
int config_page(void);
bool config_backlight(void);
bool config_bt(void);
bool config_reconnect(void);

/* Each setter stores the value when it changes. */
void config_set_color(uint16_t color);
void config_set_font(int width);
void config_set_page(int pages);
void config_set_backlight(bool on);
void config_set_bt(bool on);
void config_set_reconnect(bool on);

#ifdef __cplusplus
}
#endif
