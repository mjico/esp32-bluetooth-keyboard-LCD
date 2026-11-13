#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open the shared bus. On success, *xl_addr is the XL9555 address (0x20..0x27). */
esp_err_t i2c_port_start(uint8_t *xl_addr);

/* Drive XL9555 IO1_6 high. The five headers share this same bus. */
esp_err_t i2c_backlight_on(void);

/* LCD backlight on XL9555 IO1_6. High is on. */
esp_err_t i2c_backlight_set(bool on);
esp_err_t i2c_backlight_get(bool *on);

esp_err_t i2c_probe(uint8_t addr);

/* Fill addrs[0..*count). Stops early on repeated timeouts. */
esp_err_t i2c_scan(uint8_t *addrs, int *count, int max_count);

/* Register access. len is 1..16. write sends reg followed by data. */
esp_err_t i2c_reg_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);
esp_err_t i2c_reg_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len);

void i2c_register_commands(void);

/* argv is the text after "system backlight". */
void i2c_backlight_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif
