#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "LGFX.hpp"  // Your configured LGFX.hpp

LGFX lcd;  // Declare the display object

extern "C" void app_main(void) {
    // Initialize the display
    lcd.init();
    lcd.setRotation(0);  // Set rotation as needed (0-3)
    lcd.fillScreen(TFT_BLACK);  // Clear screen to black

    // Test 1: Draw a filled rectangle
    lcd.fillRect(10, 10, 100, 60, TFT_RED);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Test 2: Draw a circle
    lcd.fillCircle(160, 120, 50, TFT_GREEN);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Test 3: Draw text
    lcd.setTextColor(TFT_WHITE);
    lcd.setTextSize(2);
    lcd.drawString("Hello LovyanGFX!", 50, 200);
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Test 4: Draw a line
    lcd.drawLine(0, 0, lcd.width() - 1, lcd.height() - 1, TFT_BLUE);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Test 5: Fill screen with gradient-like pattern (simple loop)
    for (int i = 0; i < lcd.width(); i++) {
        lcd.drawFastVLine(i, 0, lcd.height(), lcd.color565(i * 2, 255 - i * 2, i));
    }

    // Infinite loop to keep the task alive
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
