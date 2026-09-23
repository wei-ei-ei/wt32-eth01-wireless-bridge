/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include <esp_timer.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_private/wifi.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "app_control.h"
#include "provisioning.h"
#include "wired_iface.h"
#include "wifi_profile_store.h"

static const char *TAG = "wt32_bridge";


static EventGroupHandle_t s_event_flags;
static bool s_wifi_is_connected = false;
static uint8_t s_sta_mac[6];
static uint16_t s_active_profile_id;
static __NOINIT_ATTR uint32_t s_reconfigure_requested;

static const uint32_t RECONFIGURE_REQUEST = 0x1C55AA;

const int CONNECTED_BIT = BIT0;
const int DISCONNECTED_BIT = BIT1;
const int RECONFIGURE_BIT = BIT2;
const int PROV_SUCCESS_BIT = BIT3;
const int PROV_FAIL_BIT = BIT4;

/**
 * WiFi -- Wired packet path
 */
static esp_err_t wired_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    if (s_wifi_is_connected) {
        if (!mac_spoof(FROM_WIRED, buffer, len, s_sta_mac)) {
            return ESP_OK;
        }
        if (esp_wifi_internal_tx(WIFI_IF_STA, buffer, len) != ESP_OK) {
            /* 上行拥塞时 WiFi 发送队列丢帧是静默的：TCP 只能靠 RTO 重传恢复，
             * RTMP 延迟会持续抬高。计数并周期性告警，用于量化推流期丢包率。 */
            static uint32_t s_wifi_tx_drop_cnt;
            s_wifi_tx_drop_cnt++;
            if (s_wifi_tx_drop_cnt == 1 || (s_wifi_tx_drop_cnt % 100) == 0) {
                ESP_LOGW(TAG, "WiFi TX drop #%lu (len=%u)", (unsigned long)s_wifi_tx_drop_cnt, len);
            }
        }
    }
    return ESP_OK;
}

static void wifi_buff_free(void *buffer, void *ctx)
{
    esp_wifi_internal_free_rx_buffer(buffer);
}

static esp_err_t wifi_recv_callback(void *buffer, uint16_t len, void *eb)
{
    if (!mac_spoof(TO_WIRED, buffer, len, s_sta_mac)) {
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }
    if (wired_send(buffer, len, eb) != ESP_OK) {
        esp_wifi_internal_free_rx_buffer(eb);
        ESP_LOGD(TAG, "Failed to send packet to USB!");
    }
    return ESP_OK;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi STA disconnected");
        s_wifi_is_connected = false;
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
        esp_wifi_connect();

        xEventGroupClearBits(s_event_flags, CONNECTED_BIT);
        xEventGroupSetBits(s_event_flags, DISCONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi STA connected");
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_recv_callback);
        s_wifi_is_connected = true;
        xEventGroupClearBits(s_event_flags, DISCONNECTED_BIT);
        xEventGroupSetBits(s_event_flags, CONNECTED_BIT);
        if (s_active_profile_id != 0) {
            wifi_profile_store_update_connection(s_active_profile_id,
                                                 event->channel,
                                                 event->authmode);
        }
    }
}

static esp_err_t connect_wifi(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );

    wifi_profile_t profile;
    if (wifi_profile_store_get_active(&profile) == ESP_OK) {
        wifi_config_t wifi_cfg = {0};
        ESP_ERROR_CHECK(wifi_profile_store_to_sta_config(&profile, &wifi_cfg.sta));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        s_active_profile_id = profile.id;
        ESP_LOGI(TAG, "Using stored Wi-Fi profile id %u for SSID: %.*s",
                 (unsigned)profile.id, (int)profile.ssid_len, (const char *)profile.ssid);
    } else {
        wifi_config_t wifi_cfg;
        if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
            return ESP_FAIL;
        }
        s_active_profile_id = 0;
    }

    ESP_ERROR_CHECK(esp_wifi_start() );

    /* 关闭 WiFi 省电模式（MODEM sleep）：
     * 默认省电模式下 WiFi 周期性休眠（约 102ms），下行大流量（iperf3/SSH 的
     * TCP 数据+ACK）在休眠窗口被丢包，TCP 重传崩溃、表现"不通"。
     * 关掉后 WiFi 始终接收，两跳 WiFi 场景下 TCP 才能维持。 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_wifi_connect();
    EventBits_t status = xEventGroupWaitBits(s_event_flags, CONNECTED_BIT, 0, 1, 10000 / portTICK_PERIOD_MS);
    if (status & CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi station connected successfully");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WiFi station connected failed");
    return ESP_ERR_TIMEOUT;
}

static void migrate_existing_wifi_profile(void)
{
    if (wifi_profile_store_count() != 0) {
        return;
    }

    wifi_config_t wifi_cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK ||
            wifi_cfg.sta.ssid[0] == '\0') {
        return;
    }

    uint16_t profile_id = 0;
    esp_err_t err = wifi_profile_store_upsert(&wifi_cfg.sta, &profile_id);
    if (err == ESP_OK) {
        err = wifi_profile_store_set_active(profile_id);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Migrated existing Wi-Fi credentials to profile id %u",
                 (unsigned)profile_id);
    } else {
        ESP_LOGW(TAG, "Failed to migrate existing Wi-Fi credentials: %s",
                 esp_err_to_name(err));
    }
}

/**
 * GPIO button functionality
 */
