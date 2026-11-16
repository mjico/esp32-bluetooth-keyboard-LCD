#include "camera.h"

#include <string.h>

#include "bluetooth.h"
#include "command_interpreter.h"
#include "i2c.h"
#include "lcd_text.h"
#include "trace.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_io_interface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/color_types.h"

#define CAM_W 240
#define CAM_H 240
#define CAM_XCLK_HZ 20000000
#define CAM_XCLK_SLOW_HZ 10000000
#define CAM_STRIP 16
#define CAM_LED_OUT 0x02
#define CAM_LED_CFG 0x06
#define CAM_LED_BIT 0x01

static esp_cam_ctlr_handle_t cam_ctlr;
static esp_cam_sensor_device_t *cam_dev;
static esp_sccb_io_handle_t sccb_io;
static bool sccb_on;
static uint16_t *frame_buf;
static size_t frame_bytes;
static int cam_w = CAM_W;
static int cam_h = CAM_H;
static uint32_t cam_xclk = CAM_XCLK_HZ;

static TaskHandle_t cam_task;
static TaskHandle_t starter;
static volatile esp_err_t start_err;
static volatile bool stop_req;
static volatile bool active;
static volatile bool preview_up;
static bool use_yuv;

static void xclk_stop(void);

static bool on_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    (void)handle;
    (void)user_data;
    trans->buffer = frame_buf;
    trans->buflen = frame_bytes;
    return false;
}

static bool on_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    (void)handle;
    (void)trans;
    (void)user_data;
    BaseType_t wake = pdFALSE;
    if (cam_task != NULL) {
        vTaskNotifyGiveFromISR(cam_task, &wake);
    }
    return wake == pdTRUE;
}

static esp_err_t camera_led(bool on)
{
    uint8_t addr = 0;
    uint8_t out = 0;
    uint8_t dir = 0xFF;
    esp_err_t err;
    if (i2c_port_start(&addr) != ESP_OK || addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    err = i2c_reg_read(addr, CAM_LED_OUT, &out, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_reg_read(addr, CAM_LED_CFG, &dir, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (on) {
        out = (uint8_t)(out | CAM_LED_BIT);
    } else {
        out = (uint8_t)(out & (uint8_t)~CAM_LED_BIT);
    }
    dir = (uint8_t)(dir & (uint8_t)~CAM_LED_BIT);
    err = i2c_reg_write(addr, CAM_LED_OUT, &out, 1);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_reg_write(addr, CAM_LED_CFG, &dir, 1);
}

static void sccb_bus_stop(void);

static void close_sensor(void)
{
    xclk_stop();
    camera_led(false);
    if (cam_dev != NULL) {
        int off = 0;
        esp_cam_sensor_ioctl(cam_dev, ESP_CAM_SENSOR_IOC_S_STREAM, &off);
        esp_cam_sensor_del_dev(cam_dev);
        cam_dev = NULL;
    }
    if (sccb_io != NULL) {
        sccb_io->del(sccb_io);
        sccb_io = NULL;
    }
    sccb_bus_stop();
}

static void close_ctlr(void)
{
    if (cam_ctlr != NULL) {
        esp_cam_ctlr_stop(cam_ctlr);
        esp_cam_ctlr_disable(cam_ctlr);
        esp_cam_ctlr_del(cam_ctlr);
        cam_ctlr = NULL;
    }
    if (frame_buf != NULL) {
        heap_caps_free(frame_buf);
        frame_buf = NULL;
    }
}

static esp_err_t make_ctlr(int w, int h, bool yuv, bool swap_bytes)
{
    esp_cam_ctlr_dvp_pin_config_t pin_cfg = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .vsync_io = GPIO_NUM_6,
        .de_io = GPIO_NUM_7,
        .pclk_io = GPIO_NUM_13,
        .xclk_io = GPIO_NUM_NC,
    };
    static const gpio_num_t data_pins[8] = {
        GPIO_NUM_11, GPIO_NUM_9, GPIO_NUM_8, GPIO_NUM_10,
        GPIO_NUM_12, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_16,
    };

    for (int i = 0; i < CAM_DVP_DATA_SIG_NUM; i++) {
        pin_cfg.data_io[i] = GPIO_NUM_NC;
    }
    for (int i = 0; i < 8; i++) {
        pin_cfg.data_io[i] = data_pins[i];
    }

    esp_cam_ctlr_dvp_config_t cfg = {
        .ctlr_id = 0,
        .clk_src = CAM_CLK_SRC_DEFAULT,
        .h_res = w,
        .v_res = h,
        .input_data_color_type = yuv ? CAM_CTLR_COLOR_YUV422 : CAM_CTLR_COLOR_RGB565,
        .dma_burst_size = 64,
        .xclk_freq = 0,
        .pin = &pin_cfg,
        .bk_buffer_dis = 1,
        .external_xtal = 1,
        .byte_swap_en = swap_bytes ? 1 : 0,
    };
    return esp_cam_new_dvp_ctlr(&cfg, &cam_ctlr);
}

/* The working board image clocks the sensor with LEDC at 10 MHz. */
static void xclk_release_pin(void)
{
    esp_gpio_revoke(1ULL << 15);
    gpio_reset_pin(GPIO_NUM_15);
}

static esp_err_t xclk_start_hz(uint32_t hz)
{
    xclk_release_pin();
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = GPIO_NUM_15,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1,
        .hpoint = 0,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        return err;
    }
    cam_xclk = hz;
    return ESP_OK;
}

static esp_err_t xclk_start(void)
{
    return xclk_start_hz(CAM_XCLK_SLOW_HZ);
}

static void xclk_stop(void)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    xclk_release_pin();
    cam_xclk = 0;
}

