#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "bluetooth.h"
#include "button.h"
#include "command_interpreter.h"
#include "config.h"
#include "debug_port.h"
#include "font.h"
#include "gpio_lab.h"
#include "i2c.h"
#include "sonar.h"
#include "system_cmd.h"
#include "matrix.h"
#include "lcd_text.h"
#include "trace.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void on_key_red(void)
{
    lcd_text_set_fg_color(0xF800);
}

static void on_key_green(void)
{
    lcd_text_set_fg_color(0x07E0);
}

static void on_key_serial(void)
{
    bool on = !trace_serial_enabled();
    if (!on && !trace_screen_enabled()) {
        trace_print("trace serial 0");
        trace_set_serial(false);
        return;
    }
    trace_set_serial(on);
    trace_printf("trace serial %d", on ? 1 : 0);
}

static void on_key_reconnect(void)
{
    bluetooth_reconnect();
}

static void on_key_discover(void)
{
    bluetooth_discover();
}

static void on_key_splash(void)
{
    lcd_text_show_splash();
}

static void on_debug_line(const char *line, void *ctx)
{
    char shown[130];
    (void)ctx;
    if (trace_screen_enabled()) {
        snprintf(shown, sizeof(shown), ">%.*s", 127, line != NULL ? line : "");
        trace_print_screen(shown);
    }
    command_line_execute(line);
}

void app_main(void)
{
    command_init();
    lcd_text_register_commands();
    trace_register_commands();
    bluetooth_register_commands();
    i2c_register_commands();
    sonar_register_commands();
    matrix_register_commands();
    gpio_lab_register_commands();
    system_register_commands();
    config_start();
    font_set_width(config_font());
    lcd_text_set_fg_color(config_color());
    lcd_text_set_page_count(config_page());

    trace_printf("reset reason %d", (int)esp_reset_reason());
    /* Soft reset during RF calibration leaves the modem power domain half-on. */
    bluetooth_rf_reset();

    uint8_t xl_addr = 0;
    bool i2c_ok = i2c_port_start(&xl_addr) == ESP_OK;
    if (i2c_ok && i2c_backlight_set(config_backlight()) == ESP_OK) {
        trace_printf("backlight %d", config_backlight() ? 1 : 0);
    }
    ESP_ERROR_CHECK(lcd_text_start());

    ESP_ERROR_CHECK(lcd_text_show_splash());
    ESP_ERROR_CHECK(debug_port_start(on_debug_line, NULL));
    if (i2c_ok) {
        ESP_ERROR_CHECK(button_bind(KEY1, on_key_red));
        ESP_ERROR_CHECK(button_bind(KEY2, on_key_green));
        ESP_ERROR_CHECK(button_bind(KEY3, on_key_reconnect));
        ESP_ERROR_CHECK(button_bind_long(KEY3, on_key_discover, 1000));
        ESP_ERROR_CHECK(button_bind(KEY1 | KEY4, on_key_serial));
        ESP_ERROR_CHECK(button_bind(KEY4, on_key_splash));
        ESP_ERROR_CHECK(button_start(xl_addr));
    }
    if (config_bt()) {
        bluetooth_reconnect();
    }
}
