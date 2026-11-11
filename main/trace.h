#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void trace_set_screen(bool on);
void trace_set_serial(bool on);
bool trace_screen_enabled(void);
bool trace_serial_enabled(void);

/* Draw text on the screen only. Used for the serial port's own input echo. */
void trace_print_screen(const char *text);

/* One console line. Each enabled switch receives it. */
void trace_print(const char *text);
void trace_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void trace_register_commands(void);

#ifdef __cplusplus
}
#endif
