#include "naja_dashboard_internal.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "naja_dashboard_ui";

extern const lv_font_t ui_font_DigitalNumbers28;
extern const lv_font_t ui_font_OpenSansBold13;
extern const lv_font_t ui_font_OpenSansBold14;

static void set_container_style(lv_obj_t *object, uint32_t background, lv_opa_t opacity)
{
    lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(object, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(object, lv_color_hex(background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(object, opacity, LV_PART_MAIN);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *create_label(
    lv_obj_t *parent,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const lv_font_t *font,
    uint32_t color,
    const char *text,
    lv_text_align_t align
)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *create_bar(lv_obj_t *parent, int32_t x, int32_t baseline_y, int32_t width, int32_t height)
{
    lv_obj_t *bar = lv_obj_create(parent);
    set_container_style(bar, COLOR_INACTIVE, LV_OPA_COVER);
    lv_obj_set_pos(bar, x, baseline_y - height);
    lv_obj_set_size(bar, width, height);
    return bar;
}

static lv_obj_t *create_metric(
    lv_obj_t *parent,
    int32_t x,
    int32_t y,
    int32_t width,
    const char *title
)
{
    create_label(parent, x, y, width, 14, &ui_font_OpenSansBold13, COLOR_MUTED, title, LV_TEXT_ALIGN_LEFT);
    return create_label(parent, x, y + 15, width, 18, &ui_font_OpenSansBold14, COLOR_PRIMARY, "--", LV_TEXT_ALIGN_LEFT);
}

void dashboard_set_label_text(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }

    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void dashboard_set_label_format(lv_obj_t *label, const char *format, ...)
{
    char buffer[96];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    dashboard_set_label_text(label, buffer);
}

static void format_grouped_u64(uint64_t value, char *buffer, size_t buffer_size)
{
    char digits[32];
    const int digit_count = snprintf(digits, sizeof(digits), "%" PRIu64, value);
    if (digit_count <= 0 || buffer_size == 0) {
        if (buffer_size > 0) {
            buffer[0] = '\0';
        }
        return;
    }

    const size_t comma_count = ((size_t)digit_count - 1) / 3;
    if ((size_t)digit_count + comma_count + 1 > buffer_size) {
        snprintf(buffer, buffer_size, "%" PRIu64, value);
        return;
    }

    size_t output = 0;
    for (int input = 0; input < digit_count; input++) {
        if (input > 0 && (digit_count - input) % 3 == 0) {
            buffer[output++] = ',';
        }
        buffer[output++] = digits[input];
    }
    buffer[output] = '\0';
}

void dashboard_format_usd(double usd, bool approximate, char *buffer, size_t buffer_size)
{
    const uint64_t rounded = isfinite(usd) && usd > 0.0 ? (uint64_t)(usd + 0.5) : 0;
    char grouped[32];
    format_grouped_u64(rounded, grouped, sizeof(grouped));
    snprintf(buffer, buffer_size, "%s$%s", approximate ? "~" : "", grouped);
}

void dashboard_format_hashrate(float hashrate_ghs, char *buffer, size_t buffer_size)
{
    if (!isfinite(hashrate_ghs) || hashrate_ghs <= 0.0f) {
        snprintf(buffer, buffer_size, "--");
    } else if (hashrate_ghs >= 1000.0f) {
        snprintf(buffer, buffer_size, "%.2f TH/s", hashrate_ghs / 1000.0f);
    } else {
        snprintf(buffer, buffer_size, "%.0f GH/s", hashrate_ghs);
    }
}

void dashboard_format_compact_double(double value, char *buffer, size_t buffer_size)
{
    static const struct {
        double divisor;
        const char *suffix;
    } scales[] = {
        {1e15, "P"},
        {1e12, "T"},
        {1e9, "G"},
        {1e6, "M"},
        {1e3, "K"},
    };

    if (!isfinite(value) || value <= 0.0) {
        snprintf(buffer, buffer_size, "--");
        return;
    }

    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        if (value >= scales[i].divisor) {
            const double scaled = value / scales[i].divisor;
            if (scaled >= 100.0) {
                snprintf(buffer, buffer_size, "%.0f%s", scaled, scales[i].suffix);
            } else if (scaled >= 10.0) {
                snprintf(buffer, buffer_size, "%.1f%s", scaled, scales[i].suffix);
            } else {
                snprintf(buffer, buffer_size, "%.2f%s", scaled, scales[i].suffix);
            }
            return;
        }
    }

    snprintf(buffer, buffer_size, "%.0f", value);
}

void dashboard_format_uptime(int64_t start_time_us, char *buffer, size_t buffer_size)
{
    uint32_t seconds = (uint32_t)((esp_timer_get_time() - start_time_us) / 1000000LL);
    const uint32_t days = seconds / 86400U;
    seconds %= 86400U;
    const uint32_t hours = seconds / 3600U;
    seconds %= 3600U;
    const uint32_t minutes = seconds / 60U;

    if (days > 0) {
        snprintf(buffer, buffer_size, "%" PRIu32 "d %" PRIu32 "h %" PRIu32 "m", days, hours, minutes);
    } else if (hours > 0) {
        snprintf(buffer, buffer_size, "%" PRIu32 "h %" PRIu32 "m", hours, minutes);
    } else {
        snprintf(buffer, buffer_size, "%" PRIu32 "m", minutes);
    }
}

void dashboard_format_age(uint32_t age_seconds, char *buffer, size_t buffer_size)
{
    if (age_seconds < 60) {
        snprintf(buffer, buffer_size, "now");
    } else if (age_seconds < 3600) {
        snprintf(buffer, buffer_size, "%" PRIu32 "m ago", age_seconds / 60);
    } else {
        snprintf(
            buffer,
            buffer_size,
            "%" PRIu32 "h %" PRIu32 "m ago",
            age_seconds / 3600,
            (age_seconds % 3600) / 60
        );
    }
}

static int rssi_bar_count(int8_t rssi)
{
    if (rssi <= -128) {
        return 0;
    }
    if (rssi >= -50) {
        return 4;
    }
    if (rssi >= -60) {
        return 3;
    }
    if (rssi >= -70) {
        return 2;
    }
    return 1;
}

const char *dashboard_rssi_quality(int8_t rssi)
{
    if (rssi <= -128) {
        return "--";
    }
    if (rssi >= -50) {
        return "Excellent";
    }
    if (rssi >= -60) {
        return "Good";
    }
    if (rssi >= -70) {
        return "Fair";
    }
    return "Weak";
}

void dashboard_update_bars(lv_obj_t *bars[4], int8_t rssi, uint32_t active_color)
{
    const int active_bars = rssi_bar_count(rssi);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(
            bars[i],
            lv_color_hex(i < active_bars ? active_color : COLOR_INACTIVE),
            LV_PART_MAIN
        );
    }
}

static lv_obj_t *create_page(const char *title, uint32_t title_color)
{
    lv_obj_t *page = lv_obj_create(dashboard.root);
    set_container_style(page, COLOR_BACKGROUND, LV_OPA_COVER);
    lv_obj_set_pos(page, 0, DASHBOARD_HEADER_HEIGHT);
    lv_obj_set_size(page, DASHBOARD_WIDTH, DASHBOARD_PAGE_HEIGHT);
    create_label(page, 8, 5, 304, 18, &ui_font_OpenSansBold14, title_color, title, LV_TEXT_ALIGN_LEFT);
    return page;
}

static void create_header(void)
{
    lv_obj_t *header = lv_obj_create(dashboard.root);
    set_container_style(header, COLOR_HEADER, LV_OPA_COVER);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, DASHBOARD_WIDTH, DASHBOARD_HEADER_HEIGHT);

    dashboard.header_price = create_label(
        header, 8, 3, 154, 18, &ui_font_OpenSansBold14, COLOR_BITCOIN, "BTC $--", LV_TEXT_ALIGN_LEFT
    );
    dashboard.header_page = create_label(
        header, 164, 4, 34, 16, &ui_font_OpenSansBold13, COLOR_MUTED, "1/4", LV_TEXT_ALIGN_CENTER
    );
    dashboard.header_rssi = create_label(
        header, 200, 4, 61, 16, &ui_font_OpenSansBold13, COLOR_MUTED, "--dBm", LV_TEXT_ALIGN_RIGHT
    );

    for (int i = 0; i < 4; i++) {
        dashboard.header_bars[i] = create_bar(header, 266 + (i * 8), 19, 5, 4 + (i * 3));
    }

    lv_obj_t *separator = lv_obj_create(header);
    set_container_style(separator, COLOR_SEPARATOR, LV_OPA_COVER);
    lv_obj_set_pos(separator, 0, DASHBOARD_HEADER_HEIGHT - 1);
    lv_obj_set_size(separator, DASHBOARD_WIDTH, 1);
}

static void create_home_page(void)
{
    lv_obj_t *page = create_page("BITCOIN / HOME", COLOR_BITCOIN);
    dashboard.pages[DASH_PAGE_HOME] = page;

    dashboard.home_price = create_label(
        page, 8, 28, 205, 34, &ui_font_DigitalNumbers28, COLOR_PRIMARY, "$--", LV_TEXT_ALIGN_LEFT
    );
    dashboard.home_change = create_label(
        page, 214, 34, 98, 20, &ui_font_OpenSansBold14, COLOR_MUTED, "24H --", LV_TEXT_ALIGN_RIGHT
    );
    dashboard.home_status = create_label(
        page, 8, 65, 304, 16, &ui_font_OpenSansBold13, COLOR_MUTED, "Waiting for price...", LV_TEXT_ALIGN_LEFT
    );

    lv_obj_t *separator = lv_obj_create(page);
    set_container_style(separator, COLOR_SEPARATOR, LV_OPA_COVER);
    lv_obj_set_pos(separator, 8, 85);
    lv_obj_set_size(separator, 304, 1);

    dashboard.home_hashrate = create_metric(page, 8, 94, 96, "HASHRATE");
    dashboard.home_power = create_metric(page, 112, 94, 88, "POWER");
    dashboard.home_efficiency = create_metric(page, 208, 94, 104, "EFFICIENCY");
}

static void create_mining_page(void)
{
    lv_obj_t *page = create_page("MINING", COLOR_PRIMARY);
    dashboard.pages[DASH_PAGE_MINING] = page;

    dashboard.mining_hashrate = create_metric(page, 8, 29, 144, "HASHRATE");
    dashboard.mining_session_best = create_metric(page, 164, 29, 148, "SESSION BEST");
    dashboard.mining_shares = create_metric(page, 8, 71, 144, "ACCEPT / REJECT");
    dashboard.mining_pool_diff = create_metric(page, 164, 71, 148, "POOL DIFFICULTY");
    dashboard.mining_block = create_metric(page, 8, 113, 144, "BLOCK HEIGHT");
    dashboard.mining_all_time_best = create_metric(page, 164, 113, 148, "ALL-TIME BEST");
}

static void create_hardware_page(void)
{
    lv_obj_t *page = create_page("HARDWARE", COLOR_PRIMARY);
    dashboard.pages[DASH_PAGE_HARDWARE] = page;

    dashboard.hardware_asic1 = create_metric(page, 8, 29, 92, "ASIC 1");
    dashboard.hardware_asic2 = create_metric(page, 112, 29, 92, "ASIC 2");
    dashboard.hardware_fan = create_metric(page, 216, 29, 96, "FAN");

    dashboard.hardware_power = create_metric(page, 8, 71, 92, "POWER");
    dashboard.hardware_vin = create_metric(page, 112, 71, 92, "INPUT");
    dashboard.hardware_current = create_metric(page, 216, 71, 96, "CURRENT");

    dashboard.hardware_vcore = create_metric(page, 8, 113, 92, "VCORE");
    dashboard.hardware_frequency = create_metric(page, 112, 113, 92, "FREQUENCY");
    dashboard.hardware_efficiency = create_metric(page, 216, 113, 96, "EFFICIENCY");
}

static void create_network_page(void)
{
    lv_obj_t *page = create_page("NETWORK", COLOR_PRIMARY);
    dashboard.pages[DASH_PAGE_NETWORK] = page;

    create_label(page, 8, 31, 52, 16, &ui_font_OpenSansBold13, COLOR_MUTED, "SSID", LV_TEXT_ALIGN_LEFT);
    dashboard.network_ssid = create_label(
        page, 64, 29, 248, 18, &ui_font_OpenSansBold14, COLOR_PRIMARY, "--", LV_TEXT_ALIGN_LEFT
    );
    lv_label_set_long_mode(dashboard.network_ssid, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(dashboard.network_ssid, 12000, LV_PART_MAIN);

    create_label(page, 8, 57, 52, 16, &ui_font_OpenSansBold13, COLOR_MUTED, "IP", LV_TEXT_ALIGN_LEFT);
    dashboard.network_ip = create_label(
        page, 64, 55, 248, 18, &ui_font_OpenSansBold14, COLOR_PRIMARY, "--", LV_TEXT_ALIGN_LEFT
    );

    create_label(page, 8, 85, 52, 16, &ui_font_OpenSansBold13, COLOR_MUTED, "SIGNAL", LV_TEXT_ALIGN_LEFT);
    for (int i = 0; i < 4; i++) {
        dashboard.network_bars[i] = create_bar(page, 64 + (i * 12), 99, 8, 6 + (i * 4));
    }
    dashboard.network_rssi = create_label(
        page, 116, 82, 196, 20, &ui_font_OpenSansBold14, COLOR_PRIMARY, "-- dBm  --", LV_TEXT_ALIGN_LEFT
    );

    create_label(page, 8, 111, 52, 16, &ui_font_OpenSansBold13, COLOR_MUTED, "UPTIME", LV_TEXT_ALIGN_LEFT);
    dashboard.network_uptime = create_label(
        page, 64, 109, 248, 18, &ui_font_OpenSansBold14, COLOR_PRIMARY, "--", LV_TEXT_ALIGN_LEFT
    );

    create_label(page, 8, 131, 52, 14, &ui_font_OpenSansBold13, COLOR_MUTED, "FW", LV_TEXT_ALIGN_LEFT);
    dashboard.network_firmware = create_label(
        page, 64, 129, 248, 16, &ui_font_OpenSansBold13, COLOR_PRIMARY, "--", LV_TEXT_ALIGN_LEFT
    );
}

void dashboard_set_page(dashboard_page_t page)
{
    if ((unsigned)page >= DASH_PAGE_COUNT || dashboard.root == NULL) {
        return;
    }

    for (int i = 0; i < DASH_PAGE_COUNT; i++) {
        lv_obj_set_flag(dashboard.pages[i], LV_OBJ_FLAG_HIDDEN, i != (int)page);
    }

    dashboard.current_page = page;
    dashboard.page_changed_at_us = esp_timer_get_time();
    dashboard_set_label_format(dashboard.header_page, "%d/%d", page + 1, DASH_PAGE_COUNT);
}

static void dashboard_root_deleted(lv_event_t *event)
{
    if (lv_event_get_target(event) == dashboard.root) {
        dashboard_reset_ui();
    }
}

void dashboard_create_ui(lv_obj_t *parent)
{
    if (parent == NULL || dashboard.root != NULL) {
        return;
    }
    dashboard.parent_screen = parent;
    dashboard.root = lv_obj_create(parent);
    if (dashboard.root == NULL) {
        dashboard_reset_ui();
        return;
    }
    lv_obj_add_event_cb(dashboard.root, dashboard_root_deleted, LV_EVENT_DELETE, NULL);
    set_container_style(dashboard.root, COLOR_BACKGROUND, LV_OPA_COVER);
    lv_obj_set_pos(dashboard.root, 0, 0);
    lv_obj_set_size(dashboard.root, DASHBOARD_WIDTH, DASHBOARD_HEIGHT);

    create_header();
    create_home_page();
    create_mining_page();
    create_hardware_page();
    create_network_page();
    dashboard_set_page(DASH_PAGE_HOME);

    dashboard.last_block_found = dashboard.global_state->SYSTEM_MODULE.block_found;
    lv_obj_move_foreground(dashboard.root);
    ESP_LOGI(TAG, "Attached four-page dashboard to the 320x170 stats screen");
}

void dashboard_reset_ui(void)
{
    dashboard.parent_screen = NULL;
    dashboard.root = NULL;
    memset(dashboard.pages, 0, sizeof(dashboard.pages));

    dashboard.header_price = NULL;
    dashboard.header_page = NULL;
    dashboard.header_rssi = NULL;
    memset(dashboard.header_bars, 0, sizeof(dashboard.header_bars));

    dashboard.home_price = NULL;
    dashboard.home_change = NULL;
    dashboard.home_status = NULL;
    dashboard.home_hashrate = NULL;
    dashboard.home_power = NULL;
    dashboard.home_efficiency = NULL;

    dashboard.mining_hashrate = NULL;
    dashboard.mining_session_best = NULL;
    dashboard.mining_shares = NULL;
    dashboard.mining_pool_diff = NULL;
    dashboard.mining_block = NULL;
    dashboard.mining_all_time_best = NULL;

    dashboard.hardware_asic1 = NULL;
    dashboard.hardware_asic2 = NULL;
    dashboard.hardware_fan = NULL;
    dashboard.hardware_power = NULL;
    dashboard.hardware_vin = NULL;
    dashboard.hardware_current = NULL;
    dashboard.hardware_vcore = NULL;
    dashboard.hardware_frequency = NULL;
    dashboard.hardware_efficiency = NULL;

    dashboard.network_ssid = NULL;
    dashboard.network_ip = NULL;
    dashboard.network_rssi = NULL;
    dashboard.network_uptime = NULL;
    dashboard.network_firmware = NULL;
    memset(dashboard.network_bars, 0, sizeof(dashboard.network_bars));
}