#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Hiwonder matrix-iic at 0x4C. Two 8x8 panels in one 16-byte static frame.
 * Each panel is 8 row bytes, bit 7 is the left pixel, row 0 is the top.
 * Panel 1 is bytes 0..7. Panel 2 is bytes 8..15.
 * Direction 1 is upright, 2 is 90 degrees clockwise, 3 is 180, 4 is 270 clockwise.
 */
class Matrix {
public:
    static const uint8_t kAddr = 0x4C;
    static const int kScreen1 = 1;
    static const int kScreen2 = 2;
    static const int kScreenBoth = 0;

    static const int kImageSmile = 0;
    static const int kImageSad = 1;
    static const int kImageHeart = 2;
    static const int kImageUp = 3;
    static const int kImageBox = 4;
    static const int kImageCross = 5;
    static const int kImageCheck = 6;
    static const int kImageFill = 7;
    static const int kImageCount = 8;

    esp_err_t set_brightness(uint8_t level);
    esp_err_t clear();
    esp_err_t show_raw(const uint8_t data[16]);
    esp_err_t scroll(const uint8_t *data, size_t len);
    esp_err_t show_image(int screen, int image, int direction);
    /* Top 8 rows of one font8x16 glyph on panel 1, bottom 8 rows on panel 2. */
    esp_err_t show_char(char ch);
    /* One static write, no blank delay. Used while a key report is being handled. */
    esp_err_t show_char_fast(char ch);

private:
    static const uint8_t kRegBrightness = 0x00;
    static const uint8_t kRegStatic = 0x01;
    static const uint8_t kRegScroll = 0x10;

    uint8_t frame_[16] = {};

    esp_err_t write_frame();
};

Matrix &matrix_dev(void);
