#include <LGFX.hpp>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <stdlib.h>
#include <esp_vfs_fat.h>
#include <wear_levelling.h>
#include <esp_log.h>
#include <cJSON.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <set>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "credentials.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#define TAG "merged"

LGFX lcd;
static lv_obj_t *touch_indicator;
wl_handle_t wl_handle = WL_INVALID_HANDLE;
SemaphoreHandle_t wifi_connected = NULL;

struct HttpData {
    std::string thoughts;
    std::string answer;
    std::string response_buffer;
    cJSON *grounding_metadata = nullptr;
};

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.writePixelsDMA((uint16_t *)color_p, w * h, true);
    lcd.endWrite();
    lv_display_flush_ready(disp);
}

static uint32_t get_tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
    int x = 0, y = 0;
    if (lcd.getTouch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        lv_obj_clear_flag(touch_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(touch_indicator, x - 7, y - 7);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        lv_obj_add_flag(touch_indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool load_calibration_data(uint16_t caldata[8]) {
    if (wl_handle == WL_INVALID_HANDLE) {
        return false;
    }
    FILE *f = fopen("/storage/caldata.json", "r");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json_str = (char *)malloc(len + 1);
    if (!json_str) {
        fclose(f);
        return false;
    }
    size_t read_bytes = fread(json_str, 1, len, f);
    json_str[len] = '\0';
    fclose(f);
    if (read_bytes != (size_t)len) {
        free(json_str);
        return false;
    }
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root || !cJSON_IsArray(root) || cJSON_GetArraySize(root) != 8) {
        cJSON_Delete(root);
        return false;
    }
    bool success = true;
    for (int i = 0; i < 8; ++i) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (!item || !cJSON_IsNumber(item)) {
            success = false;
            break;
        }
        caldata[i] = (uint16_t)cJSON_GetNumberValue(item);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Calibration data loaded from JSON file.");
    return success;
}

static bool save_calibration_data(const uint16_t caldata[8]) {
    if (wl_handle == WL_INVALID_HANDLE) {
        ESP_LOGW(TAG, "Cannot save calibration data: FATFS not mounted.");
        return false;
    }
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        cJSON_AddItemToArray(root, cJSON_CreateNumber(caldata[i]));
    }
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        return false;
    }
    FILE *f = fopen("/storage/caldata.json", "w");
    if (!f) {
        cJSON_free(json_str);
        cJSON_Delete(root);
        return false;
    }
    size_t json_len = strlen(json_str);
    size_t written_bytes = fwrite(json_str, 1, json_len, f);
    fclose(f);
    cJSON_free(json_str);
    cJSON_Delete(root);
    if (written_bytes != json_len) {
        ESP_LOGE(TAG, "Failed to write JSON to file.");
        return false;
    }
    ESP_LOGI(TAG, "Calibration data saved to JSON file.");
    return true;
}

static void perform_calibration(uint16_t caldata[8]) {
    ESP_LOGI(TAG, "Performing touch calibration...");
    lcd.calibrateTouch(caldata, lcd.color565(255, 0, 0), lcd.color565(0, 0, 0), 20);
}

static void ta_event_cb(lv_event_t * e);

