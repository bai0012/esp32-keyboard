#include "hid_ble_backend.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "keymap_config.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_log.h"

#define TAG "HID_BLE"

#define BLE_REPORT_ID_KEYBOARD 1
#define BLE_REPORT_ID_CONSUMER 2

#define ADV_CFG_FLAG_RAW 0x01
#define ADV_CFG_FLAG_SCAN_RSP 0x02
#define BLE_FALLBACK_DEVICE_NAME "ESP32 MacroPad BLE"
#define BLE_DEVICE_NAME_MAX_LEN 31
#define BLE_SCAN_RSP_NAME_MAX_LEN 20

typedef struct {
    bool initialized;
    bool connected;
    bool advertising;
    bool bonded;
    bool pairing_window_active;
    TickType_t pairing_deadline_tick;
    uint32_t passkey;
    bool adv_ready;
    uint8_t adv_cfg_done;
    uint8_t adv_cfg_required_mask;
    bool adv_start_requested;
    esp_hidd_dev_t *hid_dev;
    SemaphoreHandle_t lock;
    char device_name[BLE_DEVICE_NAME_MAX_LEN + 1];
    char peer_addr[18];
} hid_ble_ctx_t;

static hid_ble_ctx_t s_ble = {0};
static const char *s_last_init_step = "idle";
static esp_err_t s_last_init_error = ESP_OK;

/* Keyboard report (ID=1) + Consumer report (ID=2, 16-bit usage). */
static const uint8_t s_ble_report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, BLE_REPORT_ID_KEYBOARD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x05,
    0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00,
    0xC0,

    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, BLE_REPORT_ID_CONSUMER,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A,
    0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

static esp_hid_raw_report_map_t s_ble_report_maps[] = {
    {
        .data = s_ble_report_map,
        .len = sizeof(s_ble_report_map),
    },
};

static uint8_t s_adv_service_uuid16[2] = {0x12, 0x18};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = ESP_HID_APPEARANCE_KEYBOARD,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(s_adv_service_uuid16),
    .p_service_uuid = s_adv_service_uuid16,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t s_scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    .appearance = ESP_HID_APPEARANCE_KEYBOARD,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static inline void ble_lock(void)
{
    if (s_ble.lock != NULL) {
        (void)xSemaphoreTake(s_ble.lock, portMAX_DELAY);
    }
}

static inline void ble_unlock(void)
{
    if (s_ble.lock != NULL) {
        (void)xSemaphoreGive(s_ble.lock);
    }
}

static void format_bda(const esp_bd_addr_t bda, char out[18])
{
    (void)snprintf(out,
                   18,
                   "%02X:%02X:%02X:%02X:%02X:%02X",
                   bda[0],
                   bda[1],
                   bda[2],
                   bda[3],
                   bda[4],
                   bda[5]);
}

static void ble_set_bonded_locked(bool bonded)
{
    s_ble.bonded = bonded;
    if (!bonded) {
        s_ble.peer_addr[0] = '\0';
    }
}

static void ble_refresh_bonded_locked(void)
{
    const int num = esp_ble_get_bond_device_num();
    if (num <= 0) {
        ble_set_bonded_locked(false);
        return;
    }

    int list_count = num;
    esp_ble_bond_dev_t *list = calloc((size_t)list_count, sizeof(esp_ble_bond_dev_t));
    if (list == NULL) {
        return;
    }
    if (esp_ble_get_bond_device_list(&list_count, list) == ESP_OK && list_count > 0) {
        ble_set_bonded_locked(true);
        format_bda(list[0].bd_addr, s_ble.peer_addr);
    } else {
        ble_set_bonded_locked(false);
    }
    free(list);
}

static void ble_remove_all_other_bonds(const esp_bd_addr_t keep_bda)
{
    int num = esp_ble_get_bond_device_num();
    if (num <= 0) {
        return;
    }

    esp_ble_bond_dev_t *list = calloc((size_t)num, sizeof(esp_ble_bond_dev_t));
    if (list == NULL) {
        return;
    }
    int list_count = num;
    if (esp_ble_get_bond_device_list(&list_count, list) == ESP_OK) {
        for (int i = 0; i < list_count; ++i) {
            if (memcmp(list[i].bd_addr, keep_bda, sizeof(esp_bd_addr_t)) != 0) {
                (void)esp_ble_remove_bond_device(list[i].bd_addr);
            }
        }
    }
    free(list);
}

