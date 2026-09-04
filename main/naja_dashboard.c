#include "naja_dashboard.h"
#include "naja_dashboard_internal.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "btc_price.h"
#include "connect.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

static const char *TAG = "naja_dashboard";

dashboard_state_t dashboard = {
    .rssi = -128,
};

static void update_rssi(int64_t now_us)
{
    if (dashboard.rssi_updated_at_us != 0 && now_us - dashboard.rssi_updated_at_us < DASHBOARD_RSSI_REFRESH_US) {
        return;
    }

    int8_t rssi = -128;
    if (dashboard.global_state->SYSTEM_MODULE.is_connected && get_wifi_current_rssi(&rssi) != ESP_OK) {
        rssi = -128;
    }

    dashboard.rssi = rssi;
    dashboard.rssi_updated_at_us = now_us;
}

static void update_header(const btc_price_snapshot_t *price)
{
    char amount[32];
    if (price->valid) {
        dashboard_format_usd(price->usd, price->stale, amount, sizeof(amount));
        dashboard_set_label_format(dashboard.header_price, "BTC %s", amount);
    } else {
        dashboard_set_label_text(dashboard.header_price, "BTC $--");
    }

    if (dashboard.rssi > -128) {
        dashboard_set_label_format(dashboard.header_rssi, "%ddBm", dashboard.rssi);
    } else {
        dashboard_set_label_text(dashboard.header_rssi, "--dBm");
    }
    dashboard_update_bars(dashboard.header_bars, dashboard.rssi, COLOR_PRIMARY);
}

