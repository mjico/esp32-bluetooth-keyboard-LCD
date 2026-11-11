#include "i2c.hpp"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "command_interpreter.h"
#include "esp_err.h"
#include "lcd_text.h"
#include "trace.h"

static const int kPairs[][2] = {
    {4, 5},
    {38, 48},
};

static const uint8_t kBlOutput = 0x03;
static const uint8_t kBlConfig = 0x07;
static const uint8_t kBlBit = 6;

I2c::I2c()
    : bus_(NULL), xl_addr_(0), dev_count_(0), mu_(NULL)
{
    memset(devs_, 0, sizeof(devs_));
    memset(dev_addr_, 0, sizeof(dev_addr_));
}

void I2c::lock()
{
    if (mu_ == NULL) {
        mu_ = xSemaphoreCreateMutex();
    }
    if (mu_ != NULL) {
        xSemaphoreTake(mu_, portMAX_DELAY);
    }
}

void I2c::unlock()
{
    if (mu_ != NULL) {
        xSemaphoreGive(mu_);
    }
}

i2c_master_dev_handle_t I2c::device(uint8_t addr, esp_err_t *err)
{
    for (int i = 0; i < dev_count_; i++) {
        if (dev_addr_[i] == addr) {
            *err = ESP_OK;
            return devs_[i];
        }
    }
    if (bus_ == NULL) {
        *err = ESP_ERR_INVALID_STATE;
        return NULL;
    }
    if (dev_count_ >= kMaxDevices) {
        *err = ESP_ERR_NO_MEM;
        return NULL;
    }

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = addr;
    cfg.scl_speed_hz = kSpeedHz;
    i2c_master_dev_handle_t dev = NULL;
    *err = i2c_master_bus_add_device(bus_, &cfg, &dev);
    if (*err != ESP_OK) {
        return NULL;
    }
    devs_[dev_count_] = dev;
    dev_addr_[dev_count_] = addr;
    dev_count_++;
    return dev;
}

