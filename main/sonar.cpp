#include "sonar.hpp"

#include <ctype.h>
#include <stdio.h>

#include "command_interpreter.h"
#include "i2c.h"
#include "sonar.h"
#include "trace.h"

esp_err_t Sonar::distance(uint16_t *raw)
{
    uint8_t high = 0;
    uint8_t low = 0;
    if (raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_reg_read(kAddr, kRegDistanceH, &high, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_reg_read(kAddr, kRegDistanceL, &low, 1);
    if (err != ESP_OK) {
        return err;
    }
    *raw = (uint16_t)high * 256u + low;
    return ESP_OK;
}

esp_err_t Sonar::set_mode(uint8_t mode)
{
    if (mode != kModeSimple && mode != kModeBreath) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_reg_write(kAddr, kRegMode, &mode, 1);
}

esp_err_t Sonar::set_color(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2)
{
    uint8_t rgb[6] = {r1, g1, b1, r2, g2, b2};
    esp_err_t err = set_mode(kModeSimple);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_reg_write(kAddr, kRegRgb, rgb, sizeof(rgb));
}

esp_err_t Sonar::set_breath(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2)
{
    uint8_t cycle[6] = {r1, g1, b1, r2, g2, b2};
    esp_err_t err = set_mode(kModeBreath);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_reg_write(kAddr, kRegBreath, cycle, sizeof(cycle));
}

Sonar &sonar_dev(void)
{
    static Sonar dev;
    return dev;
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

static bool parse_six(int argc, char **argv, uint8_t out[6])
{
    if (argc < 8) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        int value = 0;
        if (!parse_u8(argv[2 + i], &value)) {
            return false;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}

static void cmd_sonar(int argc, char **argv)
{
    if (argc < 2) {
        uint16_t raw = 0;
        esp_err_t err = sonar_dev().distance(&raw);
        if (err != ESP_OK) {
            trace_printf("sonar %s", esp_err_to_name(err));
            return;
        }
        trace_printf("sonar %u.%u cm", (unsigned)(raw / 10), (unsigned)(raw % 10));
        trace_print("sonar [color|breath]");
        return;
    }
    if (same_word(argv[1], "color")) {
        uint8_t rgb[6];
        if (!parse_six(argc, argv, rgb)) {
            trace_print("sonar color <r1 g1 b1 r2 g2 b2>");
            return;
        }
        esp_err_t err = sonar_dev().set_color(rgb[0], rgb[1], rgb[2], rgb[3], rgb[4], rgb[5]);
        if (err != ESP_OK) {
            trace_printf("sonar color %s", esp_err_to_name(err));
            return;
        }
        trace_print("sonar color");
        return;
    }
    if (same_word(argv[1], "breath")) {
        uint8_t cycle[6];
        if (!parse_six(argc, argv, cycle)) {
            trace_print("sonar breath <r1 g1 b1 r2 g2 b2>");
            return;
        }
        esp_err_t err = sonar_dev().set_breath(cycle[0], cycle[1], cycle[2], cycle[3], cycle[4], cycle[5]);
        if (err != ESP_OK) {
            trace_printf("sonar breath %s", esp_err_to_name(err));
            return;
        }
        trace_print("sonar breath");
        return;
    }
    trace_print("sonar [color|breath]");
}

extern "C" void sonar_register_commands(void)
{
    command_register("sonar", "read cm or set rgb", cmd_sonar);
}
