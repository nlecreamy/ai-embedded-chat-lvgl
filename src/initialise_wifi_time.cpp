#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "initialise_wifi_time.h"

// ---------- Configuration ----------
#define WIFI_SSID       "realme NARZO 70 Turbo 5G"
#define WIFI_PASS       "Trebledroids"
#define MAX_RETRY       5
#define SNTP_TIMEOUT_MS 15000

// --- Static variables and definitions ---
static const char *TAG = "wifi_time";
static int retry_count = 0;

// Event group to signal Wi-Fi and time sync events
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define TIME_SYNCED_BIT    BIT2

// --- Pointers to store the user-provided callbacks ---
static init_callback_t s_on_success_cb = NULL;
static init_callback_t s_on_failure_cb = NULL;

// --- Forward Declarations ---
static void initialize_sntp(void);
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void wifi_init_sta(void);
static void wifi_time_manager_task(void *pvParameters); // The new task

// ----------------------------
// SNTP Time Sync Section (No changes here)
// ----------------------------

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronization event received");
    xEventGroupSetBits(wifi_event_group, TIME_SYNCED_BIT);
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "(Re)Initializing SNTP");
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.cloudflare.com");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

// ----------------------------
// Wi-Fi Event Handler Section (No changes here)
// ----------------------------

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START - connecting...");
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED");
            if (retry_count < MAX_RETRY) {
                ESP_LOGI(TAG, "Retrying Wi-Fi connection (%d/%d)", retry_count + 1, MAX_RETRY);
                esp_wifi_connect();
                retry_count++;
            } else {
                ESP_LOGE(TAG, "Reached max retries. Giving up.");
                xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            }
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        initialize_sntp();
    }
}

// ----------------------------
// Wi-Fi Initialization (No changes here)
// ----------------------------
static void wifi_init_sta(void)
{
    // Note: The event group is created here, but waited on in the manager task
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi init process started (async) - SSID:%s", WIFI_SSID);
}

// ----------------------------------------------------
// NEW: The task that waits for events and calls back
// ----------------------------------------------------

static void wifi_time_manager_task(void *pvParameters)
{
    // Wait for Wi-Fi connection or failure
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_SSID);

        // Now wait for time synchronization
        ESP_LOGI(TAG, "Waiting for SNTP time sync (timeout: %dms)...", SNTP_TIMEOUT_MS);
        EventBits_t timeBits = xEventGroupWaitBits(wifi_event_group,
                                                   TIME_SYNCED_BIT,
                                                   pdFALSE,
                                                   pdFALSE,
                                                   pdMS_TO_TICKS(SNTP_TIMEOUT_MS));

        if (timeBits & TIME_SYNCED_BIT) {
            ESP_LOGI(TAG, "Time successfully synchronized");
            setenv("TZ", "IST-5:30", 1);
            tzset();
            if (s_on_success_cb != NULL) {
                s_on_success_cb(); // <-- Invoke the success callback
            }
        } else {
            ESP_LOGE(TAG, "Time sync timeout!");
            if (s_on_failure_cb != NULL) {
                s_on_failure_cb(); // <-- Invoke the failure callback
            }
        }
    } else { // This covers WIFI_FAIL_BIT and unexpected events
        ESP_LOGE(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
        if (s_on_failure_cb != NULL) {
            s_on_failure_cb(); // <-- Invoke the failure callback
        }
    }
    
    // The task has completed its purpose, clean up and delete it.
    vEventGroupDelete(wifi_event_group);
    vTaskDelete(NULL);
}

// ------------------------------------
// Public Interface Function (Modified)
// ------------------------------------

void initialise_wifi_and_time_async(init_callback_t on_success_cb, init_callback_t on_failure_cb)
{
    // Store the callbacks
    s_on_success_cb = on_success_cb;
    s_on_failure_cb = on_failure_cb;

    // Initialize NVS (required for Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Start the Wi-Fi initialization process (this is non-blocking)
    wifi_init_sta();

    // Create the manager task that will wait for results and issue the callback
    xTaskCreate(wifi_time_manager_task, "wifi_time_mgr", 4096, NULL, 5, NULL);
}