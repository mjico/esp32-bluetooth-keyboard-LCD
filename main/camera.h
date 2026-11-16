#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start a 240x240 preview. Any later key calls camera_preview_request_stop. */
esp_err_t camera_preview_start(void);
void camera_preview_request_stop(void);
bool camera_preview_active(void);

void camera_register_commands(void);

#ifdef __cplusplus
}
#endif
