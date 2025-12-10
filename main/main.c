#include "ST77916.h"
#include "PCF85063.h"
#include "QMI8658.h"
#include "SD_MMC.h"
#include "Wireless.h"
#include "TCA9554PWR.h"
#include "LVGL_Example.h"
#include "BAT_Driver.h"
#include "PWR_Key.h"
#include "Storage_Manager.h"
#include "Boot_Logo.h"
#include "esp_log.h"

void Driver_Loop(void *parameter)
{
    Wireless_Init();
    while(1)
    {
        QMI8658_Loop();
        PCF85063_Loop();
        BAT_Get_Volts();
        PWR_Loop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}
void Driver_Init(void)
{
    PWR_Init();
    BAT_Init();
    I2C_Init();
    EXIO_Init();                    // Example Initialize EXIO
    Flash_Searching();
    PCF85063_Init();
    QMI8658_Init();
    xTaskCreatePinnedToCore(
        Driver_Loop, 
        "Other Driver task",
        4096, 
        NULL, 
        3, 
        NULL, 
        0);
}
void app_main(void)
{
    // Initialize SPIFFS storage first
    ESP_LOGI("main", "Initializing SPIFFS storage");
    if (storage_init() != ESP_OK) {
        ESP_LOGE("main", "Failed to initialize SPIFFS");
    }

    // Initialize LCD with backlight OFF
    ESP_LOGI("main", "Initializing LCD (backlight OFF)");
    LCD_Init();  // Backlight will remain OFF (set to 0 in Backlight_Init)
    
    // Initialize LVGL
    ESP_LOGI("main", "Initializing LVGL");
    LVGL_Init();
    
    // Display boot logo from SPIFFS storage
    // Note: backlight will be enabled automatically inside boot_logo_display() after logo is drawn
    ESP_LOGI("main", "Displaying boot logo from storage");
    if (boot_logo_display() == ESP_OK) {
        ESP_LOGI("main", "Boot logo displayed successfully");
    } else {
        ESP_LOGW("main", "Failed to display boot logo, enabling backlight anyway");
        boot_logo_enable_backlight(70);
    }

    // Do NOT show any other UI - just keep the logo displayed
    while (1) {
        // Only process LVGL timer handler to keep display updated
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler();
    }
}






