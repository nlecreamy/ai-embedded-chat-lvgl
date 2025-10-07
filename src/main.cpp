#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "initialise_wifi_time.h" // Your custom header
#include "driver/uart.h"

static const char *TAG = "main_app";

// --- Global define for your API Key ---
#define GEMINI_API_KEY "KEY" // IMPORTANT: Replace with your actual key

// --- UART Configuration ---
#define UART_NUM UART_NUM_0
#define BUF_SIZE 1024

// --- Global flag for WiFi initialization ---
bool wifi_initialized = false;

// --- HTTP Event Handler ---
// This handler helps in debugging by printing response data as it arrives.
esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADERS_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADERS_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            // Print the response chunk
            printf("%.*s", evt->data_len, (char*)evt->data);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            printf("\n");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

// --- Task to send a message to the Gemini API ---
// This runs in a dedicated task with its own stack.
static void gemini_task(void *pvParameters) {
    char *message = (char *)pvParameters;

    ESP_LOGI(TAG, "GEMINI TASK: Starting task. Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // 1. Construct the JSON payload
    cJSON *root = cJSON_CreateObject();
    cJSON *contents = cJSON_CreateArray();
    cJSON *content = cJSON_CreateObject();
    cJSON *parts = cJSON_CreateArray();
    cJSON *text = cJSON_CreateObject();

    cJSON_AddStringToObject(text, "text", message);
    cJSON_AddItemToArray(parts, text);
    cJSON_AddItemToObject(content, "parts", parts);
    cJSON_AddItemToArray(contents, content);
    cJSON_AddItemToObject(root, "contents", contents);

    cJSON *generationConfig = cJSON_CreateObject();
    cJSON *thinkingConfig = cJSON_CreateObject();
    cJSON_AddBoolToObject(thinkingConfig, "includeThoughts", cJSON_True);
    cJSON_AddItemToObject(generationConfig, "thinkingConfig", thinkingConfig);
    cJSON_AddItemToObject(root, "generationConfig", generationConfig);

    char *post_data = cJSON_Print(root);
    ESP_LOGI(TAG, "Request Body: %s", post_data);

    // 2. Configure the HTTP client
    char url[256];
    // Gemini 2.5 Pro is available to all API key users
    snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-pro:generateContent?key=%s", GEMINI_API_KEY);

    esp_http_client_config_t config = {
        .url = url,
        .user_agent = "esp32-http-client/1.0",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 120 * 1000, // **Increased timeout**
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // 3. Set headers and send the request
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTPS POST request sent successfully, status = %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "HTTPS POST request failed: %s", esp_err_to_name(err));
    }

    // 4. Cleanup
    esp_http_client_cleanup(client);
    cJSON_Delete(root);
    free(post_data);
    free(message);

    printf("\nPlease enter your prompt:\n>");
    fflush(stdout);
    vTaskDelete(NULL); // Delete the task once it's done
}

// --- Configure and install UART driver ---
static void init_uart() {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // Install UART driver, and get the queue.
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);
}

// --- Your existing callback functions ---
static void on_init_success() {
    ESP_LOGI(TAG, "CALLBACK: Wi-Fi and Time initialization successful.");
    wifi_initialized = true;
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Current time is: %s", strftime_buf);
    if (wifi_initialized) {
        printf("\nPlease enter your prompt:\n>");
        fflush(stdout);
    }
}

static void on_init_failure() {
    ESP_LOGE(TAG, "CALLBACK: Wi-Fi and Time initialization failed. Taking alternative action.");
}

// --- Your app_main ---
extern "C" void app_main(void) {
    init_uart(); // Initialize UART for serial input
    initialise_wifi_and_time_async(on_init_success, on_init_failure);
    ESP_LOGI(TAG, "Initialization has started in the background.");

    // Buffer for incoming data from UART
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);
    int pos = 0; // Position in the buffer

    while (1) {
        // Read data from the UART
        int len = uart_read_bytes(UART_NUM, data + pos, 1, pdMS_TO_TICKS(20));
        
        // If data is received
        if (len > 0) {
            // Echo the character back to the serial monitor
            uart_write_bytes(UART_NUM, (const char *) &data[pos], 1);

            // Check if the received character is a newline (end of prompt)
            if (data[pos] == '\n' || data[pos] == '\r') {
                printf("\n"); // Move to a new line in the monitor
                data[pos] = '\0'; // Null-terminate the string

                if (pos > 0 && wifi_initialized) {
                    // Create a copy of the prompt to pass to the task
                    char* prompt_to_send = strdup((char*)data);
                    if (prompt_to_send) {
                        xTaskCreate(&gemini_task, "gemini_task", 8192, (void *)prompt_to_send, 5, NULL);
                    } else {
                        ESP_LOGE(TAG, "Failed to allocate memory for prompt.");
                    }
                } else if (!wifi_initialized) {
                    ESP_LOGW(TAG, "Wi-Fi not ready. Please wait.");
                }
                
                // Reset buffer position for the next prompt
                pos = 0; 
                memset(data, 0, BUF_SIZE);

            } else {
                pos++;
                if (pos >= BUF_SIZE) {
                    ESP_LOGE(TAG, "Prompt too long. Resetting buffer.");
                    pos = 0;
                    memset(data, 0, BUF_SIZE);
                }
            }
        }
    }
    free(data); // Unreachable, but included for completeness
}