void lvgl_task(void *pvParameters) {
    static int32_t col_dsc[] = {LV_GRID_FR(3), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(4), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    
    lv_obj_t * cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_layout(cont, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(cont, col_dsc, row_dsc);
    lv_obj_set_flag(cont, LV_OBJ_FLAG_SCROLLABLE, false);
    
    // Status bar at top center
    lv_obj_t * status_label = lv_label_create(cont);
    lv_label_set_text(status_label, "Status: Connected");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_grid_cell(status_label, LV_GRID_ALIGN_CENTER, 0, 2, LV_GRID_ALIGN_CENTER, 0, 1);
    
    lv_obj_t *dropdown = lv_dropdown_create(cont);
    lv_dropdown_set_options(dropdown, "gemini-2.5-pro\n"
                                   "gemini-flash-latest\n"
                                   "gemini-flash-lite-latest\n"
                                   "gemini-2.5-flash\n"
                                   "gemini-2.5-flash-lite");
    lv_obj_set_grid_cell(dropdown, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);  // Adjusted to row 1
    
    // Add a menu button in the narrow right column
    lv_obj_t * conf_btn = lv_btn_create(cont);
    lv_obj_t * conf_label = lv_label_create(conf_btn);
    lv_label_set_text(conf_label, LV_SYMBOL_SETTINGS);
    lv_obj_set_grid_cell(conf_btn, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);  // Moved to row 1
    
    lv_obj_t *label = lv_label_create(cont);
    lv_label_set_text(label, "This is LVGL v9.3.0!");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_set_grid_cell(label, LV_GRID_ALIGN_CENTER, 0, 2, LV_GRID_ALIGN_CENTER, 2, 1);  // Adjusted to row 2
    
    lv_obj_t *ta = lv_textarea_create(cont);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "Ask anything...");
    lv_obj_set_grid_cell(ta, LV_GRID_ALIGN_CENTER, 0, 2, LV_GRID_ALIGN_CENTER, 3, 1);  // Adjusted to row 3
    
    lv_obj_t *kb = lv_keyboard_create(cont);
    lv_obj_set_grid_cell(kb, LV_GRID_ALIGN_CENTER, 0, 2, LV_GRID_ALIGN_END, 2, 1);  // Adjusted to row 2
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    
    // Link textarea and keyboard
    lv_keyboard_set_textarea(kb, ta);

    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_FOCUSED, kb);
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_DEFOCUSED, kb);
    
    // Create the drawer (side panel)
    static lv_obj_t * drawer = NULL;
    drawer = lv_obj_create(lv_scr_act());
    lv_obj_set_size(drawer, LV_PCT(40), LV_PCT(100));
    lv_obj_set_pos(drawer, LV_PCT(100), 0);  // Initially off-screen to the right
    lv_obj_set_style_bg_color(drawer, lv_color_hex(0x333333), 0);  // Dark background
    lv_obj_add_flag(drawer, LV_OBJ_FLAG_HIDDEN);  // Start hidden
    lv_obj_add_flag(drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(drawer, LV_DIR_VER);
    
    // Set flex layout for drawer
    lv_obj_set_layout(drawer, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(drawer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
    
    // Close button in drawer (top)
    lv_obj_t * close_btn = lv_btn_create(drawer);
    lv_obj_t * close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label,  LV_SYMBOL_CLOSE);
    lv_obj_set_size(close_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    
    lv_obj_t * cb = lv_checkbox_create(drawer);
    lv_obj_set_width(cb, LV_PCT(100));
    lv_checkbox_set_text(cb, "Search");

    // Event callback to open drawer using lambda
    lv_obj_add_event_cb(conf_btn, [](lv_event_t * e) {
        lv_obj_t * dr = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_clear_flag(dr, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, dr);
        lv_anim_set_values(&a, lv_obj_get_x(dr), LV_HOR_RES - lv_obj_get_width(dr));  // Slide from right
        lv_anim_set_time(&a, 250);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
        lv_anim_start(&a);
    }, LV_EVENT_CLICKED, drawer);
    
    // Event callback to close drawer using lambda
    lv_obj_add_event_cb(close_btn, [](lv_event_t * e) {
        lv_obj_t * dr = (lv_obj_t *)lv_event_get_user_data(e);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, dr);
        lv_anim_set_values(&a, lv_obj_get_x(dr), LV_HOR_RES);  // Slide back to right
        lv_anim_set_time(&a, 250);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
        lv_anim_set_ready_cb(&a, [](lv_anim_t * anim) {
            lv_obj_t * d = (lv_obj_t *)anim->var;
            lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        });
        lv_anim_start(&a);
    }, LV_EVENT_CLICKED, drawer);
    
    touch_indicator = lv_label_create(lv_scr_act());
    lv_label_set_text(touch_indicator, "x");
    lv_obj_set_style_text_font(touch_indicator, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(touch_indicator, lv_color_hex(0xFF0000), 0);
    lv_obj_add_flag(touch_indicator, LV_OBJ_FLAG_HIDDEN);
    
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void ta_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_obj_t * kb = (lv_obj_t *)lv_event_get_user_data(e);

    if (code == LV_EVENT_FOCUSED) {
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb, obj);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb, NULL);
    }
}

