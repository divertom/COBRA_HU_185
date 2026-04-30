#include "Page_BootLogo.h"
#include "Boot_Logo_Api.h"

esp_err_t page_boot_logo_render(uint8_t subpage_index)
{
    (void)subpage_index;
    return boot_logo_display();
}

uint8_t page_boot_logo_get_subpage_count(void)
{
    return 1;
}
