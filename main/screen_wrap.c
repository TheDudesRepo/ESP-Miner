#include "screen.h"

#include "esp_err.h"
#include "esp_log.h"
#include "global_state.h"
#include "naja_dashboard.h"

static const char *TAG = "screen_wrap";

/* Symbols supplied by GNU ld when the matching --wrap options are enabled. */
esp_err_t __real_screen_start(GlobalState *global_state);
void __real_screen_next(void);
void __real_screen_button_press(void);

esp_err_t __wrap_screen_start(GlobalState *global_state)
{
    esp_err_t err = __real_screen_start(global_state);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t dashboard_err = naja_dashboard_start(global_state);
    if (dashboard_err != ESP_OK && dashboard_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Naja dashboard did not start: %s", esp_err_to_name(dashboard_err));
    }

    return err;
}

void __wrap_screen_next(void)
{
    if (!naja_dashboard_next_if_active()) {
        __real_screen_next();
    }
}

void __wrap_screen_button_press(void)
{
    if (!naja_dashboard_next_if_active()) {
        __real_screen_button_press();
    }
}
