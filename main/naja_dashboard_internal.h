#ifndef NAJA_DASHBOARD_INTERNAL_H_
#define NAJA_DASHBOARD_INTERNAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "global_state.h"
#include "lvgl.h"

#define DASHBOARD_WIDTH 320
#define DASHBOARD_HEIGHT 170
#define DASHBOARD_HEADER_HEIGHT 24
#define DASHBOARD_PAGE_HEIGHT (DASHBOARD_HEIGHT - DASHBOARD_HEADER_HEIGHT)
#define DASHBOARD_UPDATE_MS 500
#define DASHBOARD_PAGE_ROTATE_US 10000000LL
#define DASHBOARD_RSSI_REFRESH_US 5000000LL
#define DASHBOARD_PRICE_TASK_RETRY_US 30000000LL

#define COLOR_BACKGROUND 0x000000
#define COLOR_HEADER 0x111111
#define COLOR_SEPARATOR 0x404040
#define COLOR_PRIMARY 0xFFFFFF
#define COLOR_MUTED 0xA8A8A8
#define COLOR_INACTIVE 0x353535
#define COLOR_BITCOIN 0xF7931A
#define COLOR_POSITIVE 0x4CD97B
#define COLOR_NEGATIVE 0xFF6B6B


typedef enum {
    DASH_PAGE_HOME = 0,
    DASH_PAGE_MINING,
    DASH_PAGE_HARDWARE,
    DASH_PAGE_NETWORK,
    DASH_PAGE_COUNT,
} dashboard_page_t;

typedef struct {
    GlobalState *global_state;
    lv_timer_t *timer;
    lv_obj_t *parent_screen;
    lv_obj_t *root;
    lv_obj_t *pages[DASH_PAGE_COUNT];
    dashboard_page_t current_page;
    int64_t page_changed_at_us;
    int last_block_found;
    int8_t rssi;
    int64_t rssi_updated_at_us;
    bool price_task_started;
    int64_t price_task_retry_at_us;

    lv_obj_t *header_price;
    lv_obj_t *header_page;
    lv_obj_t *header_rssi;
    lv_obj_t *header_bars[4];

    lv_obj_t *home_price;
    lv_obj_t *home_change;
    lv_obj_t *home_status;
    lv_obj_t *home_hashrate;
    lv_obj_t *home_power;
    lv_obj_t *home_efficiency;

    lv_obj_t *mining_hashrate;
    lv_obj_t *mining_session_best;
    lv_obj_t *mining_shares;
    lv_obj_t *mining_pool_diff;
    lv_obj_t *mining_block;
    lv_obj_t *mining_all_time_best;

    lv_obj_t *hardware_asic1;
    lv_obj_t *hardware_asic2;
    lv_obj_t *hardware_fan;
    lv_obj_t *hardware_power;
    lv_obj_t *hardware_vin;
    lv_obj_t *hardware_current;
    lv_obj_t *hardware_vcore;
    lv_obj_t *hardware_frequency;
    lv_obj_t *hardware_efficiency;

    lv_obj_t *network_ssid;
    lv_obj_t *network_ip;
    lv_obj_t *network_rssi;
    lv_obj_t *network_uptime;
    lv_obj_t *network_firmware;
    lv_obj_t *network_bars[4];
} dashboard_state_t;

extern dashboard_state_t dashboard;

void dashboard_set_label_text(lv_obj_t *label, const char *text);
void dashboard_set_label_format(lv_obj_t *label, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void dashboard_format_usd(double usd, bool approximate, char *buffer, size_t buffer_size);
void dashboard_format_hashrate(float hashrate_ghs, char *buffer, size_t buffer_size);
void dashboard_format_compact_double(double value, char *buffer, size_t buffer_size);
void dashboard_format_uptime(int64_t start_time_us, char *buffer, size_t buffer_size);
void dashboard_format_age(uint32_t age_seconds, char *buffer, size_t buffer_size);
const char *dashboard_rssi_quality(int8_t rssi);
void dashboard_update_bars(lv_obj_t *bars[4], int8_t rssi, uint32_t active_color);
void dashboard_set_page(dashboard_page_t page);
void dashboard_create_ui(lv_obj_t *parent);
void dashboard_reset_ui(void);

#endif /* NAJA_DASHBOARD_INTERNAL_H_ */
