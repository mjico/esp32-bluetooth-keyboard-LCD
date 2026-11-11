#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Submitted commands Up can recall. Oldest entries fall off after this many. */
#define COMMAND_HISTORY_COUNT 8

/* Remember one finished command. Blank lines are ignored. */
void command_history_add(const char *line);

/* Copy the next older command into out. *cursor starts at -1. Returns false at the end. */
bool command_history_up(int *cursor, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
