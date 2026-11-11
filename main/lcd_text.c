#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "command_interpreter.h"
#include "lcd_text.h"
#include "trace.h"

#include "esp_random.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "font8x16.inc"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define COLOR_BG 0x0000

static uint16_t color_fg = 0xFFFF;

static char screen[LCD_TEXT_ROWS][LCD_TEXT_COLS + 1];
static int screen_lines;
static bool draft_on;
static int draft_row;
static esp_lcd_panel_handle_t lcd_panel;
static esp_lcd_panel_io_handle_t panel_io;
static SemaphoreHandle_t lcd_mu;

static void lcd_take(void)
{
    if (lcd_mu != NULL) {
        xSemaphoreTakeRecursive(lcd_mu, portMAX_DELAY);
    }
}

static void lcd_give(void)
{
    if (lcd_mu != NULL) {
        xSemaphoreGiveRecursive(lcd_mu);
    }
}

static esp_err_t lcd_text_write_row_unlocked(int row, const char *text);

static void copy_line(char *dst, const char *text)
{
    memset(dst, 0, LCD_TEXT_COLS + 1);
    if (text == NULL) {
        return;
    }
    int length = 0;
    while (text[length] != '\0' && length < LCD_TEXT_COLS) {
        dst[length] = text[length];
        length++;
    }
}

static void draw_glyph(uint16_t *band, int column, char ch)
{
    const uint8_t *glyph = font8x16[0];
    if (ch >= 32 && ch <= 126) {
        glyph = font8x16[ch - 32];
    }

    for (int py = 0; py < LCD_TEXT_CHAR_H; py++) {
        uint8_t bits = glyph[py];
        uint16_t *row = band + py * LCD_TEXT_WIDTH + column * LCD_TEXT_CHAR_W;
        for (int px = 0; px < LCD_TEXT_CHAR_W; px++) {
            row[px] = (bits & (0x80 >> px)) ? color_fg : COLOR_BG;
        }
    }
}

