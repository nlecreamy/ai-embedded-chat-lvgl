#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" void app_main(void) {
    printf("Welcome !\n");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
