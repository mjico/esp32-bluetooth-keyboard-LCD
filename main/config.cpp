#include "config.hpp"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define CFG_NS "cfg"
#define CFG_KEY "blob"
#define CFG_VERSION 1

static bool nvs_ready(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK) {
            return false;
        }
        err = nvs_flash_init();
    }
    return err == ESP_OK;
}

Config::Config() : loaded_(false)
{
    use_defaults();
}

void Config::use_defaults()
{
    data_.version = CFG_VERSION;
    data_.font = 5;
    data_.page = 3;
    data_.backlight = 1;
    data_.bt = 1;
    data_.reconnect = 0;
    data_.color = 0xFFFF;
}

void Config::sanitize()
{
    if (data_.version != CFG_VERSION) {
        use_defaults();
        return;
    }
    if (data_.font != 6) {
        data_.font = 5;
    }
    if (data_.page < 2 || data_.page > 10) {
        data_.page = 3;
    }
    data_.backlight = data_.backlight ? 1 : 0;
    data_.bt = data_.bt ? 1 : 0;
    data_.reconnect = data_.reconnect ? 1 : 0;
}

bool Config::changed(const Blob *next) const
{
    return memcmp(&data_, next, sizeof(data_)) != 0;
}

void Config::load()
{
    nvs_handle_t handle;
    Blob stored;
    size_t size = sizeof(stored);

    use_defaults();
    loaded_ = true;
    if (!nvs_ready() || nvs_open(CFG_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(handle, CFG_KEY, &stored, &size) == ESP_OK && size == sizeof(stored)) {
        data_ = stored;
        sanitize();
    }
    nvs_close(handle);
}

void Config::save()
{
    nvs_handle_t handle;

    if (!loaded_ || !nvs_ready()) {
        return;
    }
    data_.version = CFG_VERSION;
    if (nvs_open(CFG_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(handle, CFG_KEY, &data_, sizeof(data_)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

uint16_t Config::color() const
{
    return data_.color;
}

int Config::font() const
{
    return data_.font;
}

int Config::page() const
{
    return data_.page;
}

bool Config::backlight() const
{
    return data_.backlight != 0;
}

bool Config::bt() const
{
    return data_.bt != 0;
}

bool Config::reconnect() const
{
    return data_.reconnect != 0;
}

void Config::set_color(uint16_t color)
{
    Blob next = data_;
    next.color = color;
    if (!changed(&next)) {
        return;
    }
    data_.color = color;
    save();
}

void Config::set_font(int width)
{
    Blob next = data_;
    next.font = (uint8_t)(width == 6 ? 6 : 5);
    if (!changed(&next)) {
        return;
    }
    data_.font = next.font;
    save();
}

void Config::set_page(int pages)
{
    Blob next = data_;
    if (pages < 2 || pages > 10) {
        return;
    }
    next.page = (uint8_t)pages;
    if (!changed(&next)) {
        return;
    }
    data_.page = next.page;
    save();
}

void Config::set_backlight(bool on)
{
    Blob next = data_;
    next.backlight = on ? 1 : 0;
    if (!changed(&next)) {
        return;
    }
    data_.backlight = next.backlight;
    save();
}

void Config::set_bt(bool on)
{
    Blob next = data_;
    next.bt = on ? 1 : 0;
    if (!changed(&next)) {
        return;
    }
    data_.bt = next.bt;
    save();
}

void Config::set_reconnect(bool on)
{
    Blob next = data_;
    next.reconnect = on ? 1 : 0;
    if (!changed(&next)) {
        return;
    }
    data_.reconnect = next.reconnect;
    save();
}

Config &config_store(void)
{
    static Config store;
    return store;
}

extern "C" void config_start(void)
{
    config_store().load();
}

extern "C" uint16_t config_color(void)
{
    return config_store().color();
}

extern "C" int config_font(void)
{
    return config_store().font();
}

extern "C" int config_page(void)
{
    return config_store().page();
}

extern "C" bool config_backlight(void)
{
    return config_store().backlight();
}

extern "C" bool config_bt(void)
{
    return config_store().bt();
}

extern "C" bool config_reconnect(void)
{
    return config_store().reconnect();
}

extern "C" void config_set_color(uint16_t color)
{
    config_store().set_color(color);
}

extern "C" void config_set_font(int width)
{
    config_store().set_font(width);
}

extern "C" void config_set_page(int pages)
{
    config_store().set_page(pages);
}

extern "C" void config_set_backlight(bool on)
{
    config_store().set_backlight(on);
}

extern "C" void config_set_bt(bool on)
{
    config_store().set_bt(on);
}

extern "C" void config_set_reconnect(bool on)
{
    config_store().set_reconnect(on);
}