static bool format_fits(const esp_cam_sensor_format_t *fmt)
{
    return fmt->port == ESP_CAM_SENSOR_DVP &&
           fmt->width > 0 && fmt->height > 0 &&
           fmt->width <= LCD_TEXT_WIDTH && fmt->height <= LCD_TEXT_HEIGHT &&
           (fmt->height % CAM_STRIP) == 0;
}

static const esp_cam_sensor_format_t *choose_format(bool *swap_bytes, bool *yuv, bool *shrink)
{
    esp_cam_sensor_format_array_t formats = {0};
    const esp_cam_sensor_format_t *rgb = NULL;
    const esp_cam_sensor_format_t *yuv_fmt = NULL;
    const esp_cam_sensor_format_t *big = NULL;

    *swap_bytes = false;
    *yuv = false;
    *shrink = false;
    if (esp_cam_sensor_query_format(cam_dev, &formats) != ESP_OK || formats.format_array == NULL) {
        return NULL;
    }
    for (int i = 0; i < formats.count; i++) {
        const esp_cam_sensor_format_t *fmt = &formats.format_array[i];
        if (fmt->port != ESP_CAM_SENSOR_DVP) {
            continue;
        }
        if (fmt->format == ESP_CAM_SENSOR_PIXFORMAT_RGB565) {
            if (format_fits(fmt)) {
                if (rgb == NULL || fmt->width * fmt->height > rgb->width * rgb->height) {
                    rgb = fmt;
                }
            } else if (big == NULL || fmt->width * fmt->height < big->width * big->height) {
                big = fmt;
            }
        } else if (fmt->format == ESP_CAM_SENSOR_PIXFORMAT_YUV422 && format_fits(fmt)) {
            if (yuv_fmt == NULL || fmt->width * fmt->height > yuv_fmt->width * yuv_fmt->height) {
                yuv_fmt = fmt;
            }
        }
    }
    if (rgb == NULL) {
        rgb = yuv_fmt;
        *yuv = rgb != NULL;
    }
    if (rgb == NULL) {
        if (big == NULL) {
            return NULL;
        }
        rgb = big;
        *shrink = true;
        cam_w = CAM_W;
        cam_h = CAM_H;
        return rgb;
    }
    cam_w = rgb->width;
    cam_h = rgb->height;
    return rgb;
}

typedef struct {
    esp_sccb_io_t base;
    uint8_t addr;
} cam_sccb_t;

static TickType_t sccb_ticks(int ms)
{
    if (ms < 1) {
        ms = 1000;
    }
    return pdMS_TO_TICKS(ms);
}

static esp_err_t sccb_bus_start(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_4,
        .scl_io_num = GPIO_NUM_5,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    esp_err_t err;
    if (sccb_on) {
        return ESP_OK;
    }
    err = i2c_param_config(I2C_NUM_0, &conf);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        return err;
    }
    sccb_on = true;
    return ESP_OK;
}