static void ble_prepare_device_name(const char *configured_name, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    const char *src = configured_name;
    if (src == NULL || src[0] == '\0') {
        src = BLE_FALLBACK_DEVICE_NAME;
        ESP_LOGW(TAG, "BLE device name is empty, fallback to \"%s\"", src);
    }

    strlcpy(out, src, out_size);
    const size_t src_len = strlen(src);
    if (src_len >= out_size) {
        ESP_LOGW(TAG, "BLE device name too long, truncated to \"%s\"", out);
    }
}

static void ble_set_init_diag(const char *step, esp_err_t err)
{
    s_last_init_step = step;
    s_last_init_error = err;
}

static void ble_start_adv_if_possible(void)
{
    bool do_start = false;
    ble_lock();
    if (s_ble.initialized && s_ble.adv_ready && s_ble.adv_start_requested &&
        !s_ble.connected && !s_ble.advertising) {
        do_start = true;
    }
    ble_unlock();

    if (do_start) {
        esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_ble_gap_start_advertising failed: %s", esp_err_to_name(err));
        }
    }
}

static void ble_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ble_lock();
        s_ble.adv_cfg_done |= ADV_CFG_FLAG_RAW;
        s_ble.adv_ready = ((s_ble.adv_cfg_done & s_ble.adv_cfg_required_mask) ==
                           s_ble.adv_cfg_required_mask);
        ble_unlock();
        ble_start_adv_if_possible();
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        ble_lock();
        s_ble.adv_cfg_done |= ADV_CFG_FLAG_SCAN_RSP;
        s_ble.adv_ready = ((s_ble.adv_cfg_done & s_ble.adv_cfg_required_mask) ==
                           s_ble.adv_cfg_required_mask);
        ble_unlock();
        ble_start_adv_if_possible();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        ble_lock();
        s_ble.advertising = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        ble_unlock();
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ble_lock();
        s_ble.advertising = false;
        ble_unlock();
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        (void)esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        (void)esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, s_ble.passkey);
        break;
    case ESP_GAP_BLE_NC_REQ_EVT:
        (void)esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ble_lock();
            ble_set_bonded_locked(true);
            format_bda(param->ble_security.auth_cmpl.bd_addr, s_ble.peer_addr);
            ble_unlock();
            ble_remove_all_other_bonds(param->ble_security.auth_cmpl.bd_addr);
            ESP_LOGI(TAG, "BLE bond/auth success with %s", s_ble.peer_addr);
        } else {
            ESP_LOGW(TAG, "BLE auth failed reason=0x%X", param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    default:
        break;
    }
}

static void ble_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ble_start_adv_if_possible();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ble_lock();
        s_ble.connected = true;
        s_ble.advertising = false;
        /* Connected sessions should not keep showing pairing countdown overlay. */
        s_ble.pairing_window_active = false;
        s_ble.pairing_deadline_tick = 0;
        ble_unlock();
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ble_lock();
        s_ble.connected = false;
        s_ble.advertising = false;
        ble_unlock();
        ble_start_adv_if_possible();
        ESP_LOGI(TAG, "BLE disconnected: reason=%d", param->disconnect.reason);
        break;
    default:
        break;
    }
}

static esp_err_t ble_setup_security(uint32_t passkey)
{
    /* Prefer broad host compatibility. MitM/display-only pairing can be enabled later by policy. */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_err_t err = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set auth req failed: %s", esp_err_to_name(err));
    }
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set iocap failed: %s", esp_err_to_name(err));
    }
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set init key failed: %s", esp_err_to_name(err));
    }
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set rsp key failed: %s", esp_err_to_name(err));
    }
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set key size failed: %s", esp_err_to_name(err));
    }
    err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(passkey));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set static passkey failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "security configured auth_req=%u iocap=%u passkey=%06" PRIu32, (unsigned)auth_req, (unsigned)iocap, passkey);
    return ESP_OK;
}

