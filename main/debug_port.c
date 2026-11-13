#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "debug_port.h"

#include "command_history.h"
#include "lcd_text.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DEBUG_LINE_MAX 128

static debug_port_line_fn s_on_line;
static void *s_ctx;

static void echo_bytes(const void *data, size_t length)
{
    const char *bytes = data;
    while (length > 0) {
        ssize_t wrote = write(STDOUT_FILENO, bytes, length);
        if (wrote < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            return;
        }
        bytes += wrote;
        length -= (size_t)wrote;
    }
}

static void replace_line(char *line, int *length, const char *text)
{
    static const char erase[3] = {'\b', ' ', '\b'};
    for (int i = 0; i < *length; i++) {
        echo_bytes(erase, sizeof(erase));
    }
    *length = 0;
    if (text == NULL) {
        line[0] = '\0';
        return;
    }
    while (text[*length] != '\0' && *length < DEBUG_LINE_MAX) {
        line[*length] = text[*length];
        (*length)++;
    }
    line[*length] = '\0';
    if (*length > 0) {
        echo_bytes(line, (size_t)*length);
    }
}

static void debug_task(void *arg)
{
    char line[DEBUG_LINE_MAX + 1];
    int length = 0;
    int hist = -1;
    int esc = 0;
    int esc_page = 0;
    (void)arg;

    static const char ready[] = "\ndebug port ready\n>";
    echo_bytes(ready, sizeof(ready) - 1);

    for (;;) {
        unsigned char ch;
        ssize_t got = read(STDIN_FILENO, &ch, 1);
        if (got <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
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
            echo_bytes("\n>", 2);
            line[length] = '\0';
            if (s_on_line != NULL) {
                s_on_line(line, s_ctx);
            }
            length = 0;
            hist = -1;
            continue;
        }

        if (ch == '\b' || ch == 0x7f) {
            if (length > 0) {
                length--;
                echo_bytes("\b \b", 3);
            }
            continue;
        }

        echo_bytes(&ch, 1);
        if (length < DEBUG_LINE_MAX) {
            line[length++] = (char)ch;
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
