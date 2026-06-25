idf_component_register(
                         SRCS 
                              "./main.c" 
                              "./gauge_bg_lvgl.c"
                              "./EXIO/TCA9554PWR.c"
                              "./LCD_Driver/esp_lcd_st77916/esp_lcd_st77916.c" 
                              "./LCD_Driver/ST77916.c"       
                              "./LVGL_Driver/LVGL_Driver.c"
                              "./LVGL_UI/LVGL_Example.c"
                              "./SD_Card/SD_MMC.c" 
                              "./I2C_Driver/I2C_Driver.c"
                              "./PCF85063/PCF85063.c"
                              "./QMI8658/QMI8658.c"
                              "./BAT_Driver/BAT_Driver.c"
                              "./PWR_Key/PWR_Key.c"
                              "./Wireless/Wireless.c"
                              "./TPMS/tesla_tpms_ble.c"
                              "./TPMS/tpms_parser.c"
                              "./TPMS/tpms_ble_adv.c"
                              "./TPMS/tpms_manager.c"
                              "./Config_Portal/Config_Portal.c"
                              "./Storage_Manager/Storage_Manager.c"
                              "./Boot_Logo/Boot_Logo.c"
                              "./Page_BootLogo/Page_BootLogo.c"
                              "./Page_Clock/Page_Clock.c"
                              "./Page_Speed/Page_Speed.c"
                              "./Page_Acceleration/Page_Acceleration.c"
                              "./Page_Weather/Page_Weather.c"
                              "./Page_Status/Page_Status.c"
                              "./UI_Navigation/UI_Navigation.c"
                              "./Console_SetTime/Console_SetTime.c"
                              "./Datetime_Set/Datetime_Set.c"
                              "./Fonts/font_ddin_115.c"
                              "./Fonts/font_ddin_reg_32.c"
                              "./Fonts/font_ddin_reg_20.c"
                              "./Fonts/font_ddin_reg_44.c"
                              "./Fonts/font_ddin_reg_15.c"

                         INCLUDE_DIRS 
                              "./LCD_Driver/esp_lcd_st77916" 
                              "./LCD_Driver"   
                              "./LVGL_Driver" 
                              "./LVGL_UI" 
                              "./EXIO"
                              "./SD_Card" 
                              "./I2C_Driver"
                              "./PCF85063"
                              "./QMI8658"
                              "./BAT_Driver"
                              "./PWR_Key"
                              "./Wireless"
                              "./TPMS"
                              "./Config_Portal"
                              "./Storage_Manager"
                              "./Boot_Logo"
                              "./Page_BootLogo"
                              "./Page_Clock"
                              "./Page_Speed"
                              "./Page_Acceleration"
                              "./Page_Weather"
                              "./Page_Status"
                              "./UI_Navigation"
                              "./Console_SetTime"
                              "./Datetime_Set"
                              "./Fonts"
                              "."
                       EMBED_TXTFILES
                              "./Config_Portal/web/service.html"
                              "./Config_Portal/web/service.css"
                              "./Config_Portal/web/service.js"
                       )

# Generated lv_font_conv .c files use:
#   #ifdef LV_LVGL_H_INCLUDE_SIMPLE
#   #include "lvgl.h"
#   #else
#   #include "lvgl/lvgl.h"
#   #endif
# We resolve "lvgl.h" via the lvgl__lvgl component's include dir, so force the
# simple include path for this component.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)

# Set to 1 to enable experimental Tesla TPMS GATT gateway debug (serial only).
target_compile_definitions(${COMPONENT_LIB} PRIVATE ENABLE_TESLA_TPMS_DEBUG=1)
