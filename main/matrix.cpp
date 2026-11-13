#include "matrix.hpp"

#include <ctype.h>
#include <string.h>

#include "command_interpreter.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"
#include "matrix.h"
#include "trace.h"

static const uint8_t kImages[Matrix::kImageCount][8] = {
    {0x3C, 0x42, 0xA5, 0x81, 0xA5, 0x99, 0x42, 0x3C}, /* smile */
    {0x3C, 0x42, 0xA5, 0x81, 0x99, 0xA5, 0x42, 0x3C}, /* sad */
    {0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00}, /* heart */
    {0x18, 0x3C, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x00}, /* up */
    {0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF}, /* box */
    {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81}, /* cross */
    {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55}, /* check */
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, /* fill */
};

static const char *const kImageNames[Matrix::kImageCount] = {
    "smile", "sad", "heart", "up", "box", "cross", "check", "fill",
};

static void rotate8(const uint8_t src[8], uint8_t dst[8], int direction)
{
    memset(dst, 0, 8);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            if ((src[y] & (uint8_t)(0x80 >> x)) == 0) {
                continue;
            }
            int nx = x;
            int ny = y;
            if (direction == 2) {
                nx = 7 - y;
                ny = x;
            } else if (direction == 3) {
                nx = 7 - x;
                ny = 7 - y;
            } else if (direction == 4) {
                nx = y;
                ny = 7 - x;
            }
            dst[ny] |= (uint8_t)(0x80 >> nx);
        }
    }
}

esp_err_t Matrix::set_brightness(uint8_t level)
{
    return i2c_reg_write(kAddr, kRegBrightness, &level, 1);
}

esp_err_t Matrix::write_frame()
{
    uint8_t blank[16] = {};
    esp_err_t err = i2c_reg_write(kAddr, kRegStatic, blank, sizeof(blank));
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    return i2c_reg_write(kAddr, kRegStatic, frame_, sizeof(frame_));
}

esp_err_t Matrix::clear()
{
    memset(frame_, 0, sizeof(frame_));
    return write_frame();
}

esp_err_t Matrix::show_raw(const uint8_t data[16])
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(frame_, data, sizeof(frame_));
    return write_frame();
}

esp_err_t Matrix::scroll(const uint8_t *data, size_t len)
{
    uint8_t blank[16] = {};
    if (data == NULL || len == 0 || len > 16) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_reg_write(kAddr, kRegStatic, blank, sizeof(blank));
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    return i2c_reg_write(kAddr, kRegScroll, data, len);
}

esp_err_t Matrix::show_image(int screen, int image, int direction)
{
    uint8_t turned[8];
    if (image < 0 || image >= kImageCount || direction < 1 || direction > 4) {
        return ESP_ERR_INVALID_ARG;
    }
    if (screen != kScreen1 && screen != kScreen2 && screen != kScreenBoth) {
        return ESP_ERR_INVALID_ARG;
    }
    rotate8(kImages[image], turned, direction);
    if (screen == kScreen1 || screen == kScreenBoth) {
        memcpy(frame_, turned, 8);
    }
    if (screen == kScreen2 || screen == kScreenBoth) {
        memcpy(frame_ + 8, turned, 8);
    }
    return write_frame();
}

static bool put_glyph(uint8_t frame[16], char ch)
{
    const uint8_t *glyph;
    if (ch < 32 || ch > 126) {
        return false;
    }
    glyph = font_glyph(ch);
    /* Shift one pixel right so the glyph clears the left edge. */
    for (int i = 0; i < 16; i++) {
        frame[i] = (uint8_t)(glyph[i] >> 1);
    }
    return true;
}

esp_err_t Matrix::show_char(char ch)
{
    if (!put_glyph(frame_, ch)) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_frame();
}

esp_err_t Matrix::show_char_fast(char ch)
{
    if (!put_glyph(frame_, ch)) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_reg_write(kAddr, kRegStatic, frame_, sizeof(frame_));
}

Matrix &matrix_dev(void)
{
    static Matrix dev;
    return dev;
}

static bool key_echo;