esp_err_t I2c::start(uint8_t *xl_addr)
{
    if (xl_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *xl_addr = 0;

    lock();
    if (bus_ != NULL) {
        *xl_addr = xl_addr_;
        unlock();
        return xl_addr_ != 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    for (size_t i = 0; i < sizeof(kPairs) / sizeof(kPairs[0]); i++) {
        i2c_master_bus_config_t bus_config = {};
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.i2c_port = (i == 0) ? I2C_NUM_0 : I2C_NUM_1;
        bus_config.sda_io_num = (gpio_num_t)kPairs[i][0];
        bus_config.scl_io_num = (gpio_num_t)kPairs[i][1];
        bus_config.flags.enable_internal_pullup = true;

        i2c_master_bus_handle_t bus = NULL;
        esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
        if (err != ESP_OK) {
            unlock();
            trace_printf("I2C SDA=%d SCL=%d %s", kPairs[i][0], kPairs[i][1], esp_err_to_name(err));
            lock();
            continue;
        }
        for (uint8_t addr = 0x20; addr <= 0x27; addr++) {
            if (i2c_master_probe(bus, addr, 30) != ESP_OK) {
                continue;
            }
            bus_ = bus;
            xl_addr_ = addr;
            *xl_addr = addr;
            unlock();
            trace_printf("XL9555 0x%02X on SDA=%d SCL=%d", addr, kPairs[i][0], kPairs[i][1]);
            return ESP_OK;
        }
        i2c_del_master_bus(bus);
    }
    unlock();
    trace_print("XL9555 not found");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t I2c::backlight_on()
{
    if (xl_addr_ == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t out = 0;
    uint8_t dir = 0xFF;
    esp_err_t err;
    read(xl_addr_, kBlOutput, &out, 1);
    read(xl_addr_, kBlConfig, &dir, 1);
    out = (uint8_t)(out | (1u << kBlBit));
    dir = (uint8_t)(dir & ~(1u << kBlBit));
    err = write(xl_addr_, kBlOutput, &out, 1);
    if (err != ESP_OK) {
        return err;
    }
    return write(xl_addr_, kBlConfig, &dir, 1);
}

esp_err_t I2c::probe(uint8_t addr)
{
    lock();
    if (bus_ == NULL) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = i2c_master_probe(bus_, addr, 20);
    unlock();
    return err;
}

esp_err_t I2c::scan(uint8_t *addrs, int *count, int max_count)
{
    int found = 0;
    int timeouts = 0;

    if (count == NULL || max_count < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    lock();
    if (bus_ == NULL) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    for (int addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t err = i2c_master_probe(bus_, (uint16_t)addr, 20);
        if (err == ESP_ERR_TIMEOUT) {
            if (++timeouts >= 3) {
                *count = found;
                unlock();
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        timeouts = 0;
        if (err != ESP_OK) {
            continue;
        }
        if (addrs != NULL && found < max_count) {
            addrs[found] = (uint8_t)addr;
        }
        found++;
        if (found >= max_count) {
            break;
        }
    }
    *count = found;
    unlock();
    return ESP_OK;
}

esp_err_t I2c::read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > (size_t)kMaxBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = ESP_OK;
    i2c_master_dev_handle_t dev = device(addr, &err);
    if (dev == NULL) {
        unlock();
        return err;
    }
    err = i2c_master_transmit_receive(dev, &reg, 1, data, len, 50);
    unlock();
    return err;
}

esp_err_t I2c::write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[1 + kMaxBytes];

    if (data == NULL || len == 0 || len > (size_t)kMaxBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = reg;
    memcpy(buf + 1, data, len);

    lock();
    esp_err_t err = ESP_OK;
    i2c_master_dev_handle_t dev = device(addr, &err);
    if (dev == NULL) {
        unlock();
        return err;
    }
    err = i2c_master_transmit(dev, buf, len + 1, 50);
    unlock();
    return err;
}

I2c &i2c_port(void)
{
    static I2c port;
    return port;
}

extern "C" esp_err_t i2c_port_start(uint8_t *xl_addr)
{
    return i2c_port().start(xl_addr);
}

extern "C" esp_err_t i2c_backlight_on(void)
{
    return i2c_port().backlight_on();
}

extern "C" esp_err_t i2c_probe(uint8_t addr)
{
    return i2c_port().probe(addr);
}

extern "C" esp_err_t i2c_scan(uint8_t *addrs, int *count, int max_count)
{
    return i2c_port().scan(addrs, count, max_count);
}

extern "C" esp_err_t i2c_reg_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_port().read(addr, reg, data, len);
}

extern "C" esp_err_t i2c_reg_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len)
{
    return i2c_port().write(addr, reg, data, len);
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
        int digit = hex_digit(*p);
        if (digit < 0 || digit >= base) {
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

static void cmd_i2c_scan(void)
{
    uint8_t addrs[16];
    int count = 0;
    char line[LCD_TEXT_COLS + 1];
    int used = 0;
    esp_err_t err = i2c_scan(addrs, &count, (int)(sizeof(addrs) / sizeof(addrs[0])));

    if (err == ESP_ERR_INVALID_STATE || (err == ESP_OK && count == 0)) {
        trace_print("i2c none");
        return;
    }
    if (err == ESP_ERR_TIMEOUT && count == 0) {
        trace_print("i2c timeout");
        return;
    }

    memcpy(line, "i2c", 4);
    used = 3;
    for (int i = 0; i < count; i++) {
        char piece[8];
        snprintf(piece, sizeof(piece), " 0x%02X", addrs[i]);
        if (used + (int)strlen(piece) >= LCD_TEXT_COLS) {
            trace_print(line);
            used = 0;
            line[0] = '\0';
        }
        memcpy(line + used, piece, strlen(piece) + 1);
        used += (int)strlen(piece);
    }
    if (used > 0) {
        trace_print(line);
    }
    if (err == ESP_ERR_TIMEOUT) {
        trace_print("i2c timeout");
    }
}

static void cmd_i2c_read(int argc, char **argv)
{
    int addr = 0;
    int reg = 0;
    int len = 1;
    uint8_t data[16];
    char line[LCD_TEXT_COLS + 1];
    int used = 0;

    if (argc < 4 || !parse_u8(argv[2], &addr) || !parse_u8(argv[3], &reg)) {
        trace_print("i2c read <addr> <reg> [n]");
        return;
    }
    if (argc >= 5) {
        if (!parse_u8(argv[4], &len) || len < 1 || len > (int)(sizeof(data))) {
            trace_print("i2c read <addr> <reg> [n]");
            return;
        }
    }
    esp_err_t err = i2c_reg_read((uint8_t)addr, (uint8_t)reg, data, (size_t)len);
    if (err != ESP_OK) {
        trace_printf("i2c read %s", esp_err_to_name(err));
        return;
    }
    used = snprintf(line, sizeof(line), "read %02X %02X:", addr, reg);
    for (int i = 0; i < len && used > 0 && used < LCD_TEXT_COLS; i++) {
        used += snprintf(line + used, sizeof(line) - (size_t)used, " %02X", data[i]);
    }
    trace_print(line);
}

static void cmd_i2c_write(int argc, char **argv)
{
    int addr = 0;
    int reg = 0;
    uint8_t data[16];
    int len = 0;
    char line[LCD_TEXT_COLS + 1];
    int used = 0;

    if (argc < 5 || !parse_u8(argv[2], &addr) || !parse_u8(argv[3], &reg)) {
        trace_print("i2c write <addr> <reg> <bytes>");
        return;
    }
    for (int i = 4; i < argc && len < (int)sizeof(data); i++) {
        int byte = 0;
        if (!parse_u8(argv[i], &byte)) {
            trace_print("i2c write <addr> <reg> <bytes>");
            return;
        }
        data[len++] = (uint8_t)byte;
    }
    esp_err_t err = i2c_reg_write((uint8_t)addr, (uint8_t)reg, data, (size_t)len);
    if (err != ESP_OK) {
        trace_printf("i2c write %s", esp_err_to_name(err));
        return;
    }
    used = snprintf(line, sizeof(line), "write %02X %02X:", addr, reg);
    for (int i = 0; i < len && used > 0 && used < LCD_TEXT_COLS; i++) {
        used += snprintf(line + used, sizeof(line) - (size_t)used, " %02X", data[i]);
    }
    trace_print(line);
}

static void cmd_i2c(int argc, char **argv)
{
    if (argc < 2 || same_word(argv[1], "scan")) {
        cmd_i2c_scan();
        return;
    }
    if (same_word(argv[1], "read")) {
        cmd_i2c_read(argc, argv);
        return;
    }
    if (same_word(argv[1], "write")) {
        cmd_i2c_write(argc, argv);
        return;
    }
    trace_print("i2c scan|read|write");
}

extern "C" void i2c_register_commands(void)
{
    command_register("i2c", "scan or read/write", cmd_i2c);
}