esp_err_t hid_ble_backend_init(const char *device_name, uint32_t passkey)
{
    ble_set_init_diag("begin", ESP_OK);

    if (passkey > 999999UL) {
        ESP_LOGW(TAG, "BLE passkey %" PRIu32 " out of range, fallback to 123456", passkey);
        passkey = 123456UL;
    }

    if (s_ble.initialized) {
        return ESP_OK;
    }

    memset(&s_ble, 0, sizeof(s_ble));
    s_ble.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ble.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");
    s_ble.passkey = passkey;
    ble_prepare_device_name(device_name, s_ble.device_name, sizeof(s_ble.device_name));

    ble_set_init_diag("bt_mem_release", ESP_OK);
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE &&
        err != ESP_ERR_NOT_FOUND &&
        err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "esp_bt_controller_mem_release(CLASSIC_BT) ignored: %s", esp_err_to_name(err));
    }

    ble_set_init_diag("bt_controller_init", ESP_OK);
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ble_set_init_diag("bt_controller_init", err);
        return err;
    }

    ble_set_init_diag("bt_controller_enable", ESP_OK);
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ble_set_init_diag("bt_controller_enable", err);
        return err;
    }

    ble_set_init_diag("bluedroid_init", ESP_OK);
    err = esp_bluedroid_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ble_set_init_diag("bluedroid_init", err);
        return err;
    }
    ble_set_init_diag("bluedroid_enable", ESP_OK);
    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ble_set_init_diag("bluedroid_enable", err);
        return err;
    }

    ble_set_init_diag("gap_cb_register", ESP_OK);
    err = esp_ble_gap_register_callback(ble_gap_event_handler);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gap cb reg failed (continue): %s", esp_err_to_name(err));
    }
    ble_set_init_diag("gatts_cb_register", ESP_OK);
    err = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gatts cb reg failed (continue): %s", esp_err_to_name(err));
    }
    (void)ble_setup_security(passkey);

    ble_set_init_diag("set_device_name", ESP_OK);
    err = esp_ble_gap_set_device_name(s_ble.device_name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set ble device name failed (continue): %s", esp_err_to_name(err));
    }

    s_ble.adv_cfg_done = 0;
    s_ble.adv_cfg_required_mask = ADV_CFG_FLAG_RAW;
    s_ble.adv_ready = false;
    ble_set_init_diag("config_adv_data", ESP_OK);
    err = esp_ble_gap_config_adv_data(&s_adv_data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config adv data failed (continue): %s", esp_err_to_name(err));
        s_ble.adv_cfg_done |= ADV_CFG_FLAG_RAW;
    }
    esp_ble_adv_data_t scan_rsp_cfg = s_scan_rsp_data;
    if (strlen(s_ble.device_name) > BLE_SCAN_RSP_NAME_MAX_LEN) {
        scan_rsp_cfg.include_name = false;
        ESP_LOGW(TAG,
                 "BLE device name too long for scan response; name omitted from scan response");
    }
    ble_set_init_diag("config_scan_rsp", ESP_OK);
    const esp_err_t scan_rsp_err = esp_ble_gap_config_adv_data(&scan_rsp_cfg);
    if (scan_rsp_err == ESP_OK) {
        s_ble.adv_cfg_required_mask |= ADV_CFG_FLAG_SCAN_RSP;
    } else {
        ESP_LOGW(TAG, "scan rsp adv-data disabled due to: %s", esp_err_to_name(scan_rsp_err));
        s_ble.adv_cfg_done |= ADV_CFG_FLAG_SCAN_RSP;
    }

    ble_set_init_diag("hidd_dev_init", ESP_OK);
    esp_hid_device_config_t hid_cfg = {
        .vendor_id = 0x303A,
        .product_id = 0x4011,
        .version = 0x0101,
        .device_name = s_ble.device_name,
        .manufacturer_name = "Espressif",
        .serial_number = "123456",
        .report_maps = s_ble_report_maps,
        .report_maps_len = (uint8_t)(sizeof(s_ble_report_maps) / sizeof(s_ble_report_maps[0])),
    };
    err = esp_hidd_dev_init(&hid_cfg, ESP_HID_TRANSPORT_BLE, ble_hidd_event_callback, &s_ble.hid_dev);
    if (err != ESP_OK) {
        ble_set_init_diag("hidd_dev_init", err);
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_lock();
    s_ble.initialized = true;
    s_ble.adv_start_requested = false;
    ble_refresh_bonded_locked();
    ble_unlock();

    ble_set_init_diag("ready", ESP_OK);
    ESP_LOGI(TAG, "ready name=%s passkey=%06" PRIu32, s_ble.device_name, passkey);
    return ESP_OK;
}

void hid_ble_backend_poll(TickType_t now)
{
    bool should_stop_adv = false;

    ble_lock();
    if (s_ble.pairing_window_active && s_ble.pairing_deadline_tick != 0 &&
        now >= s_ble.pairing_deadline_tick) {
        s_ble.pairing_window_active = false;
        s_ble.pairing_deadline_tick = 0;
        if (!s_ble.bonded) {
            s_ble.adv_start_requested = false;
            should_stop_adv = s_ble.advertising;
        }
        ESP_LOGI(TAG, "pairing window expired");
    }
    ble_unlock();

    if (should_stop_adv) {
        (void)esp_ble_gap_stop_advertising();
    }
}

