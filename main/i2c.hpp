#pragma once

#include "i2c.h"

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* One bus for every 4-pin I2C header, the XL9555, and debug register access. */
class I2c {
public:
    static const int kMaxDevices = 8;
    static const int kMaxBytes = 16;
    static const int kSpeedHz = 100000;

    I2c();

    esp_err_t start(uint8_t *xl_addr);
    esp_err_t backlight_on();
    esp_err_t probe(uint8_t addr);
    esp_err_t scan(uint8_t *addrs, int *count, int max_count);
    esp_err_t read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len);

private:
    i2c_master_dev_handle_t device(uint8_t addr, esp_err_t *err);
    void lock();
    void unlock();

    i2c_master_bus_handle_t bus_;
    uint8_t xl_addr_;
    i2c_master_dev_handle_t devs_[kMaxDevices];
    uint8_t dev_addr_[kMaxDevices];
    int dev_count_;
    SemaphoreHandle_t mu_;
};

I2c &i2c_port(void);
