/*
 * SPDX-FileCopyrightText: 2026 WT32-ETH01 bridge contributors
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#ifndef CONFIG_EXAMPLE_WIFI_PROFILE_MAX_COUNT
#define CONFIG_EXAMPLE_WIFI_PROFILE_MAX_COUNT 16
#endif

#define WIFI_PROFILE_MAX_COUNT CONFIG_EXAMPLE_WIFI_PROFILE_MAX_COUNT
#define WIFI_PROFILE_SSID_MAX_LEN 32
#define WIFI_PROFILE_PASSWORD_MAX_LEN 64
#define WIFI_PROFILE_STRUCT_VERSION 1

typedef struct {
    uint16_t struct_size;
    uint8_t version;
    uint8_t enabled;
    uint16_t id;
    uint8_t ssid[WIFI_PROFILE_SSID_MAX_LEN];
    uint8_t password[WIFI_PROFILE_PASSWORD_MAX_LEN];
    uint8_t ssid_len;
    uint8_t password_len;
    uint8_t authmode;
    uint8_t channel;
    uint8_t bssid[6];
    uint8_t priority;
    int8_t last_rssi;
    uint8_t fail_count;
    uint8_t bssid_valid;
    uint8_t reserved[8];
} wifi_profile_t;

esp_err_t wifi_profile_store_init(void);

size_t wifi_profile_store_count(void);
bool wifi_profile_store_has_active(void);

esp_err_t wifi_profile_store_get(uint16_t id, wifi_profile_t *profile);
esp_err_t wifi_profile_store_get_active(wifi_profile_t *profile);
esp_err_t wifi_profile_store_list(wifi_profile_t *profiles, size_t capacity, size_t *count);

esp_err_t wifi_profile_store_upsert(const wifi_sta_config_t *config, uint16_t *profile_id);
esp_err_t wifi_profile_store_set_active(uint16_t id);
esp_err_t wifi_profile_store_delete(uint16_t id);
esp_err_t wifi_profile_store_set_priority(uint16_t id, uint8_t priority);
esp_err_t wifi_profile_store_update_connection(uint16_t id, uint8_t channel, uint8_t authmode);

esp_err_t wifi_profile_store_to_sta_config(const wifi_profile_t *profile, wifi_sta_config_t *config);