esp_err_t hid_ble_backend_send_keyboard_report(const bool *key_pressed, uint8_t active_layer)
{
    if (key_pressed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ble_lock();
    esp_hidd_dev_t *dev = s_ble.hid_dev;
    const bool can_send = s_ble.connected && (dev != NULL);
    ble_unlock();
    if (!can_send) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t report[8] = {0};
    size_t report_index = 2;
    for (size_t i = 0; i < MACRO_KEY_COUNT && report_index < sizeof(report); ++i) {
        const macro_action_config_t *cfg = &g_macro_keymap_layers[active_layer][i];
        if (key_pressed[i] && cfg->type == MACRO_ACTION_KEYBOARD) {
            report[report_index++] = (uint8_t)cfg->usage;
        }
    }
    return esp_hidd_dev_input_set(dev, 0, BLE_REPORT_ID_KEYBOARD, report, sizeof(report));
}

esp_err_t hid_ble_backend_send_consumer_report(uint16_t usage)
{
    ble_lock();
    esp_hidd_dev_t *dev = s_ble.hid_dev;
    const bool can_send = s_ble.connected && (dev != NULL);
    ble_unlock();
    if (!can_send) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t report[2] = {
        (uint8_t)(usage & 0xFFU),
        (uint8_t)((usage >> 8) & 0xFFU),
    };
    ESP_RETURN_ON_ERROR(esp_hidd_dev_input_set(dev, 0, BLE_REPORT_ID_CONSUMER, report, sizeof(report)),
                        TAG,
                        "consumer press send failed");
    vTaskDelay(pdMS_TO_TICKS(12));

    uint8_t release[2] = {0, 0};
    return esp_hidd_dev_input_set(dev, 0, BLE_REPORT_ID_CONSUMER, release, sizeof(release));
}

esp_err_t hid_ble_backend_start_pairing_window(uint32_t timeout_ms)
{
    if (!s_ble.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ble_lock();
    s_ble.pairing_window_active = true;
    s_ble.pairing_deadline_tick = (timeout_ms > 0) ? (xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms)) : 0;
    s_ble.adv_start_requested = true;
    ble_unlock();

    ble_start_adv_if_possible();
    ESP_LOGI(TAG, "pairing window started timeout_ms=%" PRIu32, timeout_ms);
    return ESP_OK;
}

esp_err_t hid_ble_backend_clear_bond(void)
{
    const int num = esp_ble_get_bond_device_num();
    if (num <= 0) {
        ble_lock();
        ble_set_bonded_locked(false);
        ble_unlock();
        return ESP_OK;
    }

    esp_ble_bond_dev_t *list = calloc((size_t)num, sizeof(esp_ble_bond_dev_t));
    if (list == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int list_count = num;
    esp_err_t err = esp_ble_get_bond_device_list(&list_count, list);
    if (err == ESP_OK) {
        for (int i = 0; i < list_count; ++i) {
            (void)esp_ble_remove_bond_device(list[i].bd_addr);
        }
    }
    free(list);

    ble_lock();
    ble_set_bonded_locked(false);
    ble_unlock();
    return err;
}

bool hid_ble_backend_is_ready(void)
{
    ble_lock();
    const bool ready = s_ble.initialized && s_ble.connected && s_ble.hid_dev != NULL;
    ble_unlock();
    return ready;
}

void hid_ble_backend_get_status(hid_ble_backend_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    ble_lock();
    out_status->initialized = s_ble.initialized;
    out_status->connected = s_ble.connected;
    out_status->advertising = s_ble.advertising;
    out_status->bonded = s_ble.bonded;
    out_status->pairing_window_active = s_ble.pairing_window_active;
    out_status->passkey = s_ble.passkey;
    strlcpy(out_status->peer_addr, s_ble.peer_addr, sizeof(out_status->peer_addr));

    if (s_ble.pairing_window_active && s_ble.pairing_deadline_tick != 0) {
        const TickType_t now = xTaskGetTickCount();
        if (now < s_ble.pairing_deadline_tick) {
            out_status->pairing_remaining_ms =
                (uint32_t)pdTICKS_TO_MS(s_ble.pairing_deadline_tick - now);
        }
    }
    ble_unlock();
}

const char *hid_ble_backend_last_init_step(void)
{
    return s_last_init_step;
}

esp_err_t hid_ble_backend_last_init_error(void)
{
    return s_last_init_error;
}
