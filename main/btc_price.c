#include "btc_price.h"
#include "btc_price_data.h"

#include <stddef.h>
#include <string.h>
#include <time.h>

#include "connect.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "global_state.h"

#define BTC_PRICE_COINGECKO_URL \
    "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd&include_24hr_change=true&include_last_updated_at=true&precision=2"
#define BTC_PRICE_MEMPOOL_URL "https://mempool.space/api/v1/prices"
#define BTC_PRICE_HTTP_RESPONSE_SIZE BTC_PRICE_JSON_MAX_BYTES
#define BTC_PRICE_HTTP_TIMEOUT_MS 12000
#define BTC_PRICE_INITIAL_DELAY_MS 10000
#define BTC_PRICE_REFRESH_MS (5 * 60 * 1000)
#define BTC_PRICE_TASK_STACK_SIZE 16384
#define BTC_PRICE_TASK_PRIORITY 1
#define BTC_PRICE_FAILURE_MIN_MS 30000
#define BTC_PRICE_FAILURE_MAX_MS BTC_PRICE_REFRESH_MS
#define BTC_PRICE_TIME_WAIT_MS 20000
#define BTC_PRICE_MIN_FREE_PSRAM (128 * 1024)

static const char *TAG = "btc_price";

typedef struct {
    char data[BTC_PRICE_HTTP_RESPONSE_SIZE];
    size_t length;
    bool overflow;
} http_response_t;

typedef struct {
    btc_price_quote_t quote;
    bool valid;
    int64_t fetched_at_us;
    btc_price_source_t source;
} btc_price_cache_t;

static btc_price_cache_t price_cache;
static portMUX_TYPE price_cache_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t price_task_handle;
static bool sntp_initialized;

static bool ensure_wall_clock(void)
{
    if (btc_price_clock_valid((int64_t)time(NULL))) {
        return true;
    }
    if (!sntp_initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Clock synchronization could not start: %s", esp_err_to_name(err));
            return false;
        }
        /* INVALID_STATE means an existing owner has already initialized SNTP. */
        sntp_initialized = true;
    }
    for (unsigned waited = 0; waited < BTC_PRICE_TIME_WAIT_MS; waited += 500) {
        if (btc_price_clock_valid((int64_t)time(NULL))) {
            return true;
        }
        if (!wifi_is_connected()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "Clock is not synchronized; price requests deferred");
    return false;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL || event->user_data == NULL) {
        return ESP_OK;
    }
    http_response_t *response = (http_response_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        response->length = 0;
        response->overflow = false;
        response->data[0] = '\0';
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }
    const size_t incoming = (size_t)event->data_len;
    const size_t available = sizeof(response->data) - response->length - 1;
    if (incoming > available) {
        response->overflow = true;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(response->data + response->length, event->data, incoming);
    response->length += incoming;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static bool fetch_quote(const char *url, btc_price_source_t source, btc_price_quote_t *quote)
{
    http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms = BTC_PRICE_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &response,
        .user_agent = "ESP-Miner-BTC-Display/1.1",
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        /* Exact HTTPS endpoints only: never follow a downgrade or other host. */
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return false;
    }
    esp_err_t result = esp_http_client_set_header(client, "Accept", "application/json");
    if (result == ESP_OK) {
        result = esp_http_client_set_header(client, "Accept-Encoding", "identity");
    }
    if (result == ESP_OK) {
        result = esp_http_client_perform(client);
    }
    const int status = esp_http_client_get_status_code(client);
    bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK || status != 200 || !complete || response.overflow || response.length == 0) {
        ESP_LOGW(TAG, "Price request rejected (HTTP %d, %s)", status, esp_err_to_name(result));
        return false;
    }
    /* Embedded NULs must not hide a second payload from the JSON parser. */
    if (memchr(response.data, '\0', response.length) != NULL) {
        return false;
    }
    int64_t now_epoch = (int64_t)time(NULL);
    bool valid = source == BTC_PRICE_SOURCE_COINGECKO
        ? btc_price_parse_coingecko(response.data, now_epoch, quote)
        : btc_price_parse_mempool(response.data, now_epoch, quote);
    if (!valid) {
        ESP_LOGW(TAG, "Provider returned an invalid, undated, old or future-dated quote");
    }
    return valid;
}

static void update_cache(const btc_price_quote_t *quote, btc_price_source_t source)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&price_cache_mux);
    price_cache.quote = *quote;
    price_cache.valid = true;
    price_cache.fetched_at_us = now_us;
    price_cache.source = source;
    portEXIT_CRITICAL(&price_cache_mux);
}

