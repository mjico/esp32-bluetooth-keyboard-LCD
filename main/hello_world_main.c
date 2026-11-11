#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_system.h"
#include "bluetooth.h"
#include "button.h"
#include "command_interpreter.h"
#include "debug_port.h"
#include "i2c.h"
#include "lcd_text.h"
#include "trace.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void on_key_red(esp_lcd_panel_handle_t panel)
{
    lcd_text_set_fg_color(panel, 0xF800);
}

static void on_key_green(esp_lcd_panel_handle_t panel)
{
    lcd_text_set_fg_color(panel, 0x07E0);
}

static void on_key_serial(esp_lcd_panel_handle_t panel)
{
    bool on = !trace_serial_enabled();
    (void)panel;
    if (!on && !trace_screen_enabled()) {
        trace_print("trace serial 0");
        trace_set_serial(false);
        return;
    }
    trace_set_serial(on);
    trace_printf("trace serial %d", on ? 1 : 0);
}

static void on_key_reconnect(esp_lcd_panel_handle_t panel)
{
    bluetooth_reconnect(panel);
}

static void on_key_discover(esp_lcd_panel_handle_t panel)
{
    bluetooth_discover(panel);
}

static void on_key_splash(esp_lcd_panel_handle_t panel)
{
    lcd_text_show_splash(panel);
}

static void on_debug_line(const char *line, void *ctx)
{
    char shown[130];
    if (trace_screen_enabled()) {
        snprintf(shown, sizeof(shown), ">%.*s", 127, line != NULL ? line : "");
        trace_print_screen(shown);
    }
    command_line_execute((esp_lcd_panel_handle_t)ctx, line);
}

#define LCD_W LCD_TEXT_WIDTH
#define LCD_H LCD_TEXT_HEIGHT
#define PIN_MOSI 47
#define PIN_SCLK 21
#define PIN_CS 2
#define PIN_DC 3

static esp_lcd_panel_handle_t start_lcd(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_TEXT_CHAR_H * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io));
    ESP_ERROR_CHECK(lcd_text_init(io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    trace_printf("ST7789 ready, %dx%d", LCD_W, LCD_H);
    return panel;
}

void app_main(void)
{
    command_init();
    lcd_text_register_commands();
    trace_register_commands();
    bluetooth_register_commands();
    i2c_register_commands();

    trace_printf("reset reason %d", (int)esp_reset_reason());
    /* Soft reset during RF calibration leaves the modem power domain half-on. */
    bluetooth_rf_reset();

    uint8_t xl_addr = 0;
    bool i2c_ok = i2c_port_start(&xl_addr) == ESP_OK;
    if (i2c_ok && i2c_backlight_on() == ESP_OK) {
        trace_print("backlight IO1_6 set high");
    }
    esp_lcd_panel_handle_t panel = start_lcd();
    trace_bind(panel);

    ESP_ERROR_CHECK(lcd_text_show_splash(panel));
    ESP_ERROR_CHECK(debug_port_start(on_debug_line, panel));
    if (i2c_ok) {
        ESP_ERROR_CHECK(button_bind(KEY1, on_key_red));
        ESP_ERROR_CHECK(button_bind(KEY2, on_key_green));
        ESP_ERROR_CHECK(button_bind(KEY3, on_key_reconnect));
        ESP_ERROR_CHECK(button_bind_long(KEY3, on_key_discover, 1000));
        ESP_ERROR_CHECK(button_bind(KEY1 | KEY4, on_key_serial));
        ESP_ERROR_CHECK(button_bind(KEY4, on_key_splash));
        ESP_ERROR_CHECK(button_start(xl_addr, panel));
    }
}