static esp_err_t mount_fatfs(void) {
    ESP_LOGI(TAG, "--- Mounting FAT Filesystem ---");
    const char *base_path = "/storage";
    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = true;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = CONFIG_WL_SECTOR_SIZE;

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(base_path, "fatfs", &mount_config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS partition: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "FATFS mounted successfully.");
    
    ESP_LOGI(TAG, "Listing all files and directories in /storage:");
    DIR *dir = opendir("/storage");
    if (dir) {
        struct dirent *entry;
        int item_count = 0;
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGI(TAG, "- %s (type: %d)", entry->d_name, entry->d_type);
            item_count++;
        }
        closedir(dir);
        ESP_LOGI(TAG, "Total items in /storage: %d", item_count);
    } else {
        ESP_LOGE(TAG, "Failed to open directory /storage for listing");
    }
    return ESP_OK;
}

static void init_display(void) {
    lcd.init();
    lcd.setRotation(1);
    lcd.clear(lcd.color565(0, 0, 0));
    lcd.setBrightness(255);

    if (mount_fatfs() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS");
    }
    
    uint16_t caldata[8];
    bool cal_loaded = load_calibration_data(caldata);
    if (!cal_loaded) {
        perform_calibration(caldata);
        save_calibration_data(caldata);
    }
    lcd.setTouchCalibrate(caldata);

    lv_init();
    lv_tick_set_cb(get_tick_ms);

    uint32_t width = lcd.width();
    uint32_t height = lcd.height();
    uint32_t buf_size_bytes = width * height;
    uint32_t *disp_buf1 = (uint32_t *)malloc(buf_size_bytes);
    uint32_t *disp_buf2 = (uint32_t *)malloc(buf_size_bytes);
    if (!disp_buf1 || !disp_buf2) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        vTaskDelete(NULL);
        return;
    }

    lv_display_t *disp = lv_display_create(width, height);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, disp_buf1, disp_buf2, buf_size_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
    lv_indev_set_disp(indev, disp);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
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

static void init_console(void) {
    // Configure UART for console I/O
    uart_config_t uart_config = {};
    uart_config.baud_rate = 115200;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_APB;

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
    esp_vfs_dev_uart_use_driver(UART_NUM_0);

    // Initialize console
    esp_console_config_t console_config = {};
    console_config.max_cmdline_length = 256;
    console_config.max_cmdline_args = 8;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    console_config.hint_color = atoi("36"); // ANSI color code for cyan
#endif

    ESP_ERROR_CHECK(esp_console_init(&console_config));

    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(10);
    linenoiseAllowEmpty(false);
}

void print_citations(cJSON* metadata) {
    if (!cJSON_HasObjectItem(metadata, "groundingSupports") || !cJSON_HasObjectItem(metadata, "groundingChunks")) {
        return;
    }

    cJSON *supports = cJSON_GetObjectItem(metadata, "groundingSupports");
    cJSON *chunks = cJSON_GetObjectItem(metadata, "groundingChunks");

    if (!cJSON_IsArray(supports) || !cJSON_IsArray(chunks)) {
        return;
    }

    std::set<int> used_indices;
    int num_sup = cJSON_GetArraySize(supports);
    for (int i = 0; i < num_sup; i++) {
        cJSON *sup = cJSON_GetArrayItem(supports, i);
        cJSON *indices = cJSON_GetObjectItem(sup, "groundingChunkIndices");
        if (!indices || !cJSON_IsArray(indices)) continue;
        int num_idx = cJSON_GetArraySize(indices);
        for (int k = 0; k < num_idx; k++) {
            cJSON *i_json = cJSON_GetArrayItem(indices, k);
            if (!i_json || !cJSON_IsNumber(i_json)) continue;
            int idx = i_json->valueint;
            if (idx >= 0 && idx < cJSON_GetArraySize(chunks)) {
                used_indices.insert(idx);
            }
        }
    }

    if (used_indices.empty()) return;

    printf("\nCitations:\n");
    for (int idx : used_indices) {
        cJSON *chunk = cJSON_GetArrayItem(chunks, idx);
        if (!chunk || !cJSON_HasObjectItem(chunk, "web")) continue;
        cJSON *web = cJSON_GetObjectItem(chunk, "web");
        if (!web || !cJSON_HasObjectItem(web, "uri")) continue;
        cJSON *uri_json = cJSON_GetObjectItem(web, "uri");
        if (!uri_json || !cJSON_IsString(uri_json)) continue;
        printf("[%d] %s\n", idx + 1, uri_json->valuestring);
    }
}

