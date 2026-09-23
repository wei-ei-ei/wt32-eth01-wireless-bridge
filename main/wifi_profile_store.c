/*
 * SPDX-FileCopyrightText: 2026 WT32-ETH01 bridge contributors
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "wifi_profile_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "wifi_profiles";

#define WIFI_PROFILE_NVS_NAMESPACE "wifi_profiles"
#define WIFI_PROFILE_NVS_KEY_COUNT "count"
#define WIFI_PROFILE_NVS_KEY_ACTIVE "active"
#define WIFI_PROFILE_NVS_KEY_PREFIX "p"

static nvs_handle_t s_nvs;
static SemaphoreHandle_t s_mutex;
static bool s_initialized;
static uint16_t s_count;
static uint16_t s_active_id;

static void profile_key(uint16_t id, char key[8])
{
    snprintf(key, 8, WIFI_PROFILE_NVS_KEY_PREFIX "%04X", (unsigned)id);
}

static bool profile_is_valid(const wifi_profile_t *profile, uint16_t expected_id)
{
    return profile != NULL &&
           profile->version == WIFI_PROFILE_STRUCT_VERSION &&
           profile->struct_size == sizeof(*profile) &&
           profile->id == expected_id &&
           profile->ssid_len <= WIFI_PROFILE_SSID_MAX_LEN &&
           profile->password_len <= WIFI_PROFILE_PASSWORD_MAX_LEN;
}

static esp_err_t load_profile_unlocked(uint16_t id, wifi_profile_t *profile)
{
    char key[8];
    size_t size = sizeof(*profile);
    profile_key(id, key);

    esp_err_t err = nvs_get_blob(s_nvs, key, profile, &size);
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(*profile) || !profile_is_valid(profile, id)) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

static esp_err_t save_profile_unlocked(const wifi_profile_t *profile)
{
    char key[8];
    profile_key(profile->id, key);
    return nvs_set_blob(s_nvs, key, profile, sizeof(*profile));
}

static esp_err_t erase_profile_unlocked(uint16_t id)
{
    char key[8];
    profile_key(id, key);
    esp_err_t err = nvs_erase_key(s_nvs, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static uint16_t count_profiles_unlocked(void)
{
    uint16_t count = 0;
    wifi_profile_t profile;

    for (uint16_t id = 1; id <= WIFI_PROFILE_MAX_COUNT; ++id) {
        if (load_profile_unlocked(id, &profile) == ESP_OK) {
            ++count;
        }
    }
    return count;
}

static uint16_t find_best_profile_unlocked(void)
{
    uint16_t best_id = 0;
    uint8_t best_priority = 0;
    wifi_profile_t profile;

    for (uint16_t id = 1; id <= WIFI_PROFILE_MAX_COUNT; ++id) {
        if (load_profile_unlocked(id, &profile) == ESP_OK &&
                (best_id == 0 || profile.priority > best_priority)) {
            best_id = id;
            best_priority = profile.priority;
        }
    }
    return best_id;
}

static bool profile_ssid_equal(const wifi_profile_t *profile, const uint8_t *ssid, size_t ssid_len)
{
    return profile->ssid_len == ssid_len && memcmp(profile->ssid, ssid, ssid_len) == 0;
}

static void profile_from_sta_config(const wifi_sta_config_t *config, uint16_t id, wifi_profile_t *profile)
{
    memset(profile, 0, sizeof(*profile));
    profile->struct_size = sizeof(*profile);
    profile->version = WIFI_PROFILE_STRUCT_VERSION;
    profile->enabled = 1;
    profile->id = id;
    profile->ssid_len = strnlen((const char *)config->ssid, sizeof(config->ssid));
    profile->password_len = strnlen((const char *)config->password, sizeof(config->password));
    profile->authmode = (uint8_t)config->threshold.authmode;
    profile->channel = config->channel;
    profile->last_rssi = -127;
    profile->bssid_valid = config->bssid_set ? 1 : 0;
    memcpy(profile->ssid, config->ssid, sizeof(profile->ssid));
    memcpy(profile->password, config->password, sizeof(profile->password));
    if (config->bssid_set) {
        memcpy(profile->bssid, config->bssid, sizeof(profile->bssid));
    }
}

esp_err_t wifi_profile_store_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = nvs_open(WIFI_PROFILE_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    bool metadata_changed = false;
    err = nvs_get_u16(s_nvs, WIFI_PROFILE_NVS_KEY_COUNT, &s_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_count = count_profiles_unlocked();
        metadata_changed = true;
    } else if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }
    if (s_count > WIFI_PROFILE_MAX_COUNT) {
        s_count = WIFI_PROFILE_MAX_COUNT;
        metadata_changed = true;
    }

    err = nvs_get_u16(s_nvs, WIFI_PROFILE_NVS_KEY_ACTIVE, &s_active_id);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_active_id = 0;
        metadata_changed = true;
    } else if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    if (s_active_id != 0) {
        wifi_profile_t active;
        if (load_profile_unlocked(s_active_id, &active) != ESP_OK) {
            s_active_id = 0;
            metadata_changed = true;
        }
    }
    if (s_active_id == 0 && s_count > 0) {
        s_active_id = find_best_profile_unlocked();
        metadata_changed = metadata_changed || s_active_id != 0;
    }

    if (metadata_changed) {
        err = nvs_set_u16(s_nvs, WIFI_PROFILE_NVS_KEY_COUNT, s_count);
        if (err == ESP_OK) {
            err = nvs_set_u16(s_nvs, WIFI_PROFILE_NVS_KEY_ACTIVE, s_active_id);
        }
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs);
        }
        if (err != ESP_OK) {
            xSemaphoreGive(s_mutex);
            return err;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Loaded %u profiles, active id %u", (unsigned)s_count, (unsigned)s_active_id);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

size_t wifi_profile_store_count(void)
{
    if (!s_initialized) {
        return 0;
    }
    return s_count;
}

bool wifi_profile_store_has_active(void)
{
    return s_initialized && s_active_id != 0;
}

esp_err_t wifi_profile_store_get(uint16_t id, wifi_profile_t *profile)
{
    if (!s_initialized || profile == NULL || id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = load_profile_unlocked(id, profile);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_get_active(wifi_profile_t *profile)
{
    if (!s_initialized || profile == NULL || s_active_id == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return wifi_profile_store_get(s_active_id, profile);
}

esp_err_t wifi_profile_store_list(wifi_profile_t *profiles, size_t capacity, size_t *count)
{
    if (!s_initialized || profiles == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t found = 0;
    for (uint16_t id = 1; id <= WIFI_PROFILE_MAX_COUNT; ++id) {
        wifi_profile_t profile;
        if (load_profile_unlocked(id, &profile) != ESP_OK) {
            continue;
        }
        if (found < capacity) {
            profiles[found] = profile;
        }
        ++found;
    }

    xSemaphoreGive(s_mutex);
    *count = found;
    return found <= capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t wifi_profile_store_upsert(const wifi_sta_config_t *config, uint16_t *profile_id)
{
    if (!s_initialized || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *ssid = config->ssid;
    size_t ssid_len = strnlen((const char *)config->ssid, sizeof(config->ssid));
    if (ssid_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint16_t found_id = 0;
    wifi_profile_t existing = {0};
    for (uint16_t id = 1; id <= WIFI_PROFILE_MAX_COUNT; ++id) {
        wifi_profile_t profile;
        if (load_profile_unlocked(id, &profile) == ESP_OK &&
                profile_ssid_equal(&profile, ssid, ssid_len)) {
            found_id = id;
            existing = profile;
            break;
        }
    }

    bool new_record = false;
    if (found_id == 0) {
        if (s_count >= WIFI_PROFILE_MAX_COUNT) {
            uint8_t lowest_priority = UINT8_MAX;
            for (uint16_t id = 1; id <= WIFI_PROFILE_MAX_COUNT; ++id) {
                wifi_profile_t profile;
                if (load_profile_unlocked(id, &profile) == ESP_OK &&
                        profile.priority <= lowest_priority) {
                    lowest_priority = profile.priority;
                    found_id = id;
                }
            }
            if (found_id != 0) {
                ESP_LOGW(TAG, "Profile store full; replacing profile id %u (priority %u)",
                         (unsigned)found_id, (unsigned)lowest_priority);
            }
        } else {
            for (uint16_t id = 1; id <= WIFI_PROFILE_MAX_COUNT; ++id) {
                wifi_profile_t profile;
                if (load_profile_unlocked(id, &profile) != ESP_OK) {
                    found_id = id;
                    break;
                }
            }
            new_record = true;
        }
    }

    if (found_id == 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    wifi_profile_t updated;
    profile_from_sta_config(config, found_id, &updated);
    if (existing.id != 0) {
        updated.priority = existing.priority;
        updated.last_rssi = existing.last_rssi;
        updated.fail_count = existing.fail_count;
    }

    esp_err_t err = save_profile_unlocked(&updated);
    if (err == ESP_OK && new_record) {
        ++s_count;
        err = nvs_set_u16(s_nvs, WIFI_PROFILE_NVS_KEY_COUNT, s_count);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }

    if (err == ESP_OK && profile_id != NULL) {
        *profile_id = found_id;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_set_active(uint16_t id)
{
    if (!s_initialized || id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    wifi_profile_t profile;
    esp_err_t err = load_profile_unlocked(id, &profile);
    if (err == ESP_OK) {
        err = nvs_set_u16(s_nvs, WIFI_PROFILE_NVS_KEY_ACTIVE, id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err == ESP_OK) {
        s_active_id = id;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_delete(uint16_t id)
{
    if (!s_initialized || id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    wifi_profile_t profile;
    esp_err_t err = load_profile_unlocked(id, &profile);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    err = erase_profile_unlocked(id);
    if (err == ESP_OK) {
        if (s_count > 0) {
            --s_count;
        }
        err = nvs_set_u16(s_nvs, WIFI_PROFILE_NVS_KEY_COUNT, s_count);
    }
    if (err == ESP_OK && s_active_id == id) {
        s_active_id = find_best_profile_unlocked();
        err = nvs_set_u16(s_nvs, WIFI_PROFILE_NVS_KEY_ACTIVE, s_active_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_set_priority(uint16_t id, uint8_t priority)
{
    if (!s_initialized || id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    wifi_profile_t profile;
    esp_err_t err = load_profile_unlocked(id, &profile);
    if (err == ESP_OK) {
        profile.priority = priority;
        err = save_profile_unlocked(&profile);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_update_connection(uint16_t id, uint8_t channel, uint8_t authmode)
{
    if (!s_initialized || id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    wifi_profile_t profile;
    esp_err_t err = load_profile_unlocked(id, &profile);
    if (err == ESP_OK && (profile.channel != channel || profile.authmode != authmode)) {
        profile.channel = channel;
        profile.authmode = authmode;
        err = save_profile_unlocked(&profile);
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs);
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_profile_store_to_sta_config(const wifi_profile_t *profile, wifi_sta_config_t *config)
{
    if (profile == NULL || config == NULL ||
            profile->ssid_len == 0 || profile->ssid_len > WIFI_PROFILE_SSID_MAX_LEN ||
            profile->password_len > WIFI_PROFILE_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    memcpy(config->ssid, profile->ssid, profile->ssid_len);
    memcpy(config->password, profile->password, profile->password_len);
    config->scan_method = WIFI_FAST_SCAN;
    config->sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config->threshold.authmode = WIFI_AUTH_OPEN;
    config->channel = profile->channel;
    config->failure_retry_cnt = 1;
    config->pmf_cfg.capable = true;
    config->sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    if (profile->bssid_valid) {
        config->bssid_set = true;
        memcpy(config->bssid, profile->bssid, sizeof(config->bssid));
    }
    return ESP_OK;
}