extern "C" void matrix_on_key(char ch)
{
    if (!key_echo) {
        return;
    }
    (void)matrix_dev().show_char_fast(ch);
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

static bool parse_u8(const char *text, int *out)
{
    int base = 10;
    int value = 0;

    if (text == NULL || text[0] == '\0' || out == NULL) {
        return false;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
        if (text[0] == '\0') {
            return false;
        }
    }
    for (const char *p = text; *p != '\0'; p++) {
        int digit;
        if (*p >= '0' && *p <= '9') {
            digit = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            digit = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            digit = *p - 'A' + 10;
        } else {
            return false;
        }
        if (digit >= base) {
            return false;
        }
        value = value * base + digit;
        if (value > 255) {
            return false;
        }
    }
    *out = value;
    return true;
}

static int image_index(const char *name)
{
    for (int i = 0; i < Matrix::kImageCount; i++) {
        if (same_word(name, kImageNames[i])) {
            return i;
        }
    }
    return -1;
}

static void cmd_matrix(int argc, char **argv)
{
    if (argc < 2) {
        trace_print("matrix key|bright|clear|image|char|scroll|raw");
        return;
    }
    if (same_word(argv[1], "key")) {
        if (argc < 3) {
            trace_printf("matrix key %d", key_echo ? 1 : 0);
            trace_print("matrix key [0|1]");
            return;
        }
        if ((argv[2][0] != '0' && argv[2][0] != '1') || argv[2][1] != '\0') {
            trace_print("matrix key [0|1]");
            return;
        }
        key_echo = argv[2][0] == '1';
        trace_printf("matrix key %d", key_echo ? 1 : 0);
        return;
    }
    if (same_word(argv[1], "bright")) {
        int level = 0;
        if (argc < 3 || !parse_u8(argv[2], &level)) {
            trace_print("matrix bright <88-8F>");
            return;
        }
        esp_err_t err = matrix_dev().set_brightness((uint8_t)level);
        if (err != ESP_OK) {
            trace_printf("matrix bright %s", esp_err_to_name(err));
            return;
        }
        trace_print("matrix bright");
        return;
    }
    if (same_word(argv[1], "clear")) {
        esp_err_t err = matrix_dev().clear();
        if (err != ESP_OK) {
            trace_printf("matrix clear %s", esp_err_to_name(err));
            return;
        }
        trace_print("matrix clear");
        return;
    }
    if (same_word(argv[1], "image")) {
        int screen = Matrix::kScreen1;
        int direction = 1;
        int image;
        if (argc < 4) {
            trace_print("matrix image <1|2|both> <name> [1-4]");
            return;
        }
        if (same_word(argv[2], "both")) {
            screen = Matrix::kScreenBoth;
        } else if (same_word(argv[2], "1")) {
            screen = Matrix::kScreen1;
        } else if (same_word(argv[2], "2")) {
            screen = Matrix::kScreen2;
        } else {
            trace_print("matrix image <1|2|both> <name> [1-4]");
            return;
        }
        image = image_index(argv[3]);
        if (image < 0) {
            trace_print("image smile|sad|heart|up|box|cross|check|fill");
            return;
        }
        if (argc >= 5) {
            int parsed = 0;
            if (!parse_u8(argv[4], &parsed) || parsed < 1 || parsed > 4) {
                trace_print("matrix image <1|2|both> <name> [1-4]");
                return;
            }
            direction = parsed;
        }
        esp_err_t err = matrix_dev().show_image(screen, image, direction);
        if (err != ESP_OK) {
            trace_printf("matrix image %s", esp_err_to_name(err));
            return;
        }
        trace_print("matrix image");
        return;
    }
    if (same_word(argv[1], "char")) {
        if (argc < 3 || argv[2][0] == '\0' || argv[2][1] != '\0') {
            trace_print("matrix char <c>");
            return;
        }
        esp_err_t err = matrix_dev().show_char(argv[2][0]);
        if (err != ESP_OK) {
            trace_printf("matrix char %s", esp_err_to_name(err));
            return;
        }
        trace_print("matrix char");
        return;
    }
    if (same_word(argv[1], "scroll") || same_word(argv[1], "raw")) {
        uint8_t data[16];
        int len = 0;
        bool raw = same_word(argv[1], "raw");
        for (int i = 2; i < argc && len < 16; i++) {
            int byte = 0;
            if (!parse_u8(argv[i], &byte)) {
                trace_print(raw ? "matrix raw <16 bytes>" : "matrix scroll <bytes>");
                return;
            }
            data[len++] = (uint8_t)byte;
        }
        if (len == 0 || (raw && len != 16)) {
            trace_print(raw ? "matrix raw <16 bytes>" : "matrix scroll <bytes>");
            return;
        }
        esp_err_t err = raw ? matrix_dev().show_raw(data) : matrix_dev().scroll(data, (size_t)len);
        if (err != ESP_OK) {
            trace_printf("matrix %s %s", argv[1], esp_err_to_name(err));
            return;
        }
        trace_print(raw ? "matrix raw" : "matrix scroll");
        return;
    }
    trace_print("matrix key|bright|clear|image|char|scroll|raw");
}

extern "C" void matrix_register_commands(void)
{
    command_register("matrix", "key bright image char raw", cmd_matrix);
}
