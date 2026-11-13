#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "command_interpreter.h"
#include "config.h"
#include "font.h"
#include "lcd_text.h"
#include "trace.h"

#include "esp_random.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define COLOR_BG 0x0000
#define LCD_PAGE_MIN 2
#define LCD_PAGE_MAX 10
#define LCD_PAGE_DEFAULT 3
#define LCD_HIST_MAX ((LCD_PAGE_MAX - 1) * LCD_TEXT_ROWS)

static uint16_t color_fg = 0xFFFF;

static char screen[LCD_TEXT_ROWS][LCD_TEXT_COLS + 1];
static char hist[LCD_HIST_MAX][LCD_TEXT_COLS + 1];
static int hist_count;
static int hist_head;
static int page_keep = LCD_PAGE_DEFAULT;
static int view_page;
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
static esp_err_t paint_row_text(int row, const char *text);
static esp_err_t paint_live(void);

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
    const uint8_t *glyph = font_glyph(ch);

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
    view_page = 0;
    esp_err_t err = lcd_text_write_row_unlocked(row, text);
    lcd_give();
    return err;
}

static int hist_cap(void)
{
    return (page_keep - 1) * LCD_TEXT_ROWS;
}

static int history_pages(void)
{
    if (hist_count <= 0) {
        return 0;
    }
    return (hist_count + LCD_TEXT_ROWS - 1) / LCD_TEXT_ROWS;
}

static char *hist_at(int logical)
{
    return hist[(hist_head + logical) % LCD_HIST_MAX];
}

static void hist_drop_oldest(void)
{
    if (hist_count <= 0) {
        return;
    }
    hist_head = (hist_head + 1) % LCD_HIST_MAX;
    hist_count--;
}

static void hist_trim(void)
{
    int cap = hist_cap();
    while (hist_count > cap) {
        hist_drop_oldest();
    }
}

static void hist_push(const char *text)
{
    int slot;

    hist_trim();
    if (hist_count >= hist_cap()) {
        hist_drop_oldest();
    }
    slot = (hist_head + hist_count) % LCD_HIST_MAX;
    copy_line(hist[slot], text);
    hist_count++;
}

