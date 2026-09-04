#include "btc_price.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "connect.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "global_state.h"

#define BTC_PRICE_COINGECKO_URL \
    "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd&include_24hr_change=true&precision=2"
#define BTC_PRICE_MEMPOOL_URL "https://mempool.space/api/v1/prices"

#define BTC_PRICE_HTTP_RESPONSE_SIZE 2048
#define BTC_PRICE_HTTP_TIMEOUT_MS 12000
#define BTC_PRICE_MAX_USD 1000000000.0
#define BTC_PRICE_INITIAL_DELAY_MS 10000
#define BTC_PRICE_REFRESH_MS (5 * 60 * 1000)
#define BTC_PRICE_STALE_SECONDS (15 * 60)
#define BTC_PRICE_TASK_STACK_SIZE 16384
#define BTC_PRICE_TASK_PRIORITY 1
#define BTC_PRICE_FAILURE_MIN_MS 30000
#define BTC_PRICE_FAILURE_MAX_MS BTC_PRICE_REFRESH_MS

static const char *TAG = "btc_price";

typedef struct {
    char data[BTC_PRICE_HTTP_RESPONSE_SIZE];
    size_t length;
    bool overflow;
} http_response_t;

typedef struct {
    double usd;
    double change_24h;
    bool has_change_24h;
    bool valid;
    int64_t updated_at_us;
    btc_price_source_t source;
} btc_price_cache_t;

static btc_price_cache_t price_cache;
static portMUX_TYPE price_cache_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t price_task_handle;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL || event->user_data == NULL) {
        return ESP_OK;
    }

    http_response_t *response = (http_response_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED || event->event_id == HTTP_EVENT_REDIRECT) {
        response->length = 0;
        response->overflow = false;
        response->data[0] = '\0';
        return ESP_OK;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    const size_t available = sizeof(response->data) - response->length - 1;
    const size_t incoming = (size_t)event->data_len;
    const size_t copy_length = incoming < available ? incoming : available;

    if (copy_length > 0) {
        memcpy(response->data + response->length, event->data, copy_length);
        response->length += copy_length;
        response->data[response->length] = '\0';
    }

    if (copy_length != incoming) {
        response->overflow = true;
    }

    return ESP_OK;
}

static esp_err_t fetch_json(const char *url, char *body, size_t body_size)
{
    if (url == NULL || body == NULL || body_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = BTC_PRICE_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &response,
        .user_agent = "ESP-Miner-BTC-Display/1.0",
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .max_redirection_count = 3,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    esp_err_t result = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "HTTPS request failed: %s", esp_err_to_name(result));
    } else if (status_code != 200) {
        ESP_LOGW(TAG, "Price provider returned HTTP %d", status_code);
        result = ESP_FAIL;
    } else if (response.overflow || response.length == 0) {
        ESP_LOGW(TAG, "Price response was %s", response.overflow ? "too large" : "empty");
        result = response.overflow ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
    } else if (response.length >= body_size) {
        ESP_LOGW(TAG, "Destination buffer is too small for price response");
        result = ESP_ERR_INVALID_SIZE;
    } else {
        memcpy(body, response.data, response.length + 1);
    }

    esp_http_client_cleanup(client);
    return result;
}

static esp_err_t parse_coingecko(const char *json, double *usd, double *change_24h, bool *has_change_24h)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *bitcoin = cJSON_GetObjectItemCaseSensitive(root, "bitcoin");
    cJSON *price = bitcoin ? cJSON_GetObjectItemCaseSensitive(bitcoin, "usd") : NULL;
    cJSON *change = bitcoin ? cJSON_GetObjectItemCaseSensitive(bitcoin, "usd_24h_change") : NULL;

    const bool valid_price =
        price != NULL &&
        cJSON_IsNumber(price) &&
        isfinite(price->valuedouble) &&
        price->valuedouble > 0.0 &&
        price->valuedouble <= BTC_PRICE_MAX_USD;
    const bool valid_change =
        change != NULL &&
        cJSON_IsNumber(change) &&
        isfinite(change->valuedouble);

    if (valid_price) {
        *usd = price->valuedouble;
        *change_24h = valid_change ? change->valuedouble : 0.0;
        *has_change_24h = valid_change;
    }

    cJSON_Delete(root);
    return valid_price ? ESP_OK : ESP_FAIL;
}

static esp_err_t parse_mempool(const char *json, double *usd)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *price = cJSON_GetObjectItemCaseSensitive(root, "USD");
    if (!cJSON_IsNumber(price)) {
        price = cJSON_GetObjectItemCaseSensitive(root, "usd");
    }

    const bool valid_price =
        price != NULL &&
        cJSON_IsNumber(price) &&
        isfinite(price->valuedouble) &&
        price->valuedouble > 0.0 &&
        price->valuedouble <= BTC_PRICE_MAX_USD;
    if (valid_price) {
        *usd = price->valuedouble;
    }

    cJSON_Delete(root);
    return valid_price ? ESP_OK : ESP_FAIL;
}

