#include "Page_BootLogo.h"
#include "Boot_Logo_Api.h"
#include "UI_Navigation.h"

esp_err_t page_boot_logo_render(uint8_t subpage_index, lv_obj_t *root)
{
    (void)subpage_index;
    return boot_logo_display(root, ux_navigation_boot_logo_startup_timing());
}

uint8_t page_boot_logo_get_subpage_count(void)
{
    return 1;
}
