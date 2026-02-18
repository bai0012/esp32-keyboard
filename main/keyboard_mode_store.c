#include "keyboard_mode_store.h"

#include "nvs.h"

#define NVS_NS "kbd_mode"
#define NVS_KEY_MODE "mode"

esp_err_t keyboard_mode_store_load(keyboard_mode_t *out_mode, bool *out_valid)
{
    if (out_mode == NULL || out_valid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_mode = KEYBOARD_MODE_USB;
    *out_valid = false;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t raw = 0;
    err = nvs_get_u8(nvs, NVS_KEY_MODE, &raw);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    if (raw > (uint8_t)KEYBOARD_MODE_BLE) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_mode = (keyboard_mode_t)raw;
    *out_valid = true;
    return ESP_OK;
}

esp_err_t keyboard_mode_store_save(keyboard_mode_t mode)
{
    if (mode > KEYBOARD_MODE_BLE) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs, NVS_KEY_MODE, (uint8_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
