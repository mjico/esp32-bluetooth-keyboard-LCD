#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "debug_port.h"

#include "command_history.h"
#include "camera.h"
#include "lcd_text.h"
#include "trace.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DEBUG_LINE_MAX 128

static debug_port_line_fn s_on_line;
static void *s_ctx;

static void show_edit(const char *line)
{
    char shown[DEBUG_LINE_MAX + 2];

    snprintf(shown, sizeof(shown), ">%s", line != NULL ? line : "");
    trace_draft(shown);
}

/* CR LF from the host becomes two LF. The second one must not become the camera exit key. */
static int read_pending(void)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    unsigned char extra;
    ssize_t got;

    if (flags < 0) {
        return -1;
    }
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    got = read(STDIN_FILENO, &extra, 1);
    fcntl(STDIN_FILENO, F_SETFL, flags);
    if (got == 1) {
        return extra;
    }
    return -1;
}

static void replace_line(char *line, int *length, const char *text)
{
    *length = 0;
    if (text == NULL) {
        line[0] = '\0';
        show_edit(line);
        return;
    }
    while (text[*length] != '\0' && *length < DEBUG_LINE_MAX) {
        line[*length] = text[*length];
        (*length)++;
    }
    line[*length] = '\0';
    show_edit(line);
}

static void debug_task(void *arg)
{
    char line[DEBUG_LINE_MAX + 1];
    int length = 0;
    int hist = -1;
    int esc = 0;
    int esc_page = 0;
    int held = -1;
    (void)arg;

    trace_print("debug port ready");
    show_edit("");

    for (;;) {
        unsigned char ch;
        if (held >= 0) {
            ch = (unsigned char)held;
            held = -1;
        } else {
            ssize_t got = read(STDIN_FILENO, &ch, 1);
            if (got <= 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }
        if (lcd_text_pixels_holding() || camera_preview_active()) {
            if (camera_preview_active()) {
                camera_preview_request_stop();
            } else {
                lcd_text_pixels_release();
            }
            esc = 0;
            continue;
        }

        if (esc == 1) {
            esc = (ch == '[' || ch == 'O') ? 2 : 0;
            continue;
        }
        if (esc == 2) {
            char recalled[DEBUG_LINE_MAX + 1];
            esc = 0;
            if (ch == 'A' && command_history_up(&hist, recalled, sizeof(recalled))) {
                replace_line(line, &length, recalled);
            } else if (ch == '5' || ch == '6') {
                esc = 3;
                esc_page = ch;
            }
            continue;
        }
        if (esc == 3) {
            esc = 0;
            if (ch == '~' && esc_page == '5') {
                lcd_text_page_up();
            } else if (ch == '~' && esc_page == '6') {
                lcd_text_page_down();
            }
            continue;
        }

        if (ch == 0x1b) {
            esc = 1;
            continue;
        }

        if (ch == '\n') {
            int extra = read_pending();
            line[length] = '\0';
            trace_commit();
            length = 0;
            hist = -1;
            if (extra >= 0 && extra != '\n') {
                held = extra;
            }
            if (s_on_line != NULL) {
                s_on_line(line, s_ctx);
            }
            show_edit("");
            continue;
        }

        if (ch == '\b' || ch == 0x7f) {
            if (length > 0) {
                length--;
                line[length] = '\0';
                show_edit(line);
            }
            continue;
        }

        if (length < DEBUG_LINE_MAX) {
            line[length++] = (char)ch;
            line[length] = '\0';
            show_edit(line);
        }
    }
}

esp_err_t debug_port_start(debug_port_line_fn on_line, void *ctx)
{
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    /* Enter from the debug terminal arrives as CR. Send LF back as CR LF. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    fcntl(STDIN_FILENO, F_SETFL, 0);
    fcntl(STDOUT_FILENO, F_SETFL, 0);
    usb_serial_jtag_vfs_use_driver();

    s_on_line = on_line;
    s_ctx = ctx;
    if (xTaskCreate(debug_task, "debug_port", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
