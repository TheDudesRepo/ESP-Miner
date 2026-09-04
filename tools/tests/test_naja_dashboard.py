#!/usr/bin/env python3
"""Compile real dashboard/control/cache functions with narrow platform stand-ins.

The source functions are extracted at test time, not copied/reimplemented here.
This checks deterministic behavior, not physical LCD or radio/ASIC operation.
Price parsing is tested separately by the ESP-IDF/QEMU unit-test project.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path: str, name: str) -> str:
    text = (ROOT / path).read_text()
    match = re.search(r"^[\w *]+\b" + re.escape(name) + r"\([^;]*?\)\s*\{", text, re.M)
    if match is None:
        raise AssertionError(f"Function missing: {path}:{name}")
    start = text.index("{", match.start())
    depth = 0
    # Ignore comments and string/character literals while balancing C braces.
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    for token in tokens.finditer(text, start):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return text[match.start():token.end()]
    raise AssertionError(f"Unbalanced function: {name}")


PREAMBLE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include "btc_price_data.h"
#define ESP_OK 0
#define ESP_ERR_INVALID_SIZE 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_FAIL 3
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
typedef struct {const char *server;} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server_name) ((esp_sntp_config_t){server_name})
static bool clock_initialized;
static int clock_init_calls, clock_init_result;
static esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) {
    if (strcmp(config->server,"pool.ntp.org") != 0) {abort();}
    clock_init_calls++;
    return clock_init_result;
}
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#include "btc_price.h"
#define DASHBOARD_WIDTH 320
#define DASHBOARD_HEIGHT 170
#define DASHBOARD_PAGE_ROTATE_US 10000000LL
#define LV_OBJ_FLAG_HIDDEN 1
#define SCR_STATS 9
#define BTC_PRICE_HTTP_RESPONSE_SIZE 2048
#define HTTP_EVENT_ON_CONNECTED 1
#define HTTP_EVENT_ON_DATA 2

typedef struct { bool valid, hidden; char text[96]; } lv_obj_t;
typedef struct {
    bool is_screen_active, is_connected, ap_enabled, is_firmware_update;
    bool overheat_mode, hardware_fault, show_new_block;
    int identify_mode_time_ms;
    char *asic_status;
} SystemModule;
struct GlobalState { SystemModule SYSTEM_MODULE; struct {bool is_active;} SELF_TEST_MODULE; };
static struct GlobalState gs, *GLOBAL_STATE = &gs;
typedef enum {DASH_PAGE_HOME, DASH_PAGE_MINING, DASH_PAGE_HARDWARE, DASH_PAGE_NETWORK, DASH_PAGE_COUNT} dashboard_page_t;
static struct {
    struct GlobalState *global_state;
    lv_obj_t *root, *parent_screen, *pages[DASH_PAGE_COUNT], *header_page;
    dashboard_page_t current_page;
    int64_t page_changed_at_us;
} dashboard;
static lv_obj_t root_object, stats_object, other_object, pages[DASH_PAGE_COUNT], page_label;
static lv_obj_t *active;
static bool display_present;
static int width, height, stock_calls, activity_calls;
static int64_t now_us;
static int64_t esp_timer_get_time(void) {return now_us;}
static void *lv_display_get_default(void) {return display_present ? &stats_object : NULL;}
static int lv_display_get_horizontal_resolution(void *unused) {(void)unused;return width;}
static int lv_display_get_vertical_resolution(void *unused) {(void)unused;return height;}
static lv_obj_t *lv_screen_active(void) {return active;}
static lv_obj_t *screen_get_stats_screen(void) {return &stats_object;}
static bool lv_obj_is_valid(lv_obj_t *obj) {return obj && obj->valid;}
static bool lv_obj_has_flag(lv_obj_t *obj, int flag) {(void)flag;return obj->hidden;}
static void lv_obj_set_flag(lv_obj_t *obj, int flag, bool value) {(void)flag;obj->hidden=value;}
static const char *lv_label_get_text(lv_obj_t *obj) {return obj->text;}
static void lv_label_set_text(lv_obj_t *obj, const char *text) {snprintf(obj->text,sizeof(obj->text),"%s",text);}
static bool use_stats_background(void) {return true;}
static void screen_show(int scr) {(void)scr;stock_calls++;}
static void screen_next(void) {stock_calls++;}
static void lv_display_trigger_activity(void *unused) {(void)unused;activity_calls++;}
typedef struct {btc_price_quote_t quote;bool valid;int64_t fetched_at_us;btc_price_source_t source;} btc_price_cache_t;
static btc_price_cache_t price_cache;
static int price_cache_mux;
typedef struct {char data[BTC_PRICE_HTTP_RESPONSE_SIZE];size_t length;bool overflow;} http_response_t;
typedef struct {int event_id;void *user_data;char *data;int data_len;} esp_http_client_event_t;
'''

TESTS = r'''
static unsigned checks;
#define CHECK(x) do {checks++;if (!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}} while (0)
static void reset(void) {
    memset(&gs,0,sizeof(gs));memset(&dashboard,0,sizeof(dashboard));
    memset(&price_cache,0,sizeof(price_cache));
    root_object=(lv_obj_t){.valid=true};stats_object=(lv_obj_t){.valid=true};
    page_label=(lv_obj_t){.valid=true};
    gs.SYSTEM_MODULE.is_connected=true;gs.SYSTEM_MODULE.is_screen_active=true;
    dashboard.global_state=&gs;dashboard.root=&root_object;dashboard.parent_screen=&stats_object;
    dashboard.header_page=&page_label;
    for (int i=0;i<DASH_PAGE_COUNT;i++){pages[i]=(lv_obj_t){.valid=true};dashboard.pages[i]=&pages[i];}
    active=&stats_object;display_present=true;width=320;height=170;stock_calls=activity_calls=0;
    now_us=1000000;dashboard_set_page(DASH_PAGE_HOME);
}
int main(void) {
    clock_init_result=ESP_FAIL;CHECK(network_clock_start()==ESP_FAIL);CHECK(!clock_initialized);
    clock_init_result=ESP_OK;CHECK(network_clock_start()==ESP_OK);CHECK(clock_initialized);
    CHECK(clock_init_calls==2);CHECK(network_clock_start()==ESP_OK);CHECK(clock_init_calls==2);
    clock_initialized=false;clock_init_result=ESP_ERR_INVALID_STATE;
    CHECK(network_clock_start()==ESP_OK);CHECK(clock_initialized);
    reset();CHECK(dashboard_allowed());
    // Same-phase consecutive clicks: the old inactivity-polling approach missed one.
    screen_button_press();now_us+=500000;screen_button_press();
    CHECK(dashboard.current_page==DASH_PAGE_HARDWARE);CHECK(activity_calls==2);CHECK(stock_calls==0);
    screen_button_press();screen_button_press();CHECK(dashboard.current_page==DASH_PAGE_HOME);
    CHECK(strcmp(page_label.text,"1/4")==0);
    now_us+=9999999;handle_page_rotation(now_us);CHECK(dashboard.current_page==DASH_PAGE_HOME);
    now_us++;handle_page_rotation(now_us);CHECK(dashboard.current_page==DASH_PAGE_MINING);
    // A manual advance starts a new full ten-second timer.
    now_us+=5000000;screen_button_press();now_us+=9999999;handle_page_rotation(now_us);
    CHECK(dashboard.current_page==DASH_PAGE_HARDWARE);
    now_us++;handle_page_rotation(now_us);CHECK(dashboard.current_page==DASH_PAGE_NETWORK);
    reset();gs.SYSTEM_MODULE.identify_mode_time_ms=2000;screen_button_press();
    CHECK(gs.SYSTEM_MODULE.identify_mode_time_ms==0);CHECK(dashboard.current_page==DASH_PAGE_HOME);
    reset();gs.SYSTEM_MODULE.show_new_block=true;screen_button_press();now_us+=20000000;
    handle_page_rotation(now_us);CHECK(dashboard.current_page==DASH_PAGE_HOME);
    reset();width=170;height=320;CHECK(!dashboard_landscape_supported());
    CHECK(!naja_dashboard_next_if_active());CHECK(dashboard.current_page==DASH_PAGE_HOME);
    reset();width=128;height=64;CHECK(!dashboard_landscape_supported());
    reset();display_present=false;CHECK(!naja_dashboard_next_if_active());
    reset();gs.SELF_TEST_MODULE.is_active=true;CHECK(!naja_dashboard_next_if_active());
    reset();gs.SYSTEM_MODULE.overheat_mode=true;CHECK(!naja_dashboard_next_if_active());
    reset();gs.SYSTEM_MODULE.hardware_fault=true;CHECK(!naja_dashboard_next_if_active());
    reset();gs.SYSTEM_MODULE.is_firmware_update=true;CHECK(!naja_dashboard_next_if_active());
    reset();gs.SYSTEM_MODULE.ap_enabled=true;CHECK(!naja_dashboard_next_if_active());
    reset();gs.SYSTEM_MODULE.is_connected=false;CHECK(!naja_dashboard_next_if_active());
    reset();gs.SYSTEM_MODULE.is_screen_active=false;CHECK(!naja_dashboard_next_if_active());
    reset();gs.SYSTEM_MODULE.asic_status="Starting ASIC";CHECK(!naja_dashboard_next_if_active());
    reset();gs.SYSTEM_MODULE.identify_mode_time_ms=1000;CHECK(!naja_dashboard_next_if_active());
    reset();active=&other_object;CHECK(!naja_dashboard_next_if_active());
    reset();dashboard.root=NULL;CHECK(!naja_dashboard_next_if_active());
    reset();root_object.hidden=true;CHECK(!naja_dashboard_next_if_active());
    reset();root_object.valid=false;CHECK(!naja_dashboard_next_if_active());
    reset();dashboard.global_state=NULL;CHECK(!naja_dashboard_next_if_active());
    reset();dashboard.parent_screen=&other_object;active=&other_object;CHECK(!naja_dashboard_next_if_active());
    reset();dashboard_set_page(DASH_PAGE_COUNT);CHECK(dashboard.current_page==DASH_PAGE_HOME);
    char text[64];
    dashboard_format_usd(100000.75,false,text,sizeof(text));CHECK(strcmp(text,"$100,001")==0);
    dashboard_format_usd(100000,true,text,sizeof(text));CHECK(strcmp(text,"~$100,000")==0);
    dashboard_format_hashrate(8000,text,sizeof(text));CHECK(strcmp(text,"8.00 TH/s")==0);
    dashboard_format_hashrate(NAN,text,sizeof(text));CHECK(strcmp(text,"--")==0);
    CHECK(rssi_bar_count(-128)==0);CHECK(rssi_bar_count(-50)==4);CHECK(rssi_bar_count(-51)==3);
    CHECK(rssi_bar_count(-61)==2);CHECK(rssi_bar_count(-71)==1);
    CHECK(strcmp(dashboard_rssi_quality(-60),"Good")==0);
    // Snapshot starts with the provider's age and expires its 24h change as well.
    reset();btc_price_quote_t quote={.usd=100,.change_24h=2,.has_change_24h=true,
        .provider_timestamp=1800000000,.age_at_fetch_seconds=899};
    update_cache(&quote,BTC_PRICE_SOURCE_COINGECKO);
    btc_price_snapshot_t snapshot;btc_price_get_snapshot(&snapshot);
    CHECK(snapshot.valid && !snapshot.stale && snapshot.has_change_24h);
    CHECK(snapshot.age_seconds==899 && snapshot.fetched_age_seconds==0);
    now_us+=1000000;btc_price_get_snapshot(&snapshot);
    CHECK(snapshot.stale && !snapshot.has_change_24h);CHECK(snapshot.age_seconds==900);
    CHECK(snapshot.fetched_age_seconds==1);
    quote.age_at_fetch_seconds=10;quote.has_change_24h=false;quote.change_24h=0;
    update_cache(&quote,BTC_PRICE_SOURCE_MEMPOOL);btc_price_get_snapshot(&snapshot);
    CHECK(snapshot.valid && !snapshot.stale && !snapshot.has_change_24h);
    CHECK(snapshot.source==BTC_PRICE_SOURCE_MEMPOOL && snapshot.age_seconds==10);
    // The callback must bound large responses without an out-of-bounds write.
    http_response_t response={0};char bytes[2048];memset(bytes,'X',sizeof(bytes));
    esp_http_client_event_t event={HTTP_EVENT_ON_DATA,&response,bytes,2047};
    CHECK(http_event_handler(&event)==ESP_OK);CHECK(response.length==2047);
    CHECK(response.data[2047]=='\0');event.data_len=1;
    CHECK(http_event_handler(&event)==ESP_ERR_INVALID_SIZE);CHECK(response.overflow);
    CHECK(response.data[2047]=='\0');event.event_id=HTTP_EVENT_ON_CONNECTED;
    CHECK(http_event_handler(&event)==ESP_OK);CHECK(response.length==0 && !response.overflow);
    event.event_id=HTTP_EVENT_ON_DATA;event.data_len=3;CHECK(http_event_handler(&event)==ESP_OK);
    CHECK(response.length==3 && response.data[3]=='\0');
    printf("PASS: %u dashboard, button, safety, cache, and response-buffer assertions\n",checks);
    return 0;
}
'''


def main() -> None:
    screen = function('main/screen.c', 'screen_button_press')
    assert screen.index('identify_mode_time_ms = 0') < screen.index('naja_dashboard_next_if_active')
    app = (ROOT / 'main/main.c').read_text()
    clock_start = app.index('network_clock_start()')
    assert clock_start < app.index('protocol_coordinator_init(')
    assert clock_start < app.index('naja_dashboard_start(')
    assert 'network_clock.c' in (ROOT / 'main/CMakeLists.txt').read_text()
    dash = (ROOT / 'main/naja_dashboard.c').read_text()
    assert 'previous_inactive_ms' not in dash
    assert 'screen_get_stats_screen()' in dash
    assert 'strcmp(global_state->DEVICE_CONFIG.board_version, "1201")' in dash
    assert 'dashboard_root_deleted, LV_EVENT_DELETE' in (ROOT / 'main/naja_dashboard_ui.c').read_text()
    price = (ROOT / 'main/btc_price.c').read_text()
    assert '.disable_auto_redirect = true' in price and '.crt_bundle_attach = esp_crt_bundle_attach' in price
    assert 'include_last_updated_at=true' in price and 'ensure_wall_clock()' in price
    assert 'CONFIG_MBEDTLS_HAVE_TIME_DATE=y' in (ROOT / 'sdkconfig.defaults').read_text()
    for path in ['main/btc_price.c','main/btc_price_data.c','main/naja_dashboard.c','main/naja_dashboard_ui.c']:
        text = (ROOT / path).read_text()
        assert not re.search(r'\b(?:192\.168\.\d+\.\d+|10\.\d+\.\d+\.\d+|172\.(?:1[6-9]|2\d|3[01])\.\d+\.\d+)\b', text)
        assert not re.search(r'github_pat_|ghp_|Authorization:\s*Bearer', text)
    functions = [function('main/network_clock.c', 'network_clock_start')]
    for name in ['dashboard_set_label_text','dashboard_set_label_format','dashboard_set_page',
                 'format_grouped_u64','dashboard_format_usd','dashboard_format_hashrate',
                 'rssi_bar_count','dashboard_rssi_quality']:
        functions.append(function('main/naja_dashboard_ui.c', name))
    for name in ['dashboard_landscape_supported','dashboard_allowed','naja_dashboard_next_if_active','handle_page_rotation']:
        functions.append(function('main/naja_dashboard.c', name))
    functions.append(screen)
    functions.append(function('main/btc_price_data.c', 'btc_price_age_seconds'))
    for name in ['update_cache','btc_price_get_snapshot','http_event_handler']:
        functions.append(function('main/btc_price.c', name))
    with tempfile.TemporaryDirectory(prefix='naja-tests-') as directory:
        tmp = Path(directory)
        (tmp / 'esp_err.h').write_text('typedef int esp_err_t;\n')
        (tmp / 'test.c').write_text(PREAMBLE + '\n\n'.join(functions) + TESTS)
        command = [os.environ.get('CC','cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                   '-I', str(tmp), '-I', str(ROOT / 'main'), str(tmp / 'test.c'), '-lm', '-o', str(tmp / 'test')]
        subprocess.run(command, check=True)
        subprocess.run([str(tmp / 'test')], check=True)
    print('PASS: integration guards and feature privacy checks')


if __name__ == '__main__':
    main()
