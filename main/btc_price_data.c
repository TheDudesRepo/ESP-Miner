#include "btc_price_data.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"

bool btc_price_clock_valid(int64_t now_epoch)
{
    return now_epoch >= BTC_PRICE_MIN_VALID_EPOCH && now_epoch <= BTC_PRICE_MAX_VALID_EPOCH;
}

/* cJSON's general-purpose depth limit is too high for a small worker stack. */
static bool json_is_bounded(const char *json)
{
    bool in_string = false, escaped = false;
    unsigned depth = 0;
    for (size_t i = 0; i < BTC_PRICE_JSON_MAX_BYTES; ++i) {
        unsigned char ch = (unsigned char)json[i];
        if (ch == 0) {
            return !in_string && depth == 0;
        }
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
        } else if (ch == '"') {
            in_string = true;
        } else if (ch == '{' || ch == '[') {
            if (++depth > BTC_PRICE_JSON_MAX_DEPTH) {
                return false;
            }
        } else if (ch == '}' || ch == ']') {
            if (depth == 0) {
                return false;
            }
            --depth;
        }
    }
    return false;
}

static bool parse_quote(const cJSON *price, const cJSON *timestamp, int64_t now_epoch,
                        btc_price_quote_t *quote)
{
    if (!btc_price_clock_valid(now_epoch) || !cJSON_IsNumber(price) || !cJSON_IsNumber(timestamp)) {
        return false;
    }
    const double usd = price->valuedouble;
    const double epoch = timestamp->valuedouble;
    if (!isfinite(usd) || usd <= 0.0 || usd > BTC_PRICE_MAX_USD ||
        !isfinite(epoch) || epoch < BTC_PRICE_MIN_VALID_EPOCH ||
        epoch > BTC_PRICE_MAX_VALID_EPOCH || floor(epoch) != epoch) {
        return false;
    }

    const int64_t provider_epoch = (int64_t)epoch;
    if (provider_epoch > now_epoch && provider_epoch - now_epoch > BTC_PRICE_FUTURE_TOLERANCE_SECONDS) {
        return false;
    }
    const int64_t age = now_epoch > provider_epoch ? now_epoch - provider_epoch : 0;
    if (age >= BTC_PRICE_STALE_SECONDS) {
        return false;
    }

    quote->usd = usd;
    quote->provider_timestamp = provider_epoch;
    quote->age_at_fetch_seconds = (uint32_t)age;
    return true;
}

bool btc_price_parse_coingecko(const char *json, int64_t now_epoch, btc_price_quote_t *quote)
{
    if (quote == NULL) {
        return false;
    }
    memset(quote, 0, sizeof(*quote));
    if (json == NULL || !json_is_bounded(json)) {
        return false;
    }
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    cJSON *bitcoin = cJSON_GetObjectItemCaseSensitive(root, "bitcoin");
    bool valid = cJSON_IsObject(root) && cJSON_IsObject(bitcoin) && parse_quote(
        cJSON_GetObjectItemCaseSensitive(bitcoin, "usd"),
        cJSON_GetObjectItemCaseSensitive(bitcoin, "last_updated_at"), now_epoch, quote);
    if (valid) {
        cJSON *change = cJSON_GetObjectItemCaseSensitive(bitcoin, "usd_24h_change");
        if (cJSON_IsNumber(change) && isfinite(change->valuedouble) && change->valuedouble >= -100.0) {
            quote->change_24h = change->valuedouble;
            quote->has_change_24h = true;
        }
    }
    cJSON_Delete(root);
    return valid;
}

bool btc_price_parse_mempool(const char *json, int64_t now_epoch, btc_price_quote_t *quote)
{
    if (quote == NULL) {
        return false;
    }
    memset(quote, 0, sizeof(*quote));
    if (json == NULL || !json_is_bounded(json)) {
        return false;
    }
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    cJSON *price = cJSON_GetObjectItemCaseSensitive(root, "USD");
    if (!cJSON_IsNumber(price)) {
        price = cJSON_GetObjectItemCaseSensitive(root, "usd");
    }
    bool valid = cJSON_IsObject(root) && parse_quote(
        price, cJSON_GetObjectItemCaseSensitive(root, "time"), now_epoch, quote);
    cJSON_Delete(root);
    return valid;
}

uint32_t btc_price_age_seconds(uint32_t initial_age, int64_t fetched_at_us, int64_t now_us)
{
    /* Both timestamps are nonnegative esp_timer values; guard test/error inputs. */
    uint64_t elapsed = 0;
    if (fetched_at_us >= 0 && now_us > fetched_at_us) {
        elapsed = (uint64_t)(now_us - fetched_at_us) / 1000000ULL;
    }
    const uint64_t age = (uint64_t)initial_age + elapsed;
    return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}
