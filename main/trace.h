#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void trace_set_screen(bool on);
void trace_set_serial(bool on);
bool trace_screen_enabled(void);
bool trace_serial_enabled(void);

/* Live input line. Both sinks show this same text while it is being edited. */
void trace_draft(const char *text);

/* Keep that line and move to the next row. Does not print it a second time. */
void trace_commit(void);

/* One fixed screen row, and the same text as a serial line when that switch is on. */
void trace_row(int row, const char *text);

/* One console line. Each enabled switch receives it. */
void trace_print(const char *text);
void trace_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void trace_register_commands(void);

#ifdef __cplusplus
}
#endif
