#include <stddef.h>

#include "command_history.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define COMMAND_HISTORY_LINE 128

static char slots[COMMAND_HISTORY_COUNT][COMMAND_HISTORY_LINE + 1];
static int count;
static int newest;
static portMUX_TYPE history_mux = portMUX_INITIALIZER_UNLOCKED;

static void copy_out(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (dst_size == 0) {
        return;
    }
    while (src[i] != '\0' && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void command_history_add(const char *line)
{
    const char *text = line;
    int slot = 0;
    size_t i = 0;

    if (line == NULL) {
        return;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    if (*text == '\0') {
        return;
    }

    portENTER_CRITICAL(&history_mux);
    if (count < COMMAND_HISTORY_COUNT) {
        slot = count;
        count++;
    } else {
        slot = (newest + 1) % COMMAND_HISTORY_COUNT;
    }
    newest = slot;
    while (line[i] != '\0' && i < COMMAND_HISTORY_LINE) {
        slots[slot][i] = line[i];
        i++;
    }
    slots[slot][i] = '\0';
    portEXIT_CRITICAL(&history_mux);
}

bool command_history_up(int *cursor, char *out, size_t out_size)
{
    int index = 0;

    if (cursor == NULL || out == NULL || out_size == 0) {
        return false;
    }

    portENTER_CRITICAL(&history_mux);
    if (count == 0 || *cursor + 1 >= count) {
        portEXIT_CRITICAL(&history_mux);
        return false;
    }
    *cursor += 1;
    index = newest - *cursor;
    if (index < 0) {
        index += COMMAND_HISTORY_COUNT;
    }
    copy_out(out, out_size, slots[index]);
    portEXIT_CRITICAL(&history_mux);
    return true;
}
