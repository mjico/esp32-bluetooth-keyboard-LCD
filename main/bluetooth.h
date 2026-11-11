#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Clear a leftover RF power-domain state left by a soft reset mid-calibration. */
void bluetooth_rf_reset(void);

/* Scan for a bonded HID and connect it. Unknown advertisers are ignored. */
esp_err_t bluetooth_reconnect(esp_lcd_panel_handle_t panel);

/* Scan for HID devices and show the picker, including ones that are not bonded yet. */
esp_err_t bluetooth_discover(esp_lcd_panel_handle_t panel);

/* Connect to a device from the last scan. index is 1-based. */
esp_err_t bluetooth_connect(esp_lcd_panel_handle_t panel, int index);

void bluetooth_register_commands(void);

#ifdef __cplusplus
}
#endif
