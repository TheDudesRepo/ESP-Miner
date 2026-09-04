#include "network_clock.h"

#include <stdbool.h>
#include "esp_netif_sntp.h"

static bool clock_initialized;

esp_err_t network_clock_start(void)
{
    if (clock_initialized) {
        return ESP_OK;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        /* Respect an existing SNTP owner rather than resetting its servers. */
        clock_initialized = true;
        return ESP_OK;
    }
    return err;
}
