#pragma once

#include "esp_err.h"

/* Called with one finished line. The newline is not included. */
typedef void (*debug_port_line_fn)(const char *line, void *ctx);

/* Read the USB debug port, echo every key, and deliver each line. */
esp_err_t debug_port_start(debug_port_line_fn on_line, void *ctx);
