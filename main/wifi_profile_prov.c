/*
 * SPDX-FileCopyrightText: 2026 WT32-ETH01 bridge contributors
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "wifi_profile_prov.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_control.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_provisioning/manager.h"
#include "wifi_profile_store.h"

static const char *TAG = "wifi_profiles_prov";

#define WIFI_PROFILE_ENDPOINT "wifi-profiles"
#define WIFI_PROFILE_COMMAND_MAX 96
#define WIFI_PROFILE_RESPONSE_CAP 4096

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool ok;
} response_writer_t;

static bool s_restart_scheduled;

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    app_restart_to_bridge();
}

static void schedule_restart(void)
{
    if (s_restart_scheduled) {
        return;
    }
    s_restart_scheduled = true;
    if (xTaskCreate(restart_task, "wifi_apply", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create restart task");
        s_restart_scheduled = false;
    }
}

static bool writer_append(response_writer_t *writer, const char *format, ...)
{
    if (!writer->ok || writer->length >= writer->capacity) {
        writer->ok = false;
        return false;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(writer->buffer + writer->length,
                            writer->capacity - writer->length,
                            format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= writer->capacity - writer->length) {
        writer->ok = false;
        return false;
    }
    writer->length += (size_t)written;
    return true;
}

static bool writer_append_json_string(response_writer_t *writer, const uint8_t *value, size_t length)
{
    if (!writer_append(writer, "\"")) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = value[i];
        switch (ch) {
        case '"':
            if (!writer_append(writer, "\\\"")) {
                return false;
            }
            break;
        case '\\':
            if (!writer_append(writer, "\\\\")) {
                return false;
            }
            break;
        case '\b':
            if (!writer_append(writer, "\\b")) {
                return false;
            }
            break;
        case '\f':
            if (!writer_append(writer, "\\f")) {
                return false;
            }
            break;
        case '\n':
            if (!writer_append(writer, "\\n")) {
                return false;
            }
            break;
        case '\r':
            if (!writer_append(writer, "\\r")) {
                return false;
            }
            break;
        case '\t':
            if (!writer_append(writer, "\\t")) {
                return false;
            }
            break;
        default:
            if (ch < 0x20) {
                if (!writer_append(writer, "\\u%04x", ch)) {
                    return false;
                }
            } else {
                if (!writer_append(writer, "%c", ch)) {
                    return false;
                }
            }
            break;
        }
    }
    return writer_append(writer, "\"");
}

static esp_err_t finish_response(response_writer_t *writer, uint8_t **outbuf, ssize_t *outlen)
{
    if (!writer->ok) {
        free(writer->buffer);
        writer->buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    *outbuf = (uint8_t *)writer->buffer;
    *outlen = (ssize_t)writer->length;
    writer->buffer = NULL;
    return ESP_OK;
}

static esp_err_t write_error(response_writer_t *writer, const char *error,
                             uint8_t **outbuf, ssize_t *outlen)
{
    writer_append(writer, "{\"ok\":false,\"error\":");
    writer_append_json_string(writer, (const uint8_t *)error, strlen(error));
    writer_append(writer, "}");
    return finish_response(writer, outbuf, outlen);
}

static void trim_command(char *command)
{
    char *start = command;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }
    if (start != command) {
        memmove(command, start, strlen(start) + 1);
    }

    size_t length = strlen(command);
    while (length > 0 && isspace((unsigned char)command[length - 1])) {
        command[--length] = '\0';
    }
}

static bool parse_u16(const char *text, uint16_t *value)
{
    if (text == NULL || *text == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT16_MAX) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

static bool parse_u8(const char *text, uint8_t *value)
{
    uint16_t parsed;
    if (!parse_u16(text, &parsed) || parsed > UINT8_MAX) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

static bool append_profile(response_writer_t *writer, const wifi_profile_t *profile)
{
    return writer_append(writer,
                         "{\"id\":%u,\"ssid\":",
                         (unsigned)profile->id) &&
           writer_append_json_string(writer, profile->ssid, profile->ssid_len) &&
           writer_append(writer,
                         ",\"priority\":%u,\"enabled\":%s,\"authmode\":%u,"
                         "\"channel\":%u,\"last_rssi\":%d,\"fail_count\":%u}",
                         (unsigned)profile->priority,
                         profile->enabled ? "true" : "false",
                         (unsigned)profile->authmode,
                         (unsigned)profile->channel,
                         (int)profile->last_rssi,
                         (unsigned)profile->fail_count);
}

static esp_err_t handle_list(response_writer_t *writer, uint8_t **outbuf, ssize_t *outlen)
{
    wifi_profile_t *profiles = calloc(WIFI_PROFILE_MAX_COUNT, sizeof(*profiles));
    if (profiles == NULL) {
        return write_error(writer, "out of memory", outbuf, outlen);
    }

    size_t count = 0;
    esp_err_t err = wifi_profile_store_list(profiles, WIFI_PROFILE_MAX_COUNT, &count);
    if (err != ESP_OK) {
        free(profiles);
        return write_error(writer, "failed to read profiles", outbuf, outlen);
    }

    wifi_profile_t active_profile;
    uint16_t active_id = 0;
    if (wifi_profile_store_get_active(&active_profile) == ESP_OK) {
        active_id = active_profile.id;
    }
    writer_append(writer, "{\"ok\":true,\"count\":%u,\"active_id\":%u,\"profiles\":[",
                  (unsigned)count, (unsigned)active_id);
    for (size_t i = 0; i < count && i < WIFI_PROFILE_MAX_COUNT; ++i) {
        if (i != 0) {
            writer_append(writer, ",");
        }
        append_profile(writer, &profiles[i]);
    }
    writer_append(writer, "]}");
    free(profiles);
    return finish_response(writer, outbuf, outlen);
}

static esp_err_t handle_active(response_writer_t *writer, uint8_t **outbuf, ssize_t *outlen)
{
    wifi_profile_t profile;
    if (wifi_profile_store_get_active(&profile) != ESP_OK) {
        writer_append(writer, "{\"ok\":true,\"active\":0,\"profile\":null}");
        return finish_response(writer, outbuf, outlen);
    }

    writer_append(writer, "{\"ok\":true,\"active\":1,\"profile\":");
    append_profile(writer, &profile);
    writer_append(writer, "}");
    return finish_response(writer, outbuf, outlen);
}

static esp_err_t handle_select(const char *argument, response_writer_t *writer,
                               uint8_t **outbuf, ssize_t *outlen)
{
    uint16_t id;
    if (!parse_u16(argument, &id)) {
        return write_error(writer, "usage: SELECT <id>", outbuf, outlen);
    }
    esp_err_t err = wifi_profile_store_set_active(id);
    if (err != ESP_OK) {
        return write_error(writer, "profile not found", outbuf, outlen);
    }

    writer_append(writer, "{\"ok\":true,\"active_id\":%u,\"restarting\":true}", (unsigned)id);
    esp_err_t response_err = finish_response(writer, outbuf, outlen);
    if (response_err == ESP_OK) {
        schedule_restart();
    }
    return response_err;
}

static esp_err_t handle_delete(const char *argument, response_writer_t *writer,
                               uint8_t **outbuf, ssize_t *outlen)
{
    uint16_t id;
    if (!parse_u16(argument, &id)) {
        return write_error(writer, "usage: DELETE <id>", outbuf, outlen);
    }
    esp_err_t err = wifi_profile_store_delete(id);
    if (err != ESP_OK) {
        return write_error(writer, "profile not found", outbuf, outlen);
    }

    writer_append(writer, "{\"ok\":true,\"deleted\":%u,\"active\":%u}",
                  (unsigned)id,
                  (unsigned)(wifi_profile_store_has_active() ? 1 : 0));
    return finish_response(writer, outbuf, outlen);
}

static esp_err_t handle_priority(const char *argument, response_writer_t *writer,
                                 uint8_t **outbuf, ssize_t *outlen)
{
    if (argument == NULL) {
        return write_error(writer, "usage: PRIORITY <id> <0-255>", outbuf, outlen);
    }

    char *end = NULL;
    errno = 0;
    unsigned long parsed_id = strtoul(argument, &end, 10);
    if (errno != 0 || end == argument || parsed_id > UINT16_MAX) {
        return write_error(writer, "usage: PRIORITY <id> <0-255>", outbuf, outlen);
    }
    uint16_t id = (uint16_t)parsed_id;
    while (isspace((unsigned char)*end)) {
        ++end;
    }

    uint8_t priority;
    if (!parse_u8(end, &priority)) {
        return write_error(writer, "usage: PRIORITY <id> <0-255>", outbuf, outlen);
    }

    esp_err_t err = wifi_profile_store_set_priority(id, priority);
    if (err != ESP_OK) {
        return write_error(writer, "profile not found", outbuf, outlen);
    }
    writer_append(writer, "{\"ok\":true,\"id\":%u,\"priority\":%u}",
                  (unsigned)id, (unsigned)priority);
    return finish_response(writer, outbuf, outlen);
}

static esp_err_t profile_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                 uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    (void)session_id;
    (void)priv_data;

    if (outbuf == NULL || outlen == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *outbuf = NULL;
    *outlen = 0;

    response_writer_t writer = {
        .buffer = calloc(1, WIFI_PROFILE_RESPONSE_CAP),
        .capacity = WIFI_PROFILE_RESPONSE_CAP,
        .ok = true,
    };
    if (writer.buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char command[WIFI_PROFILE_COMMAND_MAX] = {0};
    if (inbuf == NULL || inlen <= 0) {
        return write_error(&writer, "empty command", outbuf, outlen);
    }
    size_t copy_len = (size_t)inlen;
    if (copy_len >= sizeof(command)) {
        copy_len = sizeof(command) - 1;
    }
    memcpy(command, inbuf, copy_len);
    trim_command(command);
    for (char *cursor = command; *cursor != '\0'; ++cursor) {
        *cursor = (char)toupper((unsigned char)*cursor);
    }

    char *argument = strchr(command, ' ');
    if (argument != NULL) {
        *argument++ = '\0';
        trim_command(argument);
    }

    if (strcmp(command, "LIST") == 0) {
        return handle_list(&writer, outbuf, outlen);
    }
    if (strcmp(command, "GET_ACTIVE") == 0) {
        return handle_active(&writer, outbuf, outlen);
    }
    if (strcmp(command, "SELECT") == 0 || strcmp(command, "APPLY") == 0) {
        return handle_select(argument, &writer, outbuf, outlen);
    }
    if (strcmp(command, "DELETE") == 0) {
        return handle_delete(argument, &writer, outbuf, outlen);
    }
    if (strcmp(command, "PRIORITY") == 0) {
        return handle_priority(argument, &writer, outbuf, outlen);
    }

    return write_error(&writer,
                       "commands: LIST, GET_ACTIVE, SELECT <id>, DELETE <id>, PRIORITY <id> <value>",
                       outbuf, outlen);
}

esp_err_t wifi_profile_prov_endpoint_create(void)
{
    esp_err_t err = network_prov_mgr_endpoint_create(WIFI_PROFILE_ENDPOINT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create provisioning endpoint: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_profile_prov_endpoint_register(void)
{
    esp_err_t err = network_prov_mgr_endpoint_register(WIFI_PROFILE_ENDPOINT, profile_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register provisioning endpoint: %s", esp_err_to_name(err));
    }
    return err;
}