static void sccb_bus_stop(void)
{
    if (!sccb_on) {
        return;
    }
    i2c_driver_delete(I2C_NUM_0);
    sccb_on = false;
}

static esp_err_t sccb_write(uint8_t addr, const uint8_t *data, size_t len, int ms)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    esp_err_t err;
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
    if (len > 0) {
        i2c_master_write(cmd, (uint8_t *)data, len, true);
    }
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(I2C_NUM_0, cmd, sccb_ticks(ms));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t sccb_read(uint8_t addr, uint8_t *data, size_t len, int ms)
{
    i2c_cmd_handle_t cmd;
    esp_err_t err;
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_READ), true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(I2C_NUM_0, cmd, sccb_ticks(ms));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t sccb_tx(esp_sccb_io_t *io, const uint8_t *buf, size_t n, int ms)
{
    return sccb_write(((cam_sccb_t *)io)->addr, buf, n, ms);
}

static esp_err_t sccb_rx(esp_sccb_io_t *io, uint8_t *buf, size_t n, int ms)
{
    return sccb_read(((cam_sccb_t *)io)->addr, buf, n, ms);
}

static esp_err_t sccb_txrx(esp_sccb_io_t *io, const uint8_t *wbuf, size_t wn,
                           uint8_t *rbuf, size_t rn, int ms)
{
    esp_err_t err = sccb_tx(io, wbuf, wn, ms);
    if (err != ESP_OK) {
        return err;
    }
    return sccb_rx(io, rbuf, rn, ms);
}

static esp_err_t sccb_del(esp_sccb_io_t *io)
{
    heap_caps_free(io);
    return ESP_OK;
}

static esp_sccb_io_handle_t sccb_open(uint8_t addr)
{
    cam_sccb_t *io = heap_caps_calloc(1, sizeof(*io), MALLOC_CAP_INTERNAL);
    if (io == NULL) {
        return NULL;
    }
    io->addr = addr;
    io->base.transmit_reg_a8v8 = sccb_tx;
    io->base.transmit_reg_a16v8 = sccb_tx;
    io->base.transmit_receive_reg_a16v8 = sccb_txrx;
    io->base.transmit_v16 = sccb_tx;
    io->base.receive_v16 = sccb_rx;
    io->base.transmit_receive_reg_a8v8 = sccb_txrx;
    io->base.del = sccb_del;
    return &io->base;
}

static esp_err_t open_sensor(void)
{
    esp_err_t err = sccb_bus_start();
    if (err != ESP_OK) {
        return err;
    }

    for (esp_cam_sensor_detect_fn_t *probe = &__esp_cam_sensor_detect_fn_array_start;
         probe < &__esp_cam_sensor_detect_fn_array_end; probe++) {
        esp_sccb_io_handle_t io;
        esp_cam_sensor_config_t cfg = {
            .reset_pin = -1,
            .pwdn_pin = -1,
            .xclk_pin = GPIO_NUM_15,
            .xclk_freq_hz = (int32_t)cam_xclk,
            .sensor_port = probe->port,
        };
        if (probe->detect == NULL || probe->port != ESP_CAM_SENSOR_DVP) {
            continue;
        }
        io = sccb_open(probe->sccb_addr);
        if (io == NULL) {
            sccb_bus_stop();
            return ESP_ERR_NO_MEM;
        }
        cfg.sccb_handle = io;
        cam_dev = probe->detect(&cfg);
        if (cam_dev != NULL) {
            sccb_io = io;
            return ESP_OK;
        }
        io->del(io);
    }
    sccb_bus_stop();
    return ESP_ERR_NOT_FOUND;
}

static void blank_screen(void)
{
    static uint16_t black[LCD_TEXT_WIDTH * CAM_STRIP];
    for (int y = 0; y < LCD_TEXT_HEIGHT; y += CAM_STRIP) {
        lcd_text_blit(0, y, LCD_TEXT_WIDTH, CAM_STRIP, black);
    }
}