static void update_home(const btc_price_snapshot_t *price)
{
    SystemModule *module = &dashboard.global_state->SYSTEM_MODULE;
    PowerManagementModule *power = &dashboard.global_state->POWER_MANAGEMENT_MODULE;

    char amount[32];
    if (price->valid) {
        dashboard_format_usd(price->usd, price->stale, amount, sizeof(amount));
        dashboard_set_label_text(dashboard.home_price, amount);
    } else {
        dashboard_set_label_text(dashboard.home_price, "$--");
    }

    if (price->valid && price->has_change_24h) {
        dashboard_set_label_format(dashboard.home_change, "24H %+.2f%%", price->change_24h);
        lv_obj_set_style_text_color(
            dashboard.home_change,
            lv_color_hex(
                price->change_24h > 0.0 ? COLOR_POSITIVE :
                price->change_24h < 0.0 ? COLOR_NEGATIVE : COLOR_PRIMARY
            ),
            LV_PART_MAIN
        );
    } else {
        dashboard_set_label_text(dashboard.home_change, "24H --");
        lv_obj_set_style_text_color(dashboard.home_change, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    }

    if (module->show_new_block) {
        dashboard_set_label_text(dashboard.home_status, "BLOCK FOUND!");
        lv_obj_set_style_text_color(dashboard.home_status, lv_color_hex(COLOR_BITCOIN), LV_PART_MAIN);
    } else if (price->valid) {
        char age[32];
        dashboard_format_age(price->age_seconds, age, sizeof(age));
        dashboard_set_label_format(
            dashboard.home_status,
            "%s | %s%s",
            btc_price_source_name(price->source),
            age,
            price->stale ? " | STALE" : ""
        );
        lv_obj_set_style_text_color(
            dashboard.home_status,
            lv_color_hex(price->stale ? COLOR_NEGATIVE : COLOR_MUTED),
            LV_PART_MAIN
        );
    } else {
        dashboard_set_label_text(dashboard.home_status, "Fetching BTC price (mempool fallback)");
        lv_obj_set_style_text_color(dashboard.home_status, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    }

    char value[32];
    dashboard_format_hashrate(module->current_hashrate, value, sizeof(value));
    dashboard_set_label_text(dashboard.home_hashrate, value);

    if (isfinite(power->power) && power->power > 0.0f) {
        dashboard_set_label_format(dashboard.home_power, "%.1f W", power->power);
    } else {
        dashboard_set_label_text(dashboard.home_power, "--");
    }

    if (isfinite(power->power) && power->power > 0.0f &&
        isfinite(module->current_hashrate) && module->current_hashrate > 0.0f) {
        dashboard_set_label_format(
            dashboard.home_efficiency,
            "%.1f J/TH",
            power->power / (module->current_hashrate / 1000.0f)
        );
    } else {
        dashboard_set_label_text(dashboard.home_efficiency, "--");
    }
}

static void update_mining(void)
{
    SystemModule *module = &dashboard.global_state->SYSTEM_MODULE;

    char value[40];
    dashboard_format_hashrate(module->current_hashrate, value, sizeof(value));
    dashboard_set_label_text(dashboard.mining_hashrate, value);

    dashboard_set_label_text(
        dashboard.mining_session_best,
        module->best_session_diff_string[0] != '\0' ? module->best_session_diff_string : "--"
    );
    dashboard_set_label_format(
        dashboard.mining_shares,
        "%" PRIu64 " / %" PRIu64,
        module->shares_accepted,
        module->shares_rejected
    );

    dashboard_format_compact_double(dashboard.global_state->pool_difficulty, value, sizeof(value));
    dashboard_set_label_text(dashboard.mining_pool_diff, value);

    if (dashboard.global_state->stratum_protocol == STRATUM_PROTOCOL_V2 || dashboard.global_state->block_height <= 0) {
        dashboard_set_label_text(
            dashboard.mining_block,
            dashboard.global_state->stratum_protocol == STRATUM_PROTOCOL_V2 ? "SV2" : "--"
        );
    } else {
        dashboard_set_label_format(dashboard.mining_block, "%d", dashboard.global_state->block_height);
    }

    dashboard_set_label_text(
        dashboard.mining_all_time_best,
        module->best_diff_string[0] != '\0' ? module->best_diff_string : "--"
    );
}

static void update_hardware(void)
{
    SystemModule *module = &dashboard.global_state->SYSTEM_MODULE;
    PowerManagementModule *power = &dashboard.global_state->POWER_MANAGEMENT_MODULE;

    if (isfinite(power->chip_temp_avg) && power->chip_temp_avg > 0.0f) {
        dashboard_set_label_format(dashboard.hardware_asic1, "%.1f C", power->chip_temp_avg);
    } else {
        dashboard_set_label_text(dashboard.hardware_asic1, "--");
    }

    if (isfinite(power->chip_temp2_avg) && power->chip_temp2_avg > 0.0f) {
        dashboard_set_label_format(dashboard.hardware_asic2, "%.1f C", power->chip_temp2_avg);
    } else {
        dashboard_set_label_text(dashboard.hardware_asic2, "--");
    }

    if (power->fan2_rpm > 0) {
        dashboard_set_label_format(
            dashboard.hardware_fan,
            "%u/%u",
            (unsigned)power->fan_rpm,
            (unsigned)power->fan2_rpm
        );
    } else if (power->fan_rpm > 0) {
        dashboard_set_label_format(dashboard.hardware_fan, "%u RPM", (unsigned)power->fan_rpm);
    } else {
        dashboard_set_label_text(dashboard.hardware_fan, "--");
    }

    if (isfinite(power->power) && power->power > 0.0f) {
        dashboard_set_label_format(dashboard.hardware_power, "%.1f W", power->power);
    } else {
        dashboard_set_label_text(dashboard.hardware_power, "--");
    }

    if (isfinite(power->voltage) && power->voltage > 0.0f) {
        dashboard_set_label_format(dashboard.hardware_vin, "%.1f V", power->voltage / 1000.0f);
    } else {
        dashboard_set_label_text(dashboard.hardware_vin, "--");
    }

    if (isfinite(power->current) && power->current > 0.0f) {
        dashboard_set_label_format(dashboard.hardware_current, "%.1f A", power->current / 1000.0f);
    } else {
        dashboard_set_label_text(dashboard.hardware_current, "--");
    }

    if (isfinite(power->core_voltage) && power->core_voltage > 0.0f) {
        dashboard_set_label_format(dashboard.hardware_vcore, "%.2f V", power->core_voltage / 1000.0f);
    } else {
        dashboard_set_label_text(dashboard.hardware_vcore, "--");
    }

    if (isfinite(power->actual_frequency) && power->actual_frequency > 0.0f) {
        dashboard_set_label_format(dashboard.hardware_frequency, "%.0f MHz", power->actual_frequency);
    } else if (isfinite(power->frequency_value) && power->frequency_value > 0.0f) {
        dashboard_set_label_format(dashboard.hardware_frequency, "%.0f MHz", power->frequency_value);
    } else {
        dashboard_set_label_text(dashboard.hardware_frequency, "--");
    }

    if (isfinite(power->power) && power->power > 0.0f &&
        isfinite(module->current_hashrate) && module->current_hashrate > 0.0f) {
        dashboard_set_label_format(
            dashboard.hardware_efficiency,
            "%.1f J/TH",
            power->power / (module->current_hashrate / 1000.0f)
        );
    } else {
        dashboard_set_label_text(dashboard.hardware_efficiency, "--");
    }
}

static void update_network(void)
{
    SystemModule *module = &dashboard.global_state->SYSTEM_MODULE;

    dashboard_set_label_text(
        dashboard.network_ssid,
        module->ssid != NULL && module->ssid[0] != '\0' ? module->ssid : "--"
    );
    dashboard_set_label_text(
        dashboard.network_ip,
        module->ip_addr_str[0] != '\0' ? module->ip_addr_str : "--"
    );

    if (dashboard.rssi > -128) {
        dashboard_set_label_format(
            dashboard.network_rssi,
            "%d dBm  %s",
            dashboard.rssi,
            dashboard_rssi_quality(dashboard.rssi)
        );
    } else {
        dashboard_set_label_text(dashboard.network_rssi, "-- dBm  --");
    }
    dashboard_update_bars(dashboard.network_bars, dashboard.rssi, COLOR_BITCOIN);

    char uptime[32];
    dashboard_format_uptime(module->start_time_us, uptime, sizeof(uptime));
    dashboard_set_label_text(dashboard.network_uptime, uptime);
    dashboard_set_label_text(
        dashboard.network_firmware,
        module->version != NULL && module->version[0] != '\0' ? module->version : "--"
    );
}

static void attach_when_stats_screen_is_stable(int64_t now_us)
{
    lv_obj_t *active = lv_screen_active();
    if (active == NULL) {
        return;
    }

    if (active != dashboard.candidate_parent) {
        dashboard.candidate_parent = active;
        dashboard.candidate_since_us = now_us;
        return;
    }

    if (now_us - dashboard.candidate_since_us >= DASHBOARD_PARENT_STABLE_US) {
        dashboard_create_ui(active);
    }
}

static void handle_page_rotation(int64_t now_us)
{
    if (dashboard.parent_screen != lv_screen_active()) {
        dashboard.activity_initialized = false;
        return;
    }

    const uint32_t inactive_ms = lv_display_get_inactive_time(NULL);

    if (dashboard.global_state->SYSTEM_MODULE.show_new_block) {
        if (dashboard.current_page != DASH_PAGE_HOME) {
            dashboard_set_page(DASH_PAGE_HOME);
        } else {
            dashboard.page_changed_at_us = now_us;
        }
        dashboard.previous_inactive_ms = inactive_ms;
        dashboard.activity_initialized = true;
        return;
    }

    if (!dashboard.activity_initialized) {
        dashboard.previous_inactive_ms = inactive_ms;
        dashboard.activity_initialized = true;
        dashboard.page_changed_at_us = now_us;
        return;
    } else {
        const bool activity_reset =
            inactive_ms < DASHBOARD_UPDATE_MS + 100 &&
            dashboard.previous_inactive_ms > inactive_ms + 100;
        dashboard.previous_inactive_ms = inactive_ms;

        // ESP-Miner resets LVGL activity when the large-display button is
        // pressed. Use that reset as a request for the next dashboard page.
        if (activity_reset) {
            dashboard_set_page((dashboard.current_page + 1) % DASH_PAGE_COUNT);
        }
    }

    if (now_us - dashboard.page_changed_at_us >= DASHBOARD_PAGE_ROTATE_US) {
        dashboard_set_page((dashboard.current_page + 1) % DASH_PAGE_COUNT);
    }
}

static void dashboard_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (dashboard.global_state == NULL) {
        return;
    }

    SystemModule *module = &dashboard.global_state->SYSTEM_MODULE;
    const bool dashboard_allowed =
        module->is_connected &&
        !module->ap_enabled &&
        !module->is_firmware_update &&
        !module->overheat_mode &&
        !module->hardware_fault &&
        module->asic_status == NULL;

    if (!dashboard_allowed) {
        if (dashboard.root != NULL && lv_obj_is_valid(dashboard.root)) {
            lv_obj_add_flag(dashboard.root, LV_OBJ_FLAG_HIDDEN);
        }
        dashboard.candidate_parent = NULL;
        dashboard.candidate_since_us = 0;
        dashboard.activity_initialized = false;
        return;
    }

    const int64_t now_us = esp_timer_get_time();

    if (dashboard.root != NULL && !lv_obj_is_valid(dashboard.root)) {
        dashboard_reset_ui();
    }

    if (dashboard.root == NULL) {
        attach_when_stats_screen_is_stable(now_us);
        return;
    }

    lv_obj_clear_flag(dashboard.root, LV_OBJ_FLAG_HIDDEN);

    if (!dashboard.price_task_started && now_us >= dashboard.price_task_retry_at_us) {
        const esp_err_t result = btc_price_start(dashboard.global_state);
        if (result == ESP_OK) {
            dashboard.price_task_started = true;
        } else {
            ESP_LOGW(TAG, "BTC price task is unavailable: %s", esp_err_to_name(result));
            dashboard.price_task_retry_at_us = now_us + DASHBOARD_PRICE_TASK_RETRY_US;
        }
    }

    update_rssi(now_us);

    btc_price_snapshot_t price;
    btc_price_get_snapshot(&price);
    update_header(&price);
    update_home(&price);
    update_mining();
    update_hardware();
    update_network();

    if (dashboard.global_state->SYSTEM_MODULE.block_found != dashboard.last_block_found) {
        dashboard.last_block_found = dashboard.global_state->SYSTEM_MODULE.block_found;
        dashboard_set_page(DASH_PAGE_HOME);
    }

    handle_page_rotation(now_us);
}

esp_err_t naja_dashboard_start(GlobalState *global_state)
{
    if (global_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dashboard.timer != NULL) {
        return ESP_OK;
    }

    dashboard.global_state = global_state;
    dashboard.rssi = -128;

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "Timed out waiting for the LVGL lock");
        return ESP_ERR_TIMEOUT;
    }

    dashboard.timer = lv_timer_create(dashboard_timer_cb, DASHBOARD_UPDATE_MS, NULL);
    lvgl_port_unlock();

    if (dashboard.timer == NULL) {
        ESP_LOGE(TAG, "Failed to create dashboard timer");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Naja/Gamma large-display dashboard started");
    return ESP_OK;
}
