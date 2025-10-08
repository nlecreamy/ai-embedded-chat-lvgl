#include <string>
#include <cstdio>
#include <cstring>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "driver/uart.h"
#include "credentials.h"

static const char *TAG = "GEMINI";

static SemaphoreHandle_t wifi_connected = NULL;

struct HttpData {
    std::string thoughts;
    std::string answer;
    std::string response_buffer;
};

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected, got IP");
        if (wifi_connected != NULL) {
            xSemaphoreGive(wifi_connected);
        }
    }
}

static void process_data_line(const std::string& line, HttpData* data) {
    if (line.rfind("data: ", 0) != 0) {
        return;  // Skip non-data lines
    }
    std::string json_str = line.substr(6);  // Skip "data: "
    // Trim leading whitespace if any
    size_t start = json_str.find_first_not_of(" \t");
    if (start != std::string::npos) {
        json_str = json_str.substr(start);
    }
    // Trim trailing whitespace if any
    size_t end = json_str.find_last_not_of(" \t");
    if (end != std::string::npos) {
        json_str = json_str.substr(0, end + 1);
    }
    if (json_str == "[DONE]") {
        return;
    }
    cJSON *json = cJSON_Parse(json_str.c_str());
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON: %s", json_str.c_str());
        return;
    }
    cJSON *candidates = cJSON_GetObjectItem(json, "candidates");
    if (candidates && cJSON_IsArray(candidates)) {
        cJSON *candidate = cJSON_GetArrayItem(candidates, 0);
        if (candidate) {
            cJSON *content = cJSON_GetObjectItem(candidate, "content");
            if (content) {
                cJSON *parts = cJSON_GetObjectItem(content, "parts");
                if (parts && cJSON_IsArray(parts)) {
                    cJSON *part = cJSON_GetArrayItem(parts, 0);
                    if (part) {
                        cJSON *thought_flag = cJSON_GetObjectItem(part, "thought");
                        cJSON *text = cJSON_GetObjectItem(part, "text");
                        if (text && cJSON_IsString(text)) {
                            if (thought_flag && cJSON_IsBool(thought_flag) && cJSON_IsTrue(thought_flag)) {
                                if (data->thoughts.empty()) {
                                    printf("Thoughts :\n");
                                }
                                printf("%s\n", text->valuestring);
                                data->thoughts += text->valuestring;
                                data->thoughts += "\n";
                            } else {
                                if (data->answer.empty()) {
                                    printf("Answer:\n");
                                }
                                printf("%s\n", text->valuestring);
                                data->answer += text->valuestring;
                                data->answer += "\n";
                            }
                        }
                    }
                }
            }
        }
    }
    cJSON_Delete(json);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    HttpData* data = (HttpData*)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                return ESP_OK;
            }
            data->response_buffer.append((char*)evt->data, evt->data_len);
            size_t pos;
            while ((pos = data->response_buffer.find('\n')) != std::string::npos) {
                std::string line = data->response_buffer.substr(0, pos);
                data->response_buffer.erase(0, pos + 1);
                process_data_line(line, data);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void process_full_buffer(HttpData* data) {
    size_t pos;
    // Process any complete lines left in the buffer
    while ((pos = data->response_buffer.find('\n')) != std::string::npos) {
        std::string line = data->response_buffer.substr(0, pos);
        data->response_buffer.erase(0, pos + 1);
        process_data_line(line, data);
    }
    // Handle any trailing incomplete line (e.g., last chunk without \n)
    if (!data->response_buffer.empty()) {
        std::string line = data->response_buffer;
        data->response_buffer.clear();
        process_data_line(line, data);
    }
    ESP_LOGI(TAG, "Stream processing complete. Final thoughts: %zu chars, answer: %zu chars", data->thoughts.length(), data->answer.length());
}

static void read_line(char* buffer, size_t max_len) {
    int idx = 0;
    while (idx < (int)max_len - 1) {
        uint8_t c;
        int ret = uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY);
        if (ret == 1) {
            if (c == '\n' || c == '\r') {
                break;
            }
            buffer[idx++] = (char)c;
        }
    }
    buffer[idx] = '\0';
}

static void process_prompt(const char* prompt) {
    HttpData data;
    data.response_buffer.reserve(1024);

    cJSON *root = cJSON_CreateObject();
    cJSON *contents = cJSON_AddArrayToObject(root, "contents");
    cJSON *content = cJSON_CreateObject();
    cJSON_AddItemToArray(contents, content);
    cJSON *parts = cJSON_AddArrayToObject(content, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", prompt);
    cJSON_AddItemToArray(parts, part);
    cJSON *generationConfig = cJSON_AddObjectToObject(root, "generationConfig");
    cJSON *thinkingConfig = cJSON_AddObjectToObject(generationConfig, "thinkingConfig");
    cJSON_AddBoolToObject(thinkingConfig, "includeThoughts", 1);
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:streamGenerateContent?alt=sse&key=%s", API_KEY);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.user_data = &data;
    config.disable_auto_redirect = false;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    process_full_buffer(&data);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Status = %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(post_data);
}

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_connected = xSemaphoreCreateBinary();

    // Wait for WiFi connection
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    if (xSemaphoreTake(wifi_connected, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to connect to WiFi within 60 seconds");
        return;
    }
    ESP_LOGI(TAG, "WiFi connected");

    // Initialize UART for serial input
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
    ESP_LOGI(TAG, "UART initialized. Enter prompts (one per line):");

    // Main loop: read prompts from serial and process
    char prompt[256];
    while (true) {
        read_line(prompt, sizeof(prompt));
        if (strlen(prompt) > 0) {
            ESP_LOGI(TAG, "Received prompt: %s", prompt);
            process_prompt(prompt);
            printf("\n");  // Add a newline after response for readability
        }
    }
}