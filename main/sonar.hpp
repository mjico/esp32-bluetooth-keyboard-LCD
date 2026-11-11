#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Hiwonder glowing ultrasonic module on the shared I2C bus. */
class Sonar {
public:
    static const uint8_t kAddr = 0x77;
    static const uint8_t kModeSimple = 0;
    static const uint8_t kModeBreath = 1;

    /* Register 1 then 0. Raw unit is 0.1 cm. */
    esp_err_t distance(uint16_t *raw);

    /* Register 2. mode is kModeSimple or kModeBreath. */
    esp_err_t set_mode(uint8_t mode);

    /* Simple mode, then RGB1 and RGB2 starting at register 3. */
    esp_err_t set_color(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2);

    /* Breathing mode, then six cycle bytes starting at register 9. */
    esp_err_t set_breath(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2);

private:
    static const uint8_t kRegDistanceL = 0;
    static const uint8_t kRegDistanceH = 1;
    static const uint8_t kRegMode = 2;
    static const uint8_t kRegRgb = 3;
    static const uint8_t kRegBreath = 9;
};

Sonar &sonar_dev(void);