void process_data_line(const std::string& line, HttpData* data) {
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
                                printf("%s", text->valuestring);
                                data->thoughts += text->valuestring;
                            } else {
                                if (data->answer.empty()) {
                                    printf("Answer:\n");
                                }
                                printf("%s", text->valuestring);
                                data->answer += text->valuestring;
                            }
                        }
                    }
                }
            }
            // Handle grounding metadata
            cJSON *gmeta = cJSON_GetObjectItem(candidate, "groundingMetadata");
            if (gmeta && cJSON_IsObject(gmeta)) {
                if (data->grounding_metadata) {
                    cJSON_Delete(data->grounding_metadata);
                }
                data->grounding_metadata = cJSON_Duplicate(gmeta, 1);
            }
        }
    }
    cJSON_Delete(json);
}

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
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

void process_full_buffer(HttpData* data) {
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

void http_task(void *pvParameters) {
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    if (xSemaphoreTake(wifi_connected, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        char *line = linenoise("Enter prompt> ");
        if (!line) continue;
        if (strlen(line) == 0) {
            free(line);
            continue;
        }

        HttpData data;
        data.response_buffer.reserve(1024);
        data.grounding_metadata = nullptr;

        // Build request JSON with user prompt
        cJSON *root = cJSON_CreateObject();
        cJSON *contents = cJSON_AddArrayToObject(root, "contents");
        cJSON *content = cJSON_CreateObject();
        cJSON_AddItemToArray(contents, content);
        cJSON *parts = cJSON_AddArrayToObject(content, "parts");
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", line);
        cJSON_AddItemToArray(parts, part);

        cJSON *tools = cJSON_AddArrayToObject(root, "tools");
        cJSON *tool_obj = cJSON_CreateObject();
        cJSON_AddObjectToObject(tool_obj, "google_search");
        cJSON_AddItemToArray(tools, tool_obj);

        cJSON *generationConfig = cJSON_AddObjectToObject(root, "generationConfig");
        cJSON *thinkingConfig = cJSON_AddObjectToObject(generationConfig, "thinkingConfig");
        cJSON_AddBoolToObject(thinkingConfig, "includeThoughts", 1);
        char *post_data = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        char url[256];
        snprintf(url, sizeof(url),
                 "https://generativelanguage.googleapis.com/v1beta/models/"
                 "gemini-flash-latest:streamGenerateContent?alt=sse&key=%s",
                 API_KEY);

        esp_http_client_config_t config = {};
        config.url = url;
        config.method = HTTP_METHOD_POST;
        config.event_handler = http_event_handler;
        config.user_data = &data;
        config.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));

        ESP_LOGI(TAG, "Sending prompt: %s", line);
        esp_err_t err = esp_http_client_perform(client);
        process_full_buffer(&data);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP POST Status = %d",
                     esp_http_client_get_status_code(client));
            printf("\n");
            if (data.grounding_metadata) {
                print_citations(data.grounding_metadata);
                cJSON_Delete(data.grounding_metadata);
            } else {
                printf("No grounding metadata available.\n");
            }
        } else {
            ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
        free(post_data);
        linenoiseHistoryAdd(line);
        free(line);
    }
}

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_console();

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

    init_display();
    
    xTaskCreate(lvgl_task, "lvgl", 20 * 1024, NULL, 5, NULL);
    xTaskCreate(http_task, "http_task", 20 * 1024, NULL, 5, NULL);

    while (true) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