static void draw_frame(void)
{
    int x = (LCD_TEXT_WIDTH - cam_w) / 2;
    int y0 = (LCD_TEXT_HEIGHT - cam_h) / 2;
    for (int y = 0; y < cam_h; y += CAM_STRIP) {
        lcd_text_blit(x, y0 + y, cam_w, CAM_STRIP, frame_buf + (size_t)y * cam_w);
    }
}

static esp_err_t ov_output_size(uint16_t w, uint16_t h)
{
    const uint16_t regs[] = {0x3808, 0x3809, 0x380a, 0x380b};
    const uint8_t vals[] = {
        (uint8_t)(w >> 8), (uint8_t)w,
        (uint8_t)(h >> 8), (uint8_t)h,
    };
    for (int i = 0; i < 4; i++) {
        esp_err_t err = esp_sccb_transmit_reg_a16v8(sccb_io, regs[i], vals[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t bringup(void)
{
    bool swap_bytes = false;
    bool shrink = false;
    const esp_cam_sensor_format_t *fmt = NULL;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    uint32_t want_hz;

    cam_w = CAM_W;
    cam_h = CAM_H;
    camera_led(true);
    err = xclk_start();
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    err = open_sensor();
    if (err != ESP_OK) {
        return err;
    }
    fmt = choose_format(&swap_bytes, &use_yuv, &shrink);
    if (fmt == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    want_hz = fmt->xclk;
    if (want_hz > CAM_XCLK_HZ) {
        want_hz = CAM_XCLK_HZ;
    }
    if (want_hz >= CAM_XCLK_SLOW_HZ && want_hz != cam_xclk) {
        if (xclk_start_hz(want_hz) != ESP_OK) {
            xclk_start();
        }
    }
    err = make_ctlr(cam_w, cam_h, use_yuv, swap_bytes);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_cam_sensor_set_format(cam_dev, fmt);
    if (err != ESP_OK) {
        return err;
    }
    if (shrink) {
        err = ov_output_size((uint16_t)cam_w, (uint16_t)cam_h);
        if (err != ESP_OK) {
            return err;
        }
    }
    int on = 1;
    err = esp_cam_sensor_ioctl(cam_dev, ESP_CAM_SENSOR_IOC_S_STREAM, &on);
    if (err != ESP_OK) {
        return err;
    }

    frame_bytes = (size_t)cam_w * cam_h * sizeof(uint16_t);
#if CONFIG_SPIRAM
    frame_buf = esp_cam_ctlr_alloc_buffer(cam_ctlr, frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
#else
    frame_buf = esp_cam_ctlr_alloc_buffer(cam_ctlr, frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
#endif
    if (frame_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    err = esp_cam_ctlr_register_event_callbacks(cam_ctlr, &cbs, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_cam_ctlr_enable(cam_ctlr);
    if (err != ESP_OK) {
        return err;
    }
    if (use_yuv) {
        const cam_ctlr_format_conv_config_t conv = {
            .src_format = CAM_CTLR_COLOR_YUV422,
            .dst_format = CAM_CTLR_COLOR_RGB565,
            .conv_std = COLOR_CONV_STD_RGB_YUV_BT601,
            .data_width = 8,
            .input_range = COLOR_RANGE_LIMIT,
            .output_range = COLOR_RANGE_LIMIT,
        };
        err = esp_cam_ctlr_format_conversion(cam_ctlr, &conv);
        if (err != ESP_OK) {
            return err;
        }
    }
    return esp_cam_ctlr_start(cam_ctlr);
}

static void cam_task_fn(void *arg)
{
    (void)arg;
    cam_task = xTaskGetCurrentTaskHandle();
    start_err = bringup();
    if (start_err == ESP_OK && !stop_req) {
        lcd_text_hold_screen();
        blank_screen();
        preview_up = true;
    }
    if (starter != NULL) {
        xTaskNotifyGive(starter);
    }
    if (start_err != ESP_OK || stop_req) {
        close_ctlr();
        close_sensor();
        active = false;
        cam_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (!stop_req) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (stop_req) {
            break;
        }
        draw_frame();
    }

    close_ctlr();
    close_sensor();
    lcd_text_pixels_release();
    bluetooth_show_key_draft();
    active = false;
    cam_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t camera_preview_start(void)
{
    if (active) {
        return ESP_ERR_INVALID_STATE;
    }
    stop_req = false;
    preview_up = false;
    use_yuv = false;
    start_err = ESP_FAIL;
    active = true;
    starter = xTaskGetCurrentTaskHandle();
    if (xTaskCreate(cam_task_fn, "camera", 16384, NULL, 5, NULL) != pdPASS) {
        active = false;
        starter = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000)) == 0) {
        camera_preview_request_stop();
        return ESP_ERR_TIMEOUT;
    }
    starter = NULL;
    if (start_err != ESP_OK) {
        return start_err;
    }
    if (!preview_up) {
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_OK;
}

void camera_preview_request_stop(void)
{
    stop_req = true;
    if (cam_task != NULL) {
        xTaskNotifyGive(cam_task);
    }
}

bool camera_preview_active(void)
{
    return active;
}

static void cmd_camled(int argc, char **argv)
{
    bool on;
    esp_err_t err;
    if (argc < 2) {
        trace_print("camled 0|1");
        return;
    }
    if (strcmp(argv[1], "1") == 0) {
        on = true;
    } else if (strcmp(argv[1], "0") == 0) {
        on = false;
    } else {
        trace_print("camled 0|1");
        return;
    }
    err = camera_led(on);
    if (err == ESP_ERR_INVALID_STATE) {
        trace_print("camled no xl9555");
        return;
    }
    if (err != ESP_OK) {
        trace_printf("camled %s", esp_err_to_name(err));
        return;
    }
    trace_printf("camled %d", on ? 1 : 0);
}

static void camera_probe(void)
{
    static const uint8_t addrs[] = {
        0x18, 0x21, 0x30, 0x36, 0x38, 0x3c, 0x50,
    };
    int found = 0;
    esp_err_t err;

    camera_led(true);
    err = xclk_start();
    if (err != ESP_OK) {
        trace_printf("xclk %s", esp_err_to_name(err));
        camera_led(false);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    trace_print("xclk 10");

    err = sccb_bus_start();
    if (err != ESP_OK) {
        trace_printf("sccb %s", esp_err_to_name(err));
        xclk_stop();
        camera_led(false);
        return;
    }
    trace_print("sccb 4/5");

    for (int i = 0; i < (int)(sizeof(addrs) / sizeof(addrs[0])); i++) {
        if (sccb_write(addrs[i], NULL, 0, 50) != ESP_OK) {
            continue;
        }
        found++;
        if (addrs[i] == 0x21) {
            trace_print("0x21 gc0308");
        } else if (addrs[i] == 0x30) {
            trace_print("0x30 ov2640");
        } else if (addrs[i] == 0x3c) {
            trace_print("0x3c cam");
        } else if (addrs[i] == 0x18) {
            trace_print("0x18 es8311");
        } else if (addrs[i] == 0x38) {
            trace_print("0x38 touch");
        } else if (addrs[i] == 0x50) {
            trace_print("0x50 eeprom");
        } else {
            trace_printf("0x%02X ack", addrs[i]);
        }
    }

    sccb_bus_stop();
    xclk_stop();
    camera_led(false);
    if (found == 0) {
        trace_print("sccb none");
    } else {
        trace_printf("sccb %d", found);
    }
}

static void cmd_camera(int argc, char **argv)
{
    esp_err_t err;
    if (argc >= 2) {
        if (strcmp(argv[1], "probe") == 0) {
            if (lcd_text_pixels_holding() || camera_preview_active()) {
                trace_print("camera busy");
                return;
            }
            camera_probe();
            return;
        }
        trace_print("camera [probe]");
        return;
    }
    if (lcd_text_pixels_holding() || camera_preview_active()) {
        trace_print("camera busy");
        return;
    }
    err = camera_preview_start();
    if (err == ESP_ERR_NOT_FINISHED) {
        trace_print("camera stop");
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        trace_print("no camera");
        return;
    }
    if (err == ESP_ERR_NO_MEM) {
        trace_print("camera no mem");
        return;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        trace_print("camera format");
        return;
    }
    if (err != ESP_OK) {
        trace_printf("camera %s", esp_err_to_name(err));
        return;
    }
    trace_print("camera");
}

void camera_register_commands(void)
{
    command_register("camera", "preview|probe", cmd_camera);
    command_register("camled", "0|1", cmd_camled);
}
