#include "bluetooth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_hidh.h"
#include "esp_private/esp_hidh_private.h"
#include "esp_heap_caps.h"
#include "esp_phy_init.h"
#include "trace.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "command_history.h"
#include "command_interpreter.h"
#include "lcd_text.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/queue.h"

#define SCAN_SECONDS 15
#define MAX_DEVICES 16
#define DEV_NAME_MAX 24
#define PEER_NVS_NS "bt_hid"
#define PEER_NVS_KEY "peer"

/* 1 prints the report map, CCCD counts, and raw HID bytes. */
#define BT_DEBUG 0

typedef struct {
    esp_bd_addr_t addr;
    esp_ble_addr_type_t addr_type;
    char name[DEV_NAME_MAX];
} found_device_t;

typedef struct {
    esp_bd_addr_t addr;
    uint8_t addr_type;
    char name[DEV_NAME_MAX];
} peer_nvs_t;

static esp_lcd_panel_handle_t panel;
static bool stack_ready;
static bool params_ready;
static bool want_scan;
static bool scanning;
static bool scan_discover;
static bool reconnect_stop;
static bool hid_ready;
static found_device_t devices[MAX_DEVICES];
static int device_count;
static esp_hidh_dev_t *active_dev;
static esp_bd_addr_t active_addr;
static esp_ble_addr_type_t active_addr_type;
static char active_name[DEV_NAME_MAX];
static bool active_connected;
static bool waiting_close;
static SemaphoreHandle_t close_done;
static esp_gatt_if_t gatt_if;
static volatile bool own_gatt_op;
static volatile esp_gatt_status_t gatt_op_status;
static SemaphoreHandle_t gatt_op_done;
static TaskHandle_t scan_task;

static esp_ble_scan_params_t scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

/* Keyboard line is separate from the serial line. Each side keeps its own text until Enter. */
#define KEY_LINE_MAX 128

static char key_line[KEY_LINE_MAX + 1];
static int key_len;
static int key_hist = -1;
static uint8_t keys_held[6];
static QueueHandle_t key_cmd_queue;

static void paint_key_draft(void);

