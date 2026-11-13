#pragma once

#include <stdint.h>

#include "config.h"

/* Small settings kept in the module Flash NVS. */
class Config {
public:
    Config();

    void load();
    void save();

    uint16_t color() const;
    int font() const;
    int page() const;
    bool backlight() const;
    bool bt() const;
    bool reconnect() const;

    void set_color(uint16_t color);
    void set_font(int width);
    void set_page(int pages);
    void set_backlight(bool on);
    void set_bt(bool on);
    void set_reconnect(bool on);

private:
    struct Blob {
        uint8_t version;
        uint8_t font;
        uint8_t page;
        uint8_t backlight;
        uint8_t bt;
        uint8_t reconnect;
        uint16_t color;
    } __attribute__((packed));

    void use_defaults();
    void sanitize();
    bool changed(const Blob *next) const;

    Blob data_;
    bool loaded_;
};

Config &config_store(void);