static esp_err_t paint_row_text(int row, const char *text)
{
    static uint16_t band[LCD_TEXT_WIDTH * LCD_TEXT_CHAR_H];
    char local[LCD_TEXT_COLS + 1];

    if (lcd_panel == NULL || row < 0 || row >= LCD_TEXT_ROWS) {
        return ESP_ERR_INVALID_ARG;
    }

    copy_line(local, text);
    int length = (int)strlen(local);
    for (int column = 0; column < LCD_TEXT_COLS; column++) {
        char ch = (column < length) ? local[column] : ' ';
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

static esp_err_t paint_live(void)
{
    esp_err_t err = ESP_OK;
    for (int row = 0; row < LCD_TEXT_ROWS; row++) {
        err = paint_row_text(row, screen[row]);
        if (err != ESP_OK) {
            break;
        }
    }
    return err;
}

static esp_err_t paint_history(int page)
{
    int end = hist_count - (page - 1) * LCD_TEXT_ROWS;
    int begin = end - LCD_TEXT_ROWS;
    int row = 0;
    esp_err_t err = ESP_OK;

    if (begin < 0) {
        begin = 0;
    }
    if (end > hist_count) {
        end = hist_count;
    }
    if (end < begin) {
        end = begin;
    }
    for (int i = begin; i < end && row < LCD_TEXT_ROWS; i++, row++) {
        err = paint_row_text(row, hist_at(i));
        if (err != ESP_OK) {
            return err;
        }
    }
    for (; row < LCD_TEXT_ROWS; row++) {
        err = paint_row_text(row, "");
        if (err != ESP_OK) {
            break;
        }
    }
    return err;
}

static esp_err_t paint_view(void)
{
    if (view_page <= 0) {
        view_page = 0;
        return paint_live();
    }
    return paint_history(view_page);
}

static esp_err_t lcd_text_write_row_unlocked(int row, const char *text)
{
    if (lcd_panel == NULL || row < 0 || row >= LCD_TEXT_ROWS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (text != screen[row]) {
        copy_line(screen[row], text);
    }
    return paint_row_text(row, screen[row]);
}

esp_err_t lcd_text_set_fg_color(uint16_t color)
{
    lcd_take();
    color_fg = color;
    if (lcd_panel == NULL) {
        lcd_give();
        config_set_color(color);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = paint_view();
    lcd_give();
    config_set_color(color);
    return err;
}

void lcd_text_redraw(void)
{
    lcd_take();
    if (lcd_panel == NULL) {
        lcd_give();
        return;
    }
    paint_view();
    lcd_give();
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
    bool show = view_page == 0;

    if (screen_lines < LCD_TEXT_ROWS) {
        if (text != screen[screen_lines]) {
            copy_line(screen[screen_lines], text);
        }
        if (show) {
            err = paint_row_text(screen_lines, screen[screen_lines]);
        }
        if (err == ESP_OK) {
            screen_lines++;
        }
        return err;
    }

    hist_push(screen[0]);
    memmove(screen[0], screen[1], (LCD_TEXT_ROWS - 1) * (LCD_TEXT_COLS + 1));
    copy_line(screen[LCD_TEXT_ROWS - 1], text);
    if (!show) {
        return ESP_OK;
    }
    for (int row = 0; row < LCD_TEXT_ROWS; row++) {
        err = paint_row_text(row, screen[row]);
        if (err != ESP_OK) {
            break;
        }
    }
    return err;
}

static esp_err_t set_draft_unlocked(const char *text, bool reveal)
{
    bool jumped = reveal && view_page != 0;

    if (reveal) {
        view_page = 0;
    }
    if (!draft_on) {
        if (screen_lines >= LCD_TEXT_ROWS) {
            hist_push(screen[0]);
            memmove(screen[0], screen[1], (LCD_TEXT_ROWS - 1) * (LCD_TEXT_COLS + 1));
            screen_lines = LCD_TEXT_ROWS - 1;
            if (reveal && !jumped) {
                for (int row = 0; row < LCD_TEXT_ROWS - 1; row++) {
                    esp_err_t err = paint_row_text(row, screen[row]);
                    if (err != ESP_OK) {
                        return err;
                    }
                }
            }
        }
        draft_row = screen_lines;
        draft_on = true;
    }
    if (text != screen[draft_row]) {
        copy_line(screen[draft_row], text);
    }
    if (!reveal) {
        return ESP_OK;
    }
    if (jumped) {
        return paint_live();
    }
    return paint_row_text(draft_row, screen[draft_row]);
}

esp_err_t lcd_text_set_draft(const char *text)
{
    if (lcd_panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lcd_take();
    esp_err_t err = set_draft_unlocked(text, true);
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
        err = set_draft_unlocked(saved, false);
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

static void blank_live(void)
{
    for (int row = 0; row < LCD_TEXT_ROWS; row++) {
        copy_line(screen[row], "");
    }
    screen_lines = 0;
    draft_on = false;
    view_page = 0;
}

static bool parse_page_count(const char *text, int *out)
{
    int n = 0;
    int digits = 0;

    if (text == NULL || out == NULL) {
        return false;
    }
    while (text[digits] != '\0') {
        if (text[digits] < '0' || text[digits] > '9' || digits >= 2) {
            return false;
        }
        n = n * 10 + (text[digits] - '0');
        digits++;
    }
    if (digits == 0 || n < LCD_PAGE_MIN || n > LCD_PAGE_MAX) {
        return false;
    }
    *out = n;
    return true;
}

int lcd_text_page_count(void)
{
    int n;
    lcd_take();
    n = page_keep;
    lcd_give();
    return n;
}

void lcd_text_set_page_count(int pages)
{
    if (pages < LCD_PAGE_MIN || pages > LCD_PAGE_MAX) {
        return;
    }
    lcd_take();
    page_keep = pages;
    hist_trim();
    if (view_page > history_pages()) {
        view_page = history_pages();
    }
    paint_view();
    lcd_give();
    config_set_page(pages);
}

void lcd_text_page_up(void)
{
    lcd_take();
    if (lcd_panel != NULL && view_page < history_pages()) {
        view_page++;
        paint_history(view_page);
    }
    lcd_give();
}

void lcd_text_page_down(void)
{
    lcd_take();
    if (lcd_panel != NULL && view_page > 0) {
        view_page--;
        paint_view();
    }
    lcd_give();
}

void lcd_text_page_status(int *shown, int *total)
{
    lcd_take();
    if (shown != NULL) {
        *shown = view_page + 1;
    }
    if (total != NULL) {
        *total = history_pages() + 1;
    }
    lcd_give();
}

void lcd_text_clear(bool history)
{
    lcd_take();
    if (history) {
        hist_count = 0;
        hist_head = 0;
    }
    blank_live();
    paint_live();
    lcd_give();
}

void lcd_text_command_page(int argc, char **argv)
{
    int pages = 0;

    if (argc < 1) {
        trace_printf("page %d", lcd_text_page_count());
        trace_print("system page [2-10]");
        return;
    }
    if (!parse_page_count(argv[0], &pages)) {
        trace_print("system page [2-10]");
        return;
    }
    lcd_text_set_page_count(pages);
    trace_printf("page %d", lcd_text_page_count());
}

static void cmd_page(int argc, char **argv)
{
    int shown = 1;
    int total = 1;

    if (argc < 2) {
        lcd_text_page_status(&shown, &total);
        trace_printf("page %d/%d", shown, total);
        trace_print("page up|down");
        return;
    }
    if (same_word(argv[1], "up")) {
        lcd_text_page_up();
        return;
    }
    if (same_word(argv[1], "down")) {
        lcd_text_page_down();
        return;
    }
    trace_print("page up|down");
}

static void cmd_clear(int argc, char **argv)
{
    if (argc < 2) {
        lcd_text_clear(false);
        trace_print("clear [history]");
        return;
    }
    if (!same_word(argv[1], "history")) {
        trace_print("clear [history]");
        return;
    }
    lcd_text_clear(true);
    trace_print("history cleared");
}

static void cmd_color(int argc, char **argv)
{
    uint16_t color = 0;
    if (argc < 1 || !parse_color(argv[0], &color)) {
        trace_print("system color <name|hex>");
        return;
    }
    lcd_text_set_fg_color(color);
}

void lcd_text_command_color(int argc, char **argv)
{
    cmd_color(argc, argv);
}

static void cmd_splash(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lcd_text_show_splash();
}

void lcd_text_register_commands(void)
{
    command_register("splash", "show boot picture", cmd_splash);
    command_register("page", "up|down", cmd_page);
    command_register("clear", "clear [history]", cmd_clear);
}