static void key_cmd_task(void *arg)
{
    char line[KEY_LINE_MAX + 1];
    (void)arg;

    for (;;) {
        if (xQueueReceive(key_cmd_queue, line, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        char prompted[KEY_LINE_MAX + 2];
        snprintf(prompted, sizeof(prompted), ">%s", line);
        /* The draft row is the line just typed. Commit it, then echo whatever is being typed now. */
        lcd_text_dismiss_draft();
        trace_print(prompted);
        command_line_execute(panel, line);
        paint_key_draft();
    }
}

static void submit_key_line(void)
{
    char line[KEY_LINE_MAX + 1];

    memcpy(line, key_line, (size_t)key_len);
    line[key_len] = '\0';
    key_len = 0;
    key_hist = -1;

    if (key_cmd_queue == NULL) {
        key_cmd_queue = xQueueCreate(4, KEY_LINE_MAX + 1);
        if (key_cmd_queue != NULL &&
            xTaskCreate(key_cmd_task, "key-cmd", 8192, NULL, 5, NULL) != pdPASS) {
            vQueueDelete(key_cmd_queue);
            key_cmd_queue = NULL;
        }
    }
    if (key_cmd_queue == NULL || xQueueSend(key_cmd_queue, line, 0) != pdTRUE) {
        trace_print("line busy");
    }
}

static bool already_seen(const esp_bd_addr_t addr)
{
    for (int i = 0; i < device_count; i++) {
        if (memcmp(devices[i].addr, addr, ESP_BD_ADDR_LEN) == 0) {
            return true;
        }
    }
    return false;
}

static bool uuid16_has_hid(const uint8_t *data, uint8_t len)
{
    if (data == NULL) {
        return false;
    }
    for (int i = 0; i + 1 < len; i += 2) {
        uint16_t uuid = (uint16_t)(data[i] | (data[i + 1] << 8));
        if (uuid == 0x1812) {
            return true;
        }
    }
    return false;
}

static bool is_input_device(const uint8_t *adv, uint8_t adv_len)
{
    uint8_t len = 0;
    uint8_t *field = esp_ble_resolve_adv_data_by_type((uint8_t *)adv, adv_len, ESP_BLE_AD_TYPE_APPEARANCE, &len);
    if (field != NULL && len >= 2) {
        uint16_t appearance = (uint16_t)(field[0] | (field[1] << 8));
        if ((appearance >> 6) == 0x0F) {
            return true;
        }
    }
    field = esp_ble_resolve_adv_data_by_type((uint8_t *)adv, adv_len, ESP_BLE_AD_TYPE_16SRV_CMPL, &len);
    if (uuid16_has_hid(field, len)) {
        return true;
    }
    field = esp_ble_resolve_adv_data_by_type((uint8_t *)adv, adv_len, ESP_BLE_AD_TYPE_16SRV_PART, &len);
    return uuid16_has_hid(field, len);
}

static void copy_name(char *dst, const uint8_t *raw, uint8_t name_len)
{
    int n = name_len < (DEV_NAME_MAX - 1) ? name_len : (DEV_NAME_MAX - 1);
    int out = 0;
    for (int i = 0; i < n; i++) {
        char ch = (char)raw[i];
        dst[out++] = (ch >= 32 && ch <= 126) ? ch : ' ';
    }
    dst[out] = '\0';
}

#define KNOWN_MAX 16

static esp_bd_addr_t known_addr[KNOWN_MAX];
static int known_count;

static bool addr_is_zero(const uint8_t *addr)
{
    for (int i = 0; i < ESP_BD_ADDR_LEN; i++) {
        if (addr[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool is_known(const esp_bd_addr_t addr)
{
    for (int i = 0; i < known_count; i++) {
        if (memcmp(known_addr[i], addr, ESP_BD_ADDR_LEN) == 0) {
            return true;
        }
    }
    return false;
}

static void remember_known_addr(const esp_bd_addr_t addr)
{
    if (addr_is_zero(addr) || known_count >= KNOWN_MAX || is_known(addr)) {
        return;
    }
    memcpy(known_addr[known_count], addr, ESP_BD_ADDR_LEN);
    known_count++;
}

static void save_peer_nvs(const found_device_t *dev)
{
    peer_nvs_t peer = {0};
    nvs_handle_t nvs;

    if (dev == NULL || addr_is_zero(dev->addr)) {
        return;
    }
    memcpy(peer.addr, dev->addr, ESP_BD_ADDR_LEN);
    peer.addr_type = (uint8_t)dev->addr_type;
    memcpy(peer.name, dev->name, sizeof(peer.name));
    peer.name[sizeof(peer.name) - 1] = '\0';

    if (nvs_open(PEER_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_blob(nvs, PEER_NVS_KEY, &peer, sizeof(peer));
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_peer_nvs(void)
{
    peer_nvs_t peer = {0};
    size_t size = sizeof(peer);
    nvs_handle_t nvs;

    if (nvs_open(PEER_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(nvs, PEER_NVS_KEY, &peer, &size) == ESP_OK && size == sizeof(peer)) {
        if (!addr_is_zero(peer.addr)) {
            memcpy(active_addr, peer.addr, ESP_BD_ADDR_LEN);
            active_addr_type = (esp_ble_addr_type_t)peer.addr_type;
            remember_known_addr(peer.addr);
        }
        if (peer.name[0] != '\0' && active_name[0] == '\0') {
            memcpy(active_name, peer.name, sizeof(active_name));
            active_name[sizeof(active_name) - 1] = '\0';
        }
    }
    nvs_close(nvs);
}

static void load_known(void)
{
    known_count = 0;
    load_peer_nvs();
    remember_known_addr(active_addr);

    int bonds = esp_ble_get_bond_device_num();
    if (bonds <= 0) {
        return;
    }
    esp_ble_bond_dev_t *list = calloc((size_t)bonds, sizeof(*list));
    if (list == NULL || esp_ble_get_bond_device_list(&bonds, list) != ESP_OK) {
        free(list);
        return;
    }
    for (int i = 0; i < bonds; i++) {
        remember_known_addr(list[i].bd_addr);
        /* Keyboard reconnect ads often use RPA. Keep the identity address too. */
        if ((list[i].bond_key.key_mask & ESP_BLE_ID_KEY_MASK) != 0) {
            remember_known_addr(list[i].bond_key.pid_key.static_addr);
            esp_ble_gap_add_device_to_resolving_list(list[i].bond_key.pid_key.static_addr,
                                                     list[i].bond_key.pid_key.addr_type,
                                                     list[i].bond_key.pid_key.irk);
        }
    }
    free(list);
}

static bool name_matches_active(const char *name)
{
    return active_name[0] != '\0' && name != NULL && strcmp(active_name, name) == 0;
}

static bool reconnect_hit(const found_device_t *dev)
{
    return is_known(dev->addr) || name_matches_active(dev->name);
}

static void fill_adv_name(char *dst, size_t dst_size, const uint8_t *adv, uint8_t adv_len, const uint8_t *addr)
{
    uint8_t name_len = 0;
    uint8_t *raw = esp_ble_resolve_adv_data_by_type((uint8_t *)adv, adv_len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if (raw == NULL || name_len == 0) {
        raw = esp_ble_resolve_adv_data_by_type((uint8_t *)adv, adv_len, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }
    if (raw != NULL && name_len > 0) {
        copy_name(dst, raw, name_len);
        return;
    }
    snprintf(dst, dst_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static void show_device(const esp_ble_gap_cb_param_t *scan_rst)
{
    const uint8_t *adv = scan_rst->scan_rst.ble_adv;
    uint8_t adv_len = scan_rst->scan_rst.adv_data_len + scan_rst->scan_rst.scan_rsp_len;
    char name[DEV_NAME_MAX];
    bool hid;
    bool known;
    bool named;

    if (already_seen(scan_rst->scan_rst.bda) || device_count >= MAX_DEVICES) {
        return;
    }

    fill_adv_name(name, sizeof(name), adv, adv_len, scan_rst->scan_rst.bda);
    hid = is_input_device(adv, adv_len);
    known = is_known(scan_rst->scan_rst.bda);
    named = name_matches_active(name);

    /*
     * Reconnect advertising often clears HID UUID/appearance (ESP-IDF #15379).
     * Keep discover strict; for reconnect accept known address, last name,
     * or directed connectable ads while we already have a bond.
     */
    if (scan_discover) {
        if (!hid) {
            return;
        }
    } else {
        bool directed = (scan_rst->scan_rst.ble_evt_type == ESP_BLE_EVT_CONN_DIR_ADV);
        if (!hid && !known && !named && !(directed && known_count > 0)) {
            return;
        }
    }

    found_device_t *dev = &devices[device_count];
    memcpy(dev->addr, scan_rst->scan_rst.bda, ESP_BD_ADDR_LEN);
    dev->addr_type = scan_rst->scan_rst.ble_addr_type;
    memcpy(dev->name, name, sizeof(dev->name));
    device_count++;

    if (scan_discover || reconnect_stop) {
        return;
    }
    if (reconnect_hit(dev)) {
        reconnect_stop = true;
        esp_ble_gap_stop_scanning();
    }
}

static char hid_to_ascii(uint8_t key, uint8_t mods)
{
    bool shift = (mods & 0x22) != 0;
    if (key >= 0x04 && key <= 0x1d) {
        return (char)((shift ? 'A' : 'a') + (key - 0x04));
    }
    if (key >= 0x1e && key <= 0x26) {
        static const char *n = "!@#$%^&*(";
        return shift ? n[key - 0x1e] : (char)('1' + (key - 0x1e));
    }
    if (key == 0x27) {
        return shift ? ')' : '0';
    }
    if (key == 0x2c) {
        return ' ';
    }
    if (key == 0x28) {
        return '\n';
    }
    if (key == 0x2d) {
        return shift ? '_' : '-';
    }
    if (key == 0x2e) {
        return shift ? '+' : '=';
    }
    return 0;
}

#if BT_DEBUG
static bool report_is_zero(const uint8_t *data, uint16_t length)
{
    for (uint16_t i = 0; i < length; i++) {
        if (data[i] != 0) {
            return false;
        }
    }
    return true;
}
#endif

/* Show a report that arrived but may not be a boot-keyboard packet. Key-up reports are all zeros. */
static void show_report(const char *kind, uint16_t report_id, const uint8_t *data, uint16_t length)
{
#if !BT_DEBUG
    (void)kind;
    (void)report_id;
    (void)data;
    (void)length;
    return;
#else
    if (data == NULL || length == 0 || report_is_zero(data, length)) {
        return;
    }
    char line[LCD_TEXT_COLS + 1];
    int used = snprintf(line, sizeof(line), "%s %u", kind, report_id);
    int shown = length > 8 ? 8 : (int)length;
    for (int i = 0; i < shown && used > 0 && used < (int)sizeof(line) - 4; i++) {
        used += snprintf(line + used, sizeof(line) - (size_t)used, " %02X", data[i]);
    }
    if (used > 0) {
        trace_print(line);
    }
#endif
}

static void paint_key_draft(void)
{
    char shown[LCD_TEXT_COLS + 1];
    int room = LCD_TEXT_COLS - 1;
    int start = 0;
    int n = key_len;
    if (n > room) {
        start = n - room;
        n = room;
    }
    shown[0] = '>';
    memcpy(shown + 1, key_line + start, (size_t)n);
    shown[n + 1] = '\0';
    if (panel != NULL) {
        lcd_text_set_draft(panel, shown);
    }
}

static void recall_key_line(void)
{
    char recalled[KEY_LINE_MAX + 1];
    if (!command_history_up(&key_hist, recalled, sizeof(recalled))) {
        return;
    }
    key_len = 0;
    while (recalled[key_len] != '\0' && key_len < KEY_LINE_MAX) {
        key_line[key_len] = recalled[key_len];
        key_len++;
    }
    paint_key_draft();
}

static bool key_is_held(uint8_t key)
{
    for (int i = 0; i < 6; i++) {
        if (keys_held[i] == key) {
            return true;
        }
    }
    return false;
}

static void show_keys(const uint8_t *data, uint16_t length)
{
    if (data == NULL || length < 3) {
        return;
    }
    /* Boot keyboard: modifiers, reserved, then up to 6 keycodes. Some reports prepend an ID. */
    int offset = (length >= 9) ? 1 : 0;
    if (length < (uint16_t)(offset + 3)) {
        return;
    }
    uint8_t mods = data[offset];
    uint8_t down[6] = {0};
    int down_count = 0;
    bool dirty = false;

    for (int i = offset + 2; i < length && down_count < 6; i++) {
        uint8_t key = data[i];
        bool seen = false;
        if (key == 0 || key == 1) {
            continue;
        }
        for (int j = 0; j < down_count; j++) {
            if (down[j] == key) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        down[down_count++] = key;
        if (key_is_held(key)) {
            continue;
        }
        if (key == 0x28 || key == 0x58) {
            submit_key_line();
            continue;
        }
        if (key == 0x52) {
            recall_key_line();
            continue;
        }
        if (key == 0x2a) {
            if (key_len > 0) {
                key_len--;
                dirty = true;
            }
            continue;
        }
        char ch = hid_to_ascii(key, mods);
        if (ch == '\n') {
            submit_key_line();
        } else if (ch != 0 && key_len < KEY_LINE_MAX) {
            key_line[key_len++] = ch;
            dirty = true;
        }
    }

    if (dirty) {
        paint_key_draft();
    }

    memset(keys_held, 0, sizeof(keys_held));
    if (down_count > 0) {
        memcpy(keys_held, down, (size_t)down_count);
    }
}

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT:
        /* Report map is readable here. Notifications are not enabled yet, so this is not the working state. */
        if (param->open.status != ESP_OK) {
            trace_print("connect failed");
        }
        break;
    case ESP_HIDH_BATTERY_EVENT:
        /* Keyboard battery notifications are periodic; keep them off the LCD. */
        break;
    case ESP_HIDH_INPUT_EVENT:
        show_report("in", param->input.report_id, param->input.data, param->input.length);
        show_keys(param->input.data, param->input.length);
        break;
    case ESP_HIDH_FEATURE_EVENT:
        show_report("feat", param->feature.report_id, param->feature.data, param->feature.length);
        break;
    case ESP_HIDH_CLOSE_EVENT:
        if (param->close.dev == active_dev) {
            active_dev = NULL;
            active_connected = false;
        }
        memset(keys_held, 0, sizeof(keys_held));
        /* The stack frees the device after this callback returns. */
        if (waiting_close && close_done != NULL) {
            waiting_close = false;
            xSemaphoreGive(close_done);
        } else {
            trace_print("disconnected");
        }
        break;
    default:
        break;
    }
}

static esp_err_t set_security(void)
{
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;

    esp_err_t err = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
    if (err != ESP_OK) {
        return err;
    }
    return esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
}

static void gattc_tap(esp_gattc_cb_event_t event, esp_gatt_if_t iface, esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT && param->reg.status == ESP_GATT_OK) {
        gatt_if = iface;
    }
    if (event == ESP_GATTC_NOTIFY_EVT && param->notify.value != NULL && param->notify.value_len > 1) {
        show_report("nt", param->notify.handle, param->notify.value, param->notify.value_len);
    }
    if (own_gatt_op && gatt_op_done != NULL &&
        (event == ESP_GATTC_REG_FOR_NOTIFY_EVT || event == ESP_GATTC_WRITE_DESCR_EVT || event == ESP_GATTC_WRITE_CHAR_EVT)) {
        gatt_op_status = (event == ESP_GATTC_REG_FOR_NOTIFY_EVT) ? param->reg_for_notify.status : param->write.status;
        own_gatt_op = false;
        xSemaphoreGive(gatt_op_done);
        /* Do not forward. The HID stack would leave its wait semaphore set. */
        return;
    }
    esp_hidh_gattc_event_handler(event, iface, param);
}

static bool gatt_begin(void)
{
    if (gatt_op_done == NULL) {
        gatt_op_done = xSemaphoreCreateBinary();
        if (gatt_op_done == NULL) {
            return false;
        }
    }
    xSemaphoreTake(gatt_op_done, 0);
    own_gatt_op = true;
    return true;
}

static bool gatt_finish(esp_err_t err)
{
    if (err != ESP_OK) {
        own_gatt_op = false;
        return false;
    }
    if (xSemaphoreTake(gatt_op_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        own_gatt_op = false;
        return false;
    }
    return gatt_op_status == ESP_GATT_OK;
}

static bool subscribe_input(esp_hidh_dev_t *dev, uint16_t value_handle, uint16_t ccc_handle)
{
    uint8_t ccc[2] = {0x01, 0x00};
    if (!gatt_begin()) {
        return false;
    }
    if (!gatt_finish(esp_ble_gattc_register_for_notify(gatt_if, dev->addr.bda, value_handle))) {
        return false;
    }
    if (!gatt_begin()) {
        return false;
    }
    return gatt_finish(esp_ble_gattc_write_char_descr(gatt_if, dev->ble.conn_id, ccc_handle, sizeof(ccc), ccc,
                                                      ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NO_MITM));
}

/* Report map is already in hand. Write each input CCCD, then the caller may enter the working state. */
static bool enable_notifications(esp_hidh_dev_t *dev)
{
    int total = 0;
    int enabled = 0;
    char line[LCD_TEXT_COLS + 1];

    if (gatt_if == 0 || dev == NULL) {
        trace_print("no gatt");
        return false;
    }

    esp_bt_uuid_t hid_uuid = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = ESP_GATT_UUID_HID_SVC};
    esp_gattc_service_elem_t service;
    uint16_t count = 1;
    if (esp_ble_gattc_get_service(gatt_if, dev->ble.conn_id, &hid_uuid, &service, &count, 0) == ESP_GATT_OK && count > 0) {
        esp_bt_uuid_t proto_uuid = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = ESP_GATT_UUID_HID_PROTO_MODE};
        esp_gattc_char_elem_t proto;
        uint16_t n = 1;
        if (esp_ble_gattc_get_char_by_uuid(gatt_if, dev->ble.conn_id, service.start_handle, service.end_handle, proto_uuid, &proto, &n) == ESP_GATT_OK && n > 0) {
            uint8_t mode = ESP_HID_PROTOCOL_MODE_REPORT;
            if (gatt_begin()) {
                gatt_finish(esp_ble_gattc_write_char(gatt_if, dev->ble.conn_id, proto.char_handle, 1, &mode,
                                                     ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NO_MITM));
            }
        }
    }

    for (esp_hidh_dev_report_t *report = dev->reports; report != NULL; report = report->next) {
        if (report->report_type != ESP_HID_REPORT_TYPE_INPUT) {
            continue;
        }
        if ((report->permissions & ESP_GATT_CHAR_PROP_BIT_NOTIFY) == 0 || report->ccc_handle == 0) {
            continue;
        }
        total++;
        if (subscribe_input(dev, report->handle, report->ccc_handle)) {
            enabled++;
        }
    }

#if BT_DEBUG
    snprintf(line, sizeof(line), "cccd %d/%d", enabled, total);
    trace_print(line);
#else
    (void)line;
#endif
    return enabled > 0;
}

static esp_err_t hid_host_init(void)
{
    if (hid_ready) {
        return ESP_OK;
    }
    esp_err_t err = esp_ble_gattc_register_callback(gattc_tap);
    if (err != ESP_OK) {
        return err;
    }
    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    err = esp_hidh_init(&config);
    if (err != ESP_OK) {
        return err;
    }
    hid_ready = true;
    return ESP_OK;
}

static void draw_device_page(esp_lcd_panel_handle_t lcd, const button_choice_t *choice, void *ctx)
{
    (void)ctx;
    char line[LCD_TEXT_COLS + 1];
    snprintf(line, sizeof(line), "KEY1-3 pick  page %d/%d", choice->page + 1, choice->page_count);
    lcd_text_write_row(lcd, 0, line);
    for (int slot = 0; slot < BUTTON_CHOICE_SLOTS; slot++) {
        int index = choice->page * BUTTON_CHOICE_SLOTS + slot;
        if (index >= 0 && index < choice->item_count && index < MAX_DEVICES) {
            snprintf(line, sizeof(line), "%d %.36s", slot + 1, devices[index].name);
        } else {
            line[0] = '\0';
        }
        lcd_text_write_row(lcd, 1 + slot, line);
    }
    lcd_text_write_row(lcd, 4, "KEY4 next page");
    for (int row = 5; row < LCD_TEXT_ROWS; row++) {
        lcd_text_write_row(lcd, row, "");
    }
    lcd_text_set_screen_lines(5);
}

static void pick_device(esp_lcd_panel_handle_t lcd, const button_choice_t *choice, void *ctx)
{
    (void)ctx;
    if (choice->index >= 0) {
        bluetooth_connect(lcd, choice->index + 1);
    }
}

static void connect_task(void *arg)
{
    bluetooth_connect(panel, (int)(intptr_t)arg);
    vTaskDelete(NULL);
}

static int reconnect_index(void)
{
    int name_hit = -1;
    int known_hit = -1;

    for (int i = 0; i < device_count; i++) {
        if (!addr_is_zero(active_addr) && memcmp(devices[i].addr, active_addr, ESP_BD_ADDR_LEN) == 0) {
            return i;
        }
        if (name_matches_active(devices[i].name) && name_hit < 0) {
            name_hit = i;
        }
        if (is_known(devices[i].addr) && known_hit < 0) {
            known_hit = i;
        }
    }
    if (name_hit >= 0) {
        return name_hit;
    }
    if (known_hit >= 0) {
        return known_hit;
    }
    /* One HID advertising and we already have bonds: use that advertising address. */
    if (device_count == 1 && known_count > 0) {
        return 0;
    }
    return -1;
}

static void finish_scan(void)
{
    scanning = false;
    if (!scan_discover) {
        int index = reconnect_index();
        if (index < 0) {
            trace_print("no paired device");
            return;
        }
        if (xTaskCreate(connect_task, "bt-connect", 8192, (void *)(intptr_t)(index + 1), 5, NULL) != pdPASS) {
            trace_print("connect failed");
        }
        return;
    }

    if (device_count == 0) {
        trace_print("no input device");
        return;
    }
    if (button_choice_start(device_count, pick_device, draw_device_page, NULL) != ESP_OK) {
        trace_print("connect <n>");
    }
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        params_ready = true;
        if (want_scan) {
            want_scan = false;
            if (esp_ble_gap_start_scanning(SCAN_SECONDS) != ESP_OK) {
                scanning = false;
                trace_print("scan failed");
            }
        }
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            scanning = false;
            trace_print("scan failed");
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            show_device(param);
        } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            finish_scan();
        }
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: {
        char line[LCD_TEXT_COLS + 1];
        snprintf(line, sizeof(line), "passkey %lu", (unsigned long)param->ble_security.key_notif.passkey);
        trace_print(line);
        break;
    }
    case ESP_GAP_BLE_NC_REQ_EVT: {
        char line[LCD_TEXT_COLS + 1];
        snprintf(line, sizeof(line), "confirm %lu", (unsigned long)param->ble_security.key_notif.passkey);
        trace_print(line);
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;
    }
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        trace_print(param->ble_security.auth_cmpl.success ? "paired" : "pair failed");
        break;
    default:
        break;
    }
}

static esp_err_t stack_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    trace_printf("controller init, internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    trace_print("controller enable");
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        return err;
    }
    trace_print("controller enabled");
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_gap_register_callback(gap_cb);
    if (err != ESP_OK) {
        return err;
    }
    err = set_security();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_gap_config_local_privacy(true);
    if (err != ESP_OK) {
        return err;
    }
    err = hid_host_init();
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

void bluetooth_rf_reset(void)
{
    esp_wifi_bt_power_domain_off();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_wifi_bt_power_domain_on();
}

static bool try_direct_reconnect(void)
{
    esp_bd_addr_t addr = {0};
    esp_ble_addr_type_t addr_type = BLE_ADDR_TYPE_PUBLIC;
    char name[DEV_NAME_MAX];

    name[0] = '\0';
    if (!addr_is_zero(active_addr)) {
        memcpy(addr, active_addr, ESP_BD_ADDR_LEN);
        addr_type = active_addr_type;
        memcpy(name, active_name, sizeof(name));
    } else {
        int bonds = esp_ble_get_bond_device_num();
        esp_ble_bond_dev_t *list;
        if (bonds <= 0) {
            return false;
        }
        list = calloc((size_t)bonds, sizeof(*list));
        if (list == NULL || esp_ble_get_bond_device_list(&bonds, list) != ESP_OK || bonds <= 0) {
            free(list);
            return false;
        }
        memcpy(addr, list[0].bd_addr, ESP_BD_ADDR_LEN);
        addr_type = list[0].bd_addr_type;
        free(list);
    }

    if (addr_is_zero(addr)) {
        return false;
    }
    if (name[0] == '\0') {
        snprintf(name, sizeof(name), "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    }

    device_count = 1;
    memcpy(devices[0].addr, addr, ESP_BD_ADDR_LEN);
    devices[0].addr_type = addr_type;
    memcpy(devices[0].name, name, sizeof(devices[0].name));
    devices[0].name[sizeof(devices[0].name) - 1] = '\0';
    return bluetooth_connect(panel, 1) == ESP_OK;
}

static void run_scan(void)
{
    if (!stack_ready) {
        bluetooth_rf_reset();
        esp_err_t err = stack_init();
        if (err != ESP_OK) {
            scanning = false;
            trace_printf("init %s", esp_err_to_name(err));
            trace_print("bt init failed");
            return;
        }
        stack_ready = true;
    }

    device_count = 0;
    reconnect_stop = false;
    load_known();

    /* Reconnect ads often omit HID UUID. Try the last working address first, then scan. */
    if (!scan_discover) {
        if (try_direct_reconnect()) {
            scanning = false;
            return;
        }
        device_count = 0;
        trace_print("reconnect");
    }

    if (!params_ready) {
        /* Set this before the scan-parameter command, or the completion callback can miss it. */
        want_scan = true;
        if (esp_ble_gap_set_scan_params(&scan_params) != ESP_OK) {
            scanning = false;
            trace_print("scan failed");
        }
        return;
    }
    if (esp_ble_gap_start_scanning(SCAN_SECONDS) != ESP_OK) {
        scanning = false;
        trace_print("scan failed");
    }
}

static void scan_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        run_scan();
    }
}

static esp_err_t start_scan(esp_lcd_panel_handle_t lcd, bool discover)
{
    if (lcd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    panel = lcd;

    button_choice_stop();
    if (scanning) {
        trace_print("scanning");
        return ESP_OK;
    }
    if (scan_task == NULL) {
        if (xTaskCreate(scan_task_fn, "bt-scan", 16384, NULL, 5, &scan_task) != pdPASS) {
            trace_print("bt init failed");
            return ESP_ERR_NO_MEM;
        }
    }
    scan_discover = discover;
    scanning = true;
    trace_print(discover ? "discover" : "reconnect");
    xTaskNotifyGive(scan_task);
    return ESP_OK;
}

esp_err_t bluetooth_reconnect(esp_lcd_panel_handle_t lcd)
{
    return start_scan(lcd, false);
}

esp_err_t bluetooth_discover(esp_lcd_panel_handle_t lcd)
{
    return start_scan(lcd, true);
}

static bool link_is_up(void)
{
    if (!active_connected || active_dev == NULL || !esp_hidh_dev_exists(active_dev)) {
        active_dev = NULL;
        active_connected = false;
        return false;
    }
    return true;
}

static void remember_link(esp_hidh_dev_t *dev, const found_device_t *found)
{
    active_dev = dev;
    active_connected = true;
    memcpy(active_addr, found->addr, ESP_BD_ADDR_LEN);
    active_addr_type = found->addr_type;
    memcpy(active_name, found->name, sizeof(active_name));
    active_name[sizeof(active_name) - 1] = '\0';
    save_peer_nvs(found);
}

/* Close the current HID link and wait until the stack has released it. */
static bool drop_link(void)
{
    esp_hidh_dev_t *dev = active_dev;
    if (!link_is_up()) {
        return true;
    }
    if (close_done == NULL) {
        close_done = xSemaphoreCreateBinary();
        if (close_done == NULL) {
            return false;
        }
    }
    xSemaphoreTake(close_done, 0);
    waiting_close = true;
    if (esp_hidh_dev_close(dev) != ESP_OK) {
        waiting_close = false;
        if (!esp_hidh_dev_exists(dev)) {
            active_dev = NULL;
            active_connected = false;
            return true;
        }
        return false;
    }
    if (xSemaphoreTake(close_done, pdMS_TO_TICKS(3000)) != pdTRUE && link_is_up()) {
        waiting_close = false;
        return false;
    }
    return true;
}

esp_err_t bluetooth_connect(esp_lcd_panel_handle_t lcd, int index)
{
    if (lcd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    panel = lcd;
    button_choice_stop();

    if (!stack_ready) {
        trace_print("scan first");
        return ESP_ERR_INVALID_STATE;
    }
    if (index < 1 || index > device_count) {
        trace_print("connect <n>");
        return ESP_ERR_INVALID_ARG;
    }

    if (scanning) {
        esp_ble_gap_stop_scanning();
        scanning = false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    esp_err_t err = hid_host_init();
    if (err != ESP_OK) {
        trace_print("hid init failed");
        return err;
    }

    found_device_t *dev = &devices[index - 1];
    char line[LCD_TEXT_COLS + 1];
    if (link_is_up() && memcmp(active_addr, dev->addr, ESP_BD_ADDR_LEN) == 0) {
        snprintf(line, sizeof(line), "connected %.29s", dev->name);
        trace_print(line);
        return ESP_OK;
    }
    if (link_is_up()) {
        snprintf(line, sizeof(line), "disconnect %.28s", active_name);
        trace_print(line);
        if (!drop_link()) {
            trace_print("disconnect failed");
            return ESP_FAIL;
        }
    }

    snprintf(line, sizeof(line), "connecting %.28s", dev->name);
    trace_print(line);

    esp_hidh_dev_t *opened = esp_hidh_dev_open(dev->addr, ESP_HID_TRANSPORT_BLE, dev->addr_type);
    if (opened == NULL) {
        trace_print("open failed");
        return ESP_FAIL;
    }
    /* open() has finished service discovery and the report-map read. */
#if BT_DEBUG
    trace_print("report map");
    esp_hidh_dev_dump(opened, stdout);
    fflush(stdout);
#endif
    remember_link(opened, dev);
    if (!enable_notifications(opened)) {
        trace_print("cccd failed");
        return ESP_FAIL;
    }
    snprintf(line, sizeof(line), "ready %.33s", dev->name);
    trace_print(line);
    return ESP_OK;
}

static void cmd_connect(esp_lcd_panel_handle_t panel, int argc, char **argv)
{
    if (argc < 2) {
        trace_print("connect <n>");
        return;
    }
    int index = 0;
    for (const char *p = argv[1]; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            trace_print("connect <n>");
            return;
        }
        index = index * 10 + (*p - '0');
    }
    bluetooth_connect(panel, index);
}

void bluetooth_register_commands(void)
{
    command_register("connect", "connect device n", cmd_connect);
}