static esp_err_t fetch_coingecko(double *usd, double *change_24h, bool *has_change_24h)
{
    char body[BTC_PRICE_HTTP_RESPONSE_SIZE];
    esp_err_t result = fetch_json(BTC_PRICE_COINGECKO_URL, body, sizeof(body));
    if (result != ESP_OK) {
        return result;
    }

    result = parse_coingecko(body, usd, change_24h, has_change_24h);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "CoinGecko response did not contain a valid USD price");
    }
    return result;
}

static esp_err_t fetch_mempool(double *usd)
{
    char body[BTC_PRICE_HTTP_RESPONSE_SIZE];
    esp_err_t result = fetch_json(BTC_PRICE_MEMPOOL_URL, body, sizeof(body));
    if (result != ESP_OK) {
        return result;
    }

    result = parse_mempool(body, usd);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "mempool.space response did not contain a valid USD price");
    }
    return result;
}

static void update_cache(double usd, double change_24h, bool has_change_24h, btc_price_source_t source)
{
    portENTER_CRITICAL(&price_cache_mux);
    price_cache.usd = usd;
    price_cache.change_24h = change_24h;
    price_cache.has_change_24h = has_change_24h;
    price_cache.valid = true;
    price_cache.updated_at_us = esp_timer_get_time();
    price_cache.source = source;
    portEXIT_CRITICAL(&price_cache_mux);
}

static void btc_price_task(void *parameter)
{
    (void)parameter;
    uint32_t failure_delay_ms = BTC_PRICE_FAILURE_MIN_MS;

    // Let mining, the web UI, and the display settle before the first TLS handshake.
    vTaskDelay(pdMS_TO_TICKS(BTC_PRICE_INITIAL_DELAY_MS));

    while (true) {
        if (!wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        double usd = 0.0;
        double change_24h = 0.0;
        bool has_change_24h = false;
        bool success = false;

        if (fetch_coingecko(&usd, &change_24h, &has_change_24h) == ESP_OK) {
            update_cache(usd, change_24h, has_change_24h, BTC_PRICE_SOURCE_COINGECKO);
            ESP_LOGI(TAG, "Updated BTC/USD from CoinGecko");
            success = true;
        } else if (wifi_is_connected() && fetch_mempool(&usd) == ESP_OK) {
            update_cache(usd, 0.0, false, BTC_PRICE_SOURCE_MEMPOOL);
            ESP_LOGI(TAG, "Updated BTC/USD from mempool.space fallback");
            success = true;
        }

        if (success) {
            failure_delay_ms = BTC_PRICE_FAILURE_MIN_MS;
            vTaskDelay(pdMS_TO_TICKS(BTC_PRICE_REFRESH_MS));
        } else {
            ESP_LOGW(TAG, "All BTC price providers failed; retaining the last cached value");
            vTaskDelay(pdMS_TO_TICKS(failure_delay_ms));
            if (failure_delay_ms < BTC_PRICE_FAILURE_MAX_MS / 2) {
                failure_delay_ms *= 2;
            } else {
                failure_delay_ms = BTC_PRICE_FAILURE_MAX_MS;
            }
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

    BaseType_t created;
    if (esp_psram_is_initialized()) {
        created = xTaskCreateWithCaps(
            btc_price_task,
            "btc_price",
            BTC_PRICE_TASK_STACK_SIZE,
            global_state,
            BTC_PRICE_TASK_PRIORITY,
            &price_task_handle,
            MALLOC_CAP_SPIRAM
        );
    } else {
        created = xTaskCreate(
            btc_price_task,
            "btc_price",
            BTC_PRICE_TASK_STACK_SIZE,
            global_state,
            BTC_PRICE_TASK_PRIORITY,
            &price_task_handle
        );
    }

    if (created != pdPASS) {
        price_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create BTC price task");
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

    int64_t updated_at_us;
    portENTER_CRITICAL(&price_cache_mux);
    snapshot->usd = price_cache.usd;
    snapshot->change_24h = price_cache.change_24h;
    snapshot->has_change_24h = price_cache.has_change_24h;
    snapshot->valid = price_cache.valid;
    snapshot->source = price_cache.source;
    updated_at_us = price_cache.updated_at_us;
    portEXIT_CRITICAL(&price_cache_mux);

    if (!snapshot->valid || updated_at_us <= 0) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    const uint64_t age_seconds = now_us > updated_at_us ? (uint64_t)(now_us - updated_at_us) / 1000000ULL : 0;
    snapshot->age_seconds = age_seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)age_seconds;
    snapshot->stale = snapshot->age_seconds >= BTC_PRICE_STALE_SECONDS;
}

const char *btc_price_source_name(btc_price_source_t source)
{
    switch (source) {
        case BTC_PRICE_SOURCE_COINGECKO:
            return "CoinGecko";
        case BTC_PRICE_SOURCE_MEMPOOL:
            return "mempool.space";
        default:
            return "--";
    }
}
