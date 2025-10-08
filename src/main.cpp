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
#include "credentials.h"

static const char *TAG = "GEMINI";

static std::string thoughts = "";
static std::string answer = "";
static std::string response_buffer = "";
static SemaphoreHandle_t wifi_connected = NULL;

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

static void process_data_line(const std::string& line) {
    if (line.find("data: ") != 0) {
        return;  // Skip non-data lines
    }
    std::string json_str = line.substr(6);  // Skip "data: "
    // Trim leading whitespace if any
    size_t start = json_str.find_first_not_of(" \t");
    if (start != std::string::npos) {
        json_str = json_str.substr(start);
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
                        cJSON *thought = cJSON_GetObjectItem(part, "thought");
                        if (thought && cJSON_IsString(thought)) {
                            if (thoughts.empty()) {
                                printf("Thoughts summary:\n");
                            }
                            printf("%s", thought->valuestring);
                            thoughts += thought->valuestring;
                        } else {
                            cJSON *text = cJSON_GetObjectItem(part, "text");
                            if (text && cJSON_IsString(text)) {
                                if (answer.empty()) {
                                    printf("Answer:\n");
                                }
                                printf("%s", text->valuestring);
                                answer += text->valuestring;
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
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            break;
        case HTTP_EVENT_ON_CONNECTED:
            break;
        case HTTP_EVENT_HEADERS_SENT:
            break;
        case HTTP_EVENT_ON_HEADER:
            break;
        case HTTP_EVENT_ON_DATA: {
            if (!esp_http_client_is_chunked_response(evt->client)) {
                return ESP_OK;
            }
            response_buffer.append(static_cast<const char*>(evt->data), evt->data_len);
            const size_t buf_len = response_buffer.size();
            size_t start = 0;
            while (start < buf_len) {
                size_t pos = response_buffer.find('\n', start);
                if (pos == std::string::npos) {
                    break;
                }
                std::string line(response_buffer.data() + start, pos - start);
                process_data_line(line);
                start = pos + 1;
            }
            response_buffer.erase(0, start);
            break;
        }
        case HTTP_EVENT_ON_FINISH:
            break;
        case HTTP_EVENT_DISCONNECTED:
            break;
        case HTTP_EVENT_REDIRECT:
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void process_full_buffer() {
    const size_t buf_len = response_buffer.size();
    size_t start = 0;
    while (start < buf_len) {
        size_t pos = response_buffer.find('\n', start);
        if (pos == std::string::npos) {
            pos = buf_len;
        }
        std::string line(response_buffer.data() + start, pos - start);
        process_data_line(line);
        start = pos + 1;
    }
    response_buffer.clear();
    ESP_LOGI(TAG, "Stream processing complete. Final thoughts: %zu chars, answer: %zu chars", thoughts.length(), answer.length());
}

// Add this function before app_main
static void http_task(void *pvParameters) {
    // Wait for WiFi connection
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    if (xSemaphoreTake(wifi_connected, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to connect to WiFi within 30 seconds");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "WiFi connected, proceeding with HTTP request");

    const char *prompt = R"(Say a very big comprehensive story!)";

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
    config.disable_auto_redirect = false;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    process_full_buffer();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Status = %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(post_data);

    // Keep this task running
    while (true) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
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

    // After WiFi start, create dedicated task instead of doing work in main
    xTaskCreate(http_task, "http_task", 8192, NULL, 5, NULL);

    // Main task just idles
    while (true) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}