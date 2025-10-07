#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "LGFX.hpp"

LGFX lcd;

extern "C" void app_main(void) {
    // Initialize the display
    lcd.init();
    lcd.setRotation(1);
    lcd.setBrightness(128);
    lcd.clear();
    
    lcd.setTextColor(lcd.color565(255, 255, 255));
    lcd.setTextSize(2);
    lcd.setTextDatum(middle_centre);
    lcd.drawString("Hello LovyanGFX!", lcd.width() / 2, lcd.height() / 2);
    vTaskDelay(pdMS_TO_TICKS(10));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
