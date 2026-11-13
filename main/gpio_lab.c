#include "gpio_lab.h"

#include <string.h>

#include "command_interpreter.h"
#include "driver/gpio.h"
#include "trace.h"

/* PWM header signal pins only. 5V and GND on those headers are not GPIOs. */
static const int kPins[] = {9, 10, 11, 12};

typedef enum {
    kModeNone = 0,
    kModeIn,
    kModeOut,
} PinMode;

static PinMode pin_mode[sizeof(kPins) / sizeof(kPins[0])];

static int pin_index(int pin)
{
    for (size_t i = 0; i < sizeof(kPins) / sizeof(kPins[0]); i++) {
        if (kPins[i] == pin) {
            return (int)i;
        }
    }
    return -1;
}

static bool allowed(int pin)
{
    return pin_index(pin) >= 0;
}

static void remember(int pin, PinMode mode)
{
    int index = pin_index(pin);
    if (index >= 0) {
        pin_mode[index] = mode;
    }
}

static PinMode remembered(int pin)
{
    int index = pin_index(pin);
    if (index < 0) {
        return kModeNone;
    }
    return pin_mode[index];
}

static bool same_word(const char *text, const char *name)
{
    if (text == NULL || name == NULL) {
        return false;
    }
    while (*text != '\0' && *name != '\0') {
        char a = *text;
        char b = *name;
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
        text++;
        name++;
    }
    return *text == '\0' && *name == '\0';
}

static bool parse_pin(const char *text, int *pin)
{
    if (text == NULL || pin == NULL) {
        return false;
    }
    if (same_word(text, "pwm1")) {
        *pin = 9;
        return true;
    }
    if (same_word(text, "pwm2")) {
        *pin = 10;
        return true;
    }
    if (same_word(text, "pwm3")) {
        *pin = 11;
        return true;
    }
    if (same_word(text, "pwm4")) {
        *pin = 12;
        return true;
    }
    if (text[0] < '0' || text[0] > '9') {
        return false;
    }
    int value = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10 + (*p - '0');
        if (value > 48) {
            return false;
        }
    }
    if (!allowed(value)) {
        return false;
    }
    *pin = value;
    return true;
}

static esp_err_t config_pin(int pin, gpio_mode_t mode, gpio_pull_mode_t pull)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = mode,
        .pull_up_en = (pull == GPIO_PULLUP_ONLY) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (pull == GPIO_PULLDOWN_ONLY) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static void print_pins(void)
{
    trace_print("gpio 9=pwm1 10=pwm2 11=pwm3 12=pwm4");
}

static void cmd_gpio(int argc, char **argv)
{
    if (argc < 2) {
        trace_print("gpio mode|pull|set|get|loop");
        print_pins();
        return;
    }
    if (same_word(argv[1], "mode")) {
        int pin = 0;
        if (argc < 4 || !parse_pin(argv[2], &pin)) {
            trace_print("gpio mode <pin> in|out");
            return;
        }
        gpio_mode_t mode;
        const char *name;
        if (same_word(argv[3], "in")) {
            mode = GPIO_MODE_INPUT;
            name = "in";
        } else if (same_word(argv[3], "out")) {
            mode = GPIO_MODE_OUTPUT;
            name = "out";
        } else {
            trace_print("gpio mode <pin> in|out");
            return;
        }
        esp_err_t err = config_pin(pin, mode, GPIO_FLOATING);
        if (err != ESP_OK) {
            trace_printf("gpio mode %s", esp_err_to_name(err));
            return;
        }
        remember(pin, same_word(argv[3], "in") ? kModeIn : kModeOut);
        trace_printf("gpio mode %d %s", pin, name);
        return;
    }
    if (same_word(argv[1], "pull")) {
        int pin = 0;
        if (argc < 4 || !parse_pin(argv[2], &pin)) {
            trace_print("gpio pull <pin> up|down|none");
            return;
        }
        gpio_pull_mode_t pull;
        const char *name;
        if (same_word(argv[3], "up")) {
            pull = GPIO_PULLUP_ONLY;
            name = "up";
        } else if (same_word(argv[3], "down")) {
            pull = GPIO_PULLDOWN_ONLY;
            name = "down";
        } else if (same_word(argv[3], "none")) {
            pull = GPIO_FLOATING;
            name = "none";
        } else {
            trace_print("gpio pull <pin> up|down|none");
            return;
        }
        esp_err_t err = config_pin(pin, GPIO_MODE_INPUT, pull);
        if (err != ESP_OK) {
            trace_printf("gpio pull %s", esp_err_to_name(err));
            return;
        }
        remember(pin, kModeIn);
        trace_printf("gpio pull %d %s", pin, name);
        return;
    }
    if (same_word(argv[1], "set")) {
        int pin = 0;
        if (argc < 4 || !parse_pin(argv[2], &pin) ||
            (argv[3][0] != '0' && argv[3][0] != '1') || argv[3][1] != '\0') {
            trace_print("gpio set <pin> 0|1");
            return;
        }
        if (remembered(pin) != kModeOut) {
            trace_print("gpio set needs mode out");
            return;
        }
        int level = argv[3][0] - '0';
        esp_err_t err = gpio_set_level((gpio_num_t)pin, level);
        if (err != ESP_OK) {
            trace_printf("gpio set %s", esp_err_to_name(err));
            return;
        }
        trace_printf("gpio set %d %d", pin, level);
        return;
    }
    if (same_word(argv[1], "get")) {
        int pin = 0;
        if (argc < 3 || !parse_pin(argv[2], &pin)) {
            trace_print("gpio get <pin>");
            return;
        }
        if (remembered(pin) != kModeIn) {
            trace_print("gpio get needs mode in");
            return;
        }
        int level = gpio_get_level((gpio_num_t)pin);
        trace_printf("gpio get %d %d", pin, level);
        return;
    }
    if (same_word(argv[1], "loop")) {
        int out_pin = 0;
        int in_pin = 0;
        if (argc < 5 || !parse_pin(argv[2], &out_pin) || !parse_pin(argv[3], &in_pin) ||
            out_pin == in_pin ||
            (argv[4][0] != '0' && argv[4][0] != '1') || argv[4][1] != '\0') {
            trace_print("gpio loop <out> <in> 0|1");
            return;
        }
        int level = argv[4][0] - '0';
        esp_err_t err = config_pin(out_pin, GPIO_MODE_OUTPUT, GPIO_FLOATING);
        if (err == ESP_OK) {
            err = config_pin(in_pin, GPIO_MODE_INPUT, GPIO_FLOATING);
        }
        if (err == ESP_OK) {
            err = gpio_set_level((gpio_num_t)out_pin, level);
        }
        if (err != ESP_OK) {
            trace_printf("gpio loop %s", esp_err_to_name(err));
            return;
        }
        remember(out_pin, kModeOut);
        remember(in_pin, kModeIn);
        int read = gpio_get_level((gpio_num_t)in_pin);
        trace_printf("gpio loop %d=%d %d=%d", out_pin, level, in_pin, read);
        return;
    }
    trace_print("gpio mode|pull|set|get|loop");
}

void gpio_lab_register_commands(void)
{
    command_register("gpio", "pwm pin mode set get", cmd_gpio);
}