static void btc_price_task(void *parameter)
{
    GlobalState *state = parameter;
    uint32_t failure_delay_ms = BTC_PRICE_FAILURE_MIN_MS;
    vTaskDelay(pdMS_TO_TICKS(BTC_PRICE_INITIAL_DELAY_MS));
    while (true) {
        /* This optional task must yield to provisioning, OTA and fault recovery. */
        if (!wifi_is_connected() || state->SYSTEM_MODULE.ap_enabled ||
            state->SYSTEM_MODULE.is_firmware_update || state->SYSTEM_MODULE.overheat_mode ||
            state->SYSTEM_MODULE.hardware_fault || state->SELF_TEST_MODULE.is_active) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (esp_psram_is_initialized() &&
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < BTC_PRICE_MIN_FREE_PSRAM) {
            vTaskDelay(pdMS_TO_TICKS(BTC_PRICE_REFRESH_MS));
            continue;
        }
        bool success = false;
        btc_price_quote_t quote;
        if (ensure_wall_clock()) {
            if (fetch_quote(BTC_PRICE_COINGECKO_URL, BTC_PRICE_SOURCE_COINGECKO, &quote)) {
                update_cache(&quote, BTC_PRICE_SOURCE_COINGECKO);
                success = true;
            } else if (wifi_is_connected() && !state->SYSTEM_MODULE.is_firmware_update &&
                       fetch_quote(BTC_PRICE_MEMPOOL_URL, BTC_PRICE_SOURCE_MEMPOOL, &quote)) {
                update_cache(&quote, BTC_PRICE_SOURCE_MEMPOOL);
                success = true;
            }
        }
        if (success) {
            failure_delay_ms = BTC_PRICE_FAILURE_MIN_MS;
            vTaskDelay(pdMS_TO_TICKS(BTC_PRICE_REFRESH_MS));
        } else {
            ESP_LOGW(TAG, "No fresh BTC quote; retaining cached data");
            vTaskDelay(pdMS_TO_TICKS(failure_delay_ms));
            failure_delay_ms = failure_delay_ms < BTC_PRICE_FAILURE_MAX_MS / 2
                ? failure_delay_ms * 2 : BTC_PRICE_FAILURE_MAX_MS;
        }
    }
}

esp_err_t btc_price_start(GlobalState *global_state)
{
    if (global_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (price_task_handle != NULL) {
        return ESP_OK;
    }
    if (esp_psram_is_initialized() &&
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < BTC_PRICE_MIN_FREE_PSRAM) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t created;
    if (esp_psram_is_initialized()) {
        created = xTaskCreateWithCaps(btc_price_task, "btc_price", BTC_PRICE_TASK_STACK_SIZE,
            global_state, BTC_PRICE_TASK_PRIORITY, &price_task_handle, MALLOC_CAP_SPIRAM);
    } else {
        created = xTaskCreate(btc_price_task, "btc_price", BTC_PRICE_TASK_STACK_SIZE,
            global_state, BTC_PRICE_TASK_PRIORITY, &price_task_handle);
    }
    if (created != pdPASS) {
        price_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void btc_price_get_snapshot(btc_price_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    btc_price_cache_t cached;
    portENTER_CRITICAL(&price_cache_mux);
    cached = price_cache;
    portEXIT_CRITICAL(&price_cache_mux);
    if (!cached.valid) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    snapshot->usd = cached.quote.usd;
    snapshot->change_24h = cached.quote.change_24h;
    snapshot->valid = true;
    snapshot->source = cached.source;
    snapshot->provider_timestamp = cached.quote.provider_timestamp;
    snapshot->age_seconds = btc_price_age_seconds(cached.quote.age_at_fetch_seconds, cached.fetched_at_us, now_us);
    snapshot->fetched_age_seconds = btc_price_age_seconds(0, cached.fetched_at_us, now_us);
    snapshot->stale = snapshot->age_seconds >= BTC_PRICE_STALE_SECONDS;
    snapshot->has_change_24h = cached.quote.has_change_24h && !snapshot->stale;
}

const char *btc_price_source_name(btc_price_source_t source)
{
    switch (source) {
        case BTC_PRICE_SOURCE_COINGECKO: return "CoinGecko";
        case BTC_PRICE_SOURCE_MEMPOOL: return "mempool.space";
        default: return "--";
    }
}