#define GPIO_INPUT          CONFIG_EXAMPLE_RECONFIGURE_BUTTON
#define GPIO_LONG_PUSH_US   2000000  /* push for 2 seconds to reconfigure */

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    static int64_t last_pushed = -1;
    if (gpio_get_level(GPIO_INPUT) == 0) {
        last_pushed = esp_timer_get_time();
    } else {
        uint64_t now = esp_timer_get_time();
        if (last_pushed != -1 && now - last_pushed > GPIO_LONG_PUSH_US) {
            BaseType_t high_task_wakeup;
            xEventGroupSetBitsFromISR(s_event_flags, RECONFIGURE_BIT, &high_task_wakeup);
            if (high_task_wakeup) {
                portYIELD_FROM_ISR();
            }
        }
        last_pushed = -1;
    }
}

static void gpio_init(void)
{
    gpio_config_t io_conf = { .intr_type = GPIO_INTR_ANYEDGE,
                              .pin_bit_mask = (1ULL << GPIO_INPUT),
                              .mode = GPIO_MODE_INPUT,
                              .pull_up_en = GPIO_PULLUP_ENABLE
                            };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT, gpio_isr_handler, NULL);
}

void app_restart_to_bridge(void)
{
    s_reconfigure_requested = 0;
    esp_restart();
}

/**
 * Application
 */
void app_main(void)
{
    /* Check reset reason and decide if we should re-provision */
    bool do_provision = false;
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGD(TAG, "After restart! %d", reason);
    if (reason != ESP_RST_SW) {
        s_reconfigure_requested = 0;
    } else if (s_reconfigure_requested == RECONFIGURE_REQUEST) {
        do_provision = true;
    }

    /* Initialize NVS and WiFi */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(wifi_profile_store_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    migrate_existing_wifi_profile();

    // init the flags and event loop
    s_event_flags = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Init the re-provisioning button (long-press with initiate provisioning restart) */
    gpio_init();
    esp_read_mac(s_sta_mac, ESP_MAC_WIFI_STA);

    /* Start the application in configuration mode (to perform provisioning)
     * or in a bridge mode (already provisioned) */
    if (do_provision || !is_provisioned()) {
        ESP_LOGI(TAG, "Starting provisioning");
        ESP_ERROR_CHECK(esp_netif_init());
        // needed to complete provisioning with getting a valid IP event
        esp_netif_create_default_wifi_sta();

        // starts the wired interface with virtual network used to configure/provision the example
        wired_netif_init();
        start_provisioning(&s_event_flags, PROV_SUCCESS_BIT, PROV_FAIL_BIT);
    } else {
        ESP_LOGI(TAG, "Starting USB-WiFi bridge");
        if (connect_wifi() != ESP_OK) {
            // if we cannot connect to WiFi we just try to re-provision
            xEventGroupSetBits(s_event_flags, RECONFIGURE_BIT);
        } else {
            // start the wired interface in the bridge mode
            wired_bridge_init(wired_recv_callback, wifi_buff_free);
        }
    }

    EventBits_t bits = xEventGroupWaitBits(s_event_flags, RECONFIGURE_BIT | PROV_SUCCESS_BIT | PROV_FAIL_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
    if (bits & RECONFIGURE_BIT || bits & PROV_FAIL_BIT) {
        // retry provisioning if it previously failed or if requested by the button press
        s_reconfigure_requested = RECONFIGURE_REQUEST;
    } else {
        // provisioning successfully finished, restart to the bridge mode
        s_reconfigure_requested = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));    // to let httpd handle the closure
    esp_restart();
}
