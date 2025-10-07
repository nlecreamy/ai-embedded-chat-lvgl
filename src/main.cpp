#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "initialise_wifi_time.h" // Include the new header

static const char *TAG = "main_app";

// --- Define the functions that will be used as callbacks ---

/**
 * @brief This function is called when Wi-Fi and Time are successfully initialized.
 */
static void on_init_success()
{
    ESP_LOGI(TAG, "CALLBACK: Wi-Fi and Time initialization successful.");

    // --- You can now get the time ---
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Current time is: %s", strftime_buf);

    // --- Start other tasks that require the network/time here ---
    ESP_LOGI(TAG, "Proceeding with network-dependent application logic...");
}

/**
 * @brief This function is called if Wi-Fi or Time initialization fails.
 */
static void on_init_failure()
{
    ESP_LOGE(TAG, "CALLBACK: Wi-Fi and Time initialization failed. Taking alternative action.");
    // Take action: e.g., restart, enter a fault state, or run offline logic.
}


extern "C" void app_main(void)
{
    // Start the asynchronous initialization process by providing the callbacks.
    // This call returns immediately.
    initialise_wifi_and_time_async(on_init_success, on_init_failure);

    ESP_LOGI(TAG, "Initialization has started in the background.");
    ESP_LOGI(TAG, "app_main can now perform other non-network-related tasks.");

    // Example of doing other work while waiting
    int counter = 0;
    while (1) {
        ESP_LOGI(TAG, "app_main is alive and running... (%d)", counter++);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
