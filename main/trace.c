#include "trace.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "command_interpreter.h"
#include "lcd_text.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TRACE_LINE_MAX 5
static const char trace_mark[] = "[...]";

static esp_lcd_panel_handle_t trace_panel;
static bool trace_screen_on = true;
static bool trace_serial_on = false;
static SemaphoreHandle_t trace_mu;

static void trace_lock(void)
{
    if (trace_mu == NULL) {
        trace_mu = xSemaphoreCreateMutex();
    }
    if (trace_mu != NULL) {
        xSemaphoreTake(trace_mu, portMAX_DELAY);
    }
}

static void trace_unlock(void)
{
    if (trace_mu != NULL) {
        xSemaphoreGive(trace_mu);
    }
}

void trace_bind(esp_lcd_panel_handle_t panel)
{
    trace_lock();
    trace_panel = panel;
    trace_unlock();
}

void trace_set_screen(bool on)
{
    trace_lock();
    trace_screen_on = on;
    trace_unlock();
}

void trace_set_serial(bool on)
{
    trace_lock();
    trace_serial_on = on;
    trace_unlock();
}

bool trace_screen_enabled(void)
{
    bool on;
    trace_lock();
    on = trace_screen_on;
    trace_unlock();
    return on;
}

bool trace_serial_enabled(void)
{
    bool on;
    trace_lock();
    on = trace_serial_on;
    trace_unlock();
    return on;
}

static void trace_serial(const char *text)
{
    /* The console driver already turns LF into CR LF. */
    printf("%s\n", text != NULL ? text : "");
}

static void trace_screen(const char *text)
{
    char chunk[LCD_TEXT_COLS + 1];
    int offset = 0;
    int length;

    if (text == NULL) {
        return;
    }
    length = (int)strlen(text);
    if (length == 0) {
        lcd_text_append_line(trace_panel, "");
        return;
    }
    for (int row = 0; row < TRACE_LINE_MAX && offset < length; row++) {
        int at = 0;
        int room = LCD_TEXT_COLS;
        int mark = (int)strlen(trace_mark);
        bool last = (row == TRACE_LINE_MAX - 1);

        if (offset > 0) {
            chunk[0] = ' ';
            at = 1;
            room = LCD_TEXT_COLS - 1;
        }
        if (last && offset + room < length) {
            if (mark > room) {
                mark = room;
            }
            room -= mark;
            memcpy(chunk + at, text + offset, (size_t)room);
            memcpy(chunk + at + room, trace_mark, (size_t)mark);
            chunk[at + room + mark] = '\0';
            lcd_text_append_line(trace_panel, chunk);
            return;
        }
        if (room > length - offset) {
            room = length - offset;
        }
        memcpy(chunk + at, text + offset, (size_t)room);
        chunk[at + room] = '\0';
        lcd_text_append_line(trace_panel, chunk);
        offset += room;
    }
}

void trace_print_screen(const char *text)
{
    trace_lock();
    if (trace_screen_on && trace_panel != NULL) {
        trace_screen(text);
    }
    trace_unlock();
}

void trace_print(const char *text)
{
    bool screen;
    bool serial;
    esp_lcd_panel_handle_t panel;

    trace_lock();
    screen = trace_screen_on;
    serial = trace_serial_on;
    panel = trace_panel;
    if (panel == NULL) {
        trace_serial(text);
        trace_unlock();
        return;
    }
    if (screen) {
        trace_screen(text);
    }
    if (serial) {
        trace_serial(text);
    }
    trace_unlock();
}

void trace_printf(const char *fmt, ...)
{
    char line[256];
    va_list args;

    if (fmt == NULL) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    trace_print(line);
}

static bool same_word(const char *text, const char *name)
{
    if (text == NULL) {
        return false;
    }
    while (*text != '\0' && *name != '\0') {
        if (tolower((unsigned char)*text) != tolower((unsigned char)*name)) {
            return false;
        }
        text++;
        name++;
    }
    return *text == '\0' && *name == '\0';
}

static void show_switch(const char *name, bool on)
{
    trace_printf("trace %s %d", name, on ? 1 : 0);
}

static void cmd_trace(esp_lcd_panel_handle_t panel, int argc, char **argv)
{
    bool screen = trace_screen_enabled();
    bool serial = trace_serial_enabled();
    const char *name = NULL;
    bool next;
    bool is_screen;

    (void)panel;
    if (argc < 2) {
        show_switch("screen", screen);
        show_switch("serial", serial);
        return;
    }
    if (same_word(argv[1], "screen")) {
        name = "screen";
    } else if (same_word(argv[1], "serial")) {
        name = "serial";
    } else {
        trace_print("trace serial|screen [0|1]");
        return;
    }
    is_screen = name[1] == 'c';
    if (argc < 3) {
        show_switch(name, is_screen ? screen : serial);
        return;
    }
    if ((argv[2][0] != '0' && argv[2][0] != '1') || argv[2][1] != '\0') {
        trace_print("trace serial|screen [0|1]");
        return;
    }
    next = argv[2][0] == '1';
    /* Say the new state before turning off the last remaining switch. */
    if (!next && (is_screen ? !serial : !screen)) {
        show_switch(name, false);
        if (is_screen) {
            trace_set_screen(false);
        } else {
            trace_set_serial(false);
        }
        return;
    }
    if (is_screen) {
        trace_set_screen(next);
    } else {
        trace_set_serial(next);
    }
    show_switch(name, next);
}

void trace_register_commands(void)
{
    command_register("trace", "serial|screen [0|1]", cmd_trace);
}
