#include "esp_log.h"
#include "nvs_flash.h"

#include "bt_a2dp.h"
#include "button.h"
#include "leds.h"

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    leds_start();
    button_start();
    ESP_ERROR_CHECK(bt_a2dp_start());
}
