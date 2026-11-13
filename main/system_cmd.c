#include "system_cmd.h"

#include <stdio.h>

#include "bluetooth.h"
#include "command_interpreter.h"
#include "config.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "font.h"
#include "i2c.h"
#include "lcd_text.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "trace.h"

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

static const char *reset_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "ext";
    case ESP_RST_SW:
        return "sw";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int_wdt";
    case ESP_RST_TASK_WDT:
        return "task_wdt";
    case ESP_RST_WDT:
        return "wdt";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    default:
        return "unknown";
    }
}

static void print_up(void)
{
    uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t days = sec / 86400;
    sec %= 86400;
    uint32_t hours = sec / 3600;
    sec %= 3600;
    uint32_t mins = sec / 60;
    sec %= 60;
    if (days > 0) {
        trace_printf("up %ud %02u:%02u:%02u", (unsigned)days, (unsigned)hours, (unsigned)mins, (unsigned)sec);
        return;
    }
    trace_printf("up %02u:%02u:%02u", (unsigned)hours, (unsigned)mins, (unsigned)sec);
}

static void print_heap(void)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    trace_printf("free %u min %u", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    trace_printf("int %u/%u big %u", (unsigned)internal_free, (unsigned)internal_total, (unsigned)largest);
    if (psram_total == 0) {
        trace_print("psram off");
        return;
    }
    trace_printf("psram %u/%u", (unsigned)psram_free, (unsigned)psram_total);
}

static void print_chip(void)
{
    esp_chip_info_t chip;
    uint32_t flash = 0;
    uint8_t mac[6] = {0};
    char feat[40];
    int used = 0;

    esp_chip_info(&chip);
    esp_flash_get_size(NULL, &flash);
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    feat[0] = '\0';
    if (chip.features & CHIP_FEATURE_WIFI_BGN) {
        used += snprintf(feat + used, sizeof(feat) - (size_t)used, "wifi ");
    }
    if (chip.features & CHIP_FEATURE_BT) {
        used += snprintf(feat + used, sizeof(feat) - (size_t)used, "bt ");
    }
    if (chip.features & CHIP_FEATURE_BLE) {
        used += snprintf(feat + used, sizeof(feat) - (size_t)used, "ble");
    }

    trace_printf("esp32s3 rev %u.%u cores %u", (unsigned)(chip.revision / 100), (unsigned)(chip.revision % 100), (unsigned)chip.cores);
    trace_print(feat[0] != '\0' ? feat : "feat none");
    trace_printf("flash %uMB cpu %uMHz", (unsigned)(flash / (1024 * 1024)), (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    trace_printf("mac %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static char task_state(eTaskState state)
{
    switch (state) {
    case eRunning:
        return 'X';
    case eReady:
        return 'R';
    case eBlocked:
        return 'B';
    case eSuspended:
        return 'S';
    case eDeleted:
        return 'D';
    default:
        return '?';
    }
}

static void print_tasks(void)
{
    UBaseType_t expected = uxTaskGetNumberOfTasks();
    UBaseType_t cap = expected + 16;
    TaskStatus_t *tasks;
    UBaseType_t got;
    char name[13];

    if (cap < 32) {
        cap = 32;
    }
    tasks = heap_caps_malloc(sizeof(TaskStatus_t) * cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (tasks == NULL) {
        trace_print("tasks no mem");
        return;
    }
    got = uxTaskGetSystemState(tasks, cap, NULL);
    if (got == 0) {
        heap_caps_free(tasks);
        expected = uxTaskGetNumberOfTasks();
        cap = expected + 32;
        if (cap < 48) {
            cap = 48;
        }
        tasks = heap_caps_malloc(sizeof(TaskStatus_t) * cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (tasks == NULL) {
            trace_print("tasks no mem");
            return;
        }
        got = uxTaskGetSystemState(tasks, cap, NULL);
    }
    if (got == 0) {
        trace_printf("tasks 0 need %u cap %u", (unsigned)expected, (unsigned)cap);
        heap_caps_free(tasks);
        return;
    }
    trace_printf("tasks %u", (unsigned)got);
    trace_print("X run  R ready  B block");
    trace_print("name st pri free core");
    for (UBaseType_t i = 0; i < got; i++) {
        int core = -1;
        const char *src = tasks[i].pcTaskName;
        int n = 0;
#if (configTASKLIST_INCLUDE_COREID == 1)
        core = (int)tasks[i].xCoreID;
        if (core == (int)tskNO_AFFINITY) {
            core = -1;
        }
#endif
        if (src == NULL) {
            src = "?";
        }
        while (n < 12 && src[n] != '\0') {
            name[n] = src[n];
            n++;
        }
        name[n] = '\0';
        trace_printf("%s %c %u %u %d", name, task_state(tasks[i].eCurrentState),
                     (unsigned)tasks[i].uxCurrentPriority,
                     (unsigned)tasks[i].usStackHighWaterMark, core);
    }
    heap_caps_free(tasks);
}

static void print_ver(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    trace_printf("app %s", app->project_name);
    trace_printf("ver %s", app->version);
    trace_printf("idf %s", app->idf_ver);
}

static void print_system_hint(void)
{
    trace_print("system heap|chip|task|ver|up");
    trace_print("system font|color|backlight|page|bt");
}

static void cmd_system(int argc, char **argv)
{
    if (argc < 2) {
        const esp_app_desc_t *app = esp_app_get_description();
        print_up();
        trace_printf("heap %u min %u", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
        trace_printf("rst %s tasks %u", reset_name(esp_reset_reason()), (unsigned)uxTaskGetNumberOfTasks());
        trace_printf("ver %s", app->version);
        print_system_hint();
        return;
    }
    if (same_word(argv[1], "heap")) {
        print_heap();
        return;
    }
    if (same_word(argv[1], "chip")) {
        print_chip();
        return;
    }
    if (same_word(argv[1], "task") || same_word(argv[1], "tasks")) {
        print_tasks();
        return;
    }
    if (same_word(argv[1], "ver")) {
        print_ver();
        return;
    }
    if (same_word(argv[1], "up")) {
        print_up();
        return;
    }
    if (same_word(argv[1], "font")) {
        font_command(argc - 2, argv + 2);
        return;
    }
    if (same_word(argv[1], "color")) {
        lcd_text_command_color(argc - 2, argv + 2);
        return;
    }
    if (same_word(argv[1], "backlight")) {
        i2c_backlight_command(argc - 2, argv + 2);
        return;
    }
    if (same_word(argv[1], "page")) {
        lcd_text_command_page(argc - 2, argv + 2);
        return;
    }
    if (same_word(argv[1], "bt")) {
        if (argc >= 3) {
            if ((argv[2][0] != '0' && argv[2][0] != '1') || argv[2][1] != '\0') {
                trace_print("system bt [0|1]");
                return;
            }
            config_set_bt(argv[2][0] == '1');
            if (config_bt()) {
                bluetooth_reconnect();
            }
        }
        trace_printf("bt %d", config_bt() ? 1 : 0);
        if (argc < 3) {
            trace_print("system bt [0|1]");
        }
        return;
    }
    print_system_hint();
}

void system_register_commands(void)
{
    command_register("system", "heap task font page bt bl", cmd_system);
}