static esp_err_t lcd_text_init(esp_lcd_panel_io_handle_t io)
{
    panel_io = io;
    if (lcd_mu == NULL) {
        lcd_mu = xSemaphoreCreateRecursiveMutex();
        if (lcd_mu == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

bool lcd_text_ready(void)
{
    return lcd_panel != NULL;
}

esp_err_t lcd_text_start(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = 21,
        .mosi_io_num = 47,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_TEXT_WIDTH * LCD_TEXT_CHAR_H * (int)sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = 3,
        .cs_gpio_num = 2,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io);
    if (err != ESP_OK) {
        return err;
    }
    err = lcd_text_init(io);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &panel_config, &panel);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    trace_printf("ST7789 ready, %dx%d", LCD_TEXT_WIDTH, LCD_TEXT_HEIGHT);
    lcd_panel = panel;
    return ESP_OK;
}

esp_err_t lcd_text_write_row(int row, const char *text)
{
    lcd_take();
    esp_err_t err = lcd_text_write_row_unlocked(row, text);
    lcd_give();
    return err;
}

static esp_err_t lcd_text_write_row_unlocked(int row, const char *text)
{
    static uint16_t band[LCD_TEXT_WIDTH * LCD_TEXT_CHAR_H];

    if (lcd_panel == NULL || row < 0 || row >= LCD_TEXT_ROWS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (text != screen[row]) {
        copy_line(screen[row], text);
    }
    int length = (int)strlen(screen[row]);

    for (int column = 0; column < LCD_TEXT_COLS; column++) {
        char ch = (column < length) ? screen[row][column] : ' ';
        draw_glyph(band, column, ch);
    }

    int y = row * LCD_TEXT_CHAR_H;
    esp_err_t err = esp_lcd_panel_draw_bitmap(lcd_panel, 0, y, LCD_TEXT_WIDTH, y + LCD_TEXT_CHAR_H, band);
    if (err != ESP_OK || panel_io == NULL) {
        return err;
    }
    /* tx_param waits until the queued bitmap DMA finishes, so the next row can reuse band. */
    return esp_lcd_panel_io_tx_param(panel_io, 0x00, NULL, 0);
}

esp_err_t lcd_text_set_fg_color(uint16_t color)
{
    lcd_take();
    color_fg = color;
    if (lcd_panel == NULL) {
        lcd_give();
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    for (int row = 0; row < LCD_TEXT_ROWS; row++) {
        err = lcd_text_write_row_unlocked(row, screen[row]);
        if (err != ESP_OK) {
            break;
        }
    }
    lcd_give();
    return err;
}

void lcd_text_set_screen_lines(int screen_line)
{
    lcd_take();
    draft_on = false;
    if (screen_line < 0) {
        screen_lines = 0;
    } else if (screen_line > LCD_TEXT_ROWS) {
        screen_lines = LCD_TEXT_ROWS;
    } else {
        screen_lines = screen_line;
    }
    lcd_give();
}

static esp_err_t append_line_unlocked(const char *text)
{
    esp_err_t err = ESP_OK;
    if (screen_lines < LCD_TEXT_ROWS) {
        err = lcd_text_write_row_unlocked(screen_lines, text);
        if (err == ESP_OK) {
            screen_lines++;
        }
        return err;
    }

    memmove(screen[0], screen[1], (LCD_TEXT_ROWS - 1) * (LCD_TEXT_COLS + 1));
    copy_line(screen[LCD_TEXT_ROWS - 1], text);
    for (int row = 0; row < LCD_TEXT_ROWS; row++) {
        err = lcd_text_write_row_unlocked(row, screen[row]);
        if (err != ESP_OK) {
            break;
        }
    }
    return err;
}

static esp_err_t set_draft_unlocked(const char *text)
{
    if (!draft_on) {
        if (screen_lines >= LCD_TEXT_ROWS) {
            memmove(screen[0], screen[1], (LCD_TEXT_ROWS - 1) * (LCD_TEXT_COLS + 1));
            screen_lines = LCD_TEXT_ROWS - 1;
            for (int row = 0; row < LCD_TEXT_ROWS - 1; row++) {
                esp_err_t err = lcd_text_write_row_unlocked(row, screen[row]);
                if (err != ESP_OK) {
                    return err;
                }
            }
        }
        draft_row = screen_lines;
        draft_on = true;
    }
    return lcd_text_write_row_unlocked(draft_row, text);
}

esp_err_t lcd_text_set_draft(const char *text)
{
    if (lcd_panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lcd_take();
    esp_err_t err = set_draft_unlocked(text);
    lcd_give();
    return err;
}

void lcd_text_dismiss_draft(void)
{
    lcd_take();
    draft_on = false;
    lcd_give();
}

esp_err_t lcd_text_append_line(const char *text)
{
    char saved[LCD_TEXT_COLS + 1];
    bool restore = false;

    if (lcd_panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lcd_take();
    if (draft_on) {
        memcpy(saved, screen[draft_row], sizeof(saved));
        draft_on = false;
        restore = true;
    }
    esp_err_t err = append_line_unlocked(text);
    if (restore && err == ESP_OK) {
        err = set_draft_unlocked(saved);
    }
    lcd_give();
    return err;
}

esp_err_t lcd_text_show_splash(void)
{
    static const char *art[] = {
		"",
        ".--------------------------------------.",
        "| .----------------------------------. |",
        "| |                                  | |",
        "| |             Hello World!         | |",
        "| |                                  | |",
        "| |    /\\_/\\                         | |",
        "| |   ( o.o )  <Meow System Lunched! | |",
        "| |    > ^ <                         | |",
        "| |                                  | |",
        "| |           Debug Port is ready!   | |",
        "| \\__________________________________/ |",
        "\\______________________________________/",
        "              ||        ||              ",
        "              ||________||              ",
        "             /____________\\             ",
    };
    static const char *art2[] = {
        "",
        ".--------------------------------------.",
        "| .----------------------------------. |",
        "| |                                  | |",
        "| |             Hello World!         | |",
        "| |                                  | |",
        "| |         <Meow System Lunched!    | |",
        "| |         /)                       | |",
        "| | /\\___/\\((                        | |",
        "| | \\`@_@'/ ))                       | |",
        "| | {_:Y:.}//     Fun is ready!      | |",
        "| \\_{_}~{_}__________________________/ |",
        "\\______________________________________/",
        "              ||        ||              ",
        "              ||________||              ",
        "             /____________\\             ",
    };
    static const char *const *pictures[] = { art, art2 };
    static const size_t picture_rows[] = {
        sizeof(art) / sizeof(art[0]),
        sizeof(art2) / sizeof(art2[0]),
    };
    size_t which = (size_t)(esp_random() % 2);

    for (size_t row = 0; row < picture_rows[which]; row++) {
        trace_print(pictures[which][row]);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return ESP_OK;
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
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

static bool parse_color(const char *text, uint16_t *color)
{
    static const struct {
        const char *name;
        uint16_t value;
    } names[] = {
        {"black", 0x0000},
        {"white", 0xFFFF},
        {"red", 0xF800},
        {"green", 0x07E0},
        {"blue", 0x001F},
        {"yellow", 0xFFE0},
        {"cyan", 0x07FF},
        {"magenta", 0xF81F},
    };

    if (text == NULL || color == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (same_word(text, names[i].name)) {
            *color = names[i].value;
            return true;
        }
    }

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }

    uint32_t value = 0;
    int digits = 0;
    while (text[digits] != '\0') {
        int nibble = hex_digit(text[digits]);
        if (nibble < 0 || digits >= 6) {
            return false;
        }
        value = (value << 4) | (uint32_t)nibble;
        digits++;
    }
    if (digits == 4) {
        *color = (uint16_t)value;
        return true;
    }
    if (digits == 6) {
        *color = (uint16_t)(((value >> 8) & 0xF800) | ((value >> 5) & 0x07E0) | ((value >> 3) & 0x001F));
        return true;
    }
    return false;
}

static void cmd_color(int argc, char **argv)
{
    uint16_t color = 0;
    if (argc < 2 || !parse_color(argv[1], &color)) {
        trace_print("color <name|hex>");
        return;
    }
    lcd_text_set_fg_color(color);
}

static void cmd_splash(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lcd_text_show_splash();
}

void lcd_text_register_commands(void)
{
    command_register("color", "set text color", cmd_color);
    command_register("splash", "show boot picture", cmd_splash);
}
