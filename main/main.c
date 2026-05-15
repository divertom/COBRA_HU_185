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
#include "Boot_Logo_Api.h"
#include "UI_Navigation.h"
#include "Console_SetTime.h"
#include "esp_log.h"

void Driver_Loop(void *parameter)
{
    /* Wireless_Init() runs once from app_main — do not call here or BLE_Init runs twice and aborts. */
    while (1) {
        QMI8658_Loop();
        PCF85063_Loop();
        BAT_Get_Volts();
        PWR_Loop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
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
    } else {
        storage_log_all_files();
    }

    Driver_Init();
    console_settime_task_start();

    // Initialize LCD with backlight OFF
    ESP_LOGI("main", "Initializing LCD (backlight OFF)");
    LCD_Init();  // Backlight will remain OFF (set to 0 in Backlight_Init)
    
    // Initialize LVGL
    ESP_LOGI("main", "Initializing LVGL");
    LVGL_Init();
    
    // Initialize UX navigation and show boot logo for 5 seconds.
    if (ux_navigation_init() != ESP_OK) {
        ESP_LOGE("main", "Failed to initialize UI navigation");
    }
    if (ux_navigation_show_boot_logo() != ESP_OK) {
        ESP_LOGW("main", "Failed to display boot logo, enabling backlight anyway");
        boot_logo_enable_backlight(70);
    }

    // Start WiFi/BLE tasks (including Smartremote BLE HID receive) and hook UI navigation handlers.
    Wireless_RegisterRemoteEventHandler(ux_navigation_queue_remote_event);
    Wireless_Init();

    for (int i = 0; i < 500; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler();
    }

    if (ux_navigation_show_restored_page() != ESP_OK) {
        ESP_LOGW("main", "Failed to show restored page");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        ux_navigation_process_events();
        lv_timer_handler();
    }
}






