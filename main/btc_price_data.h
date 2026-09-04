#ifndef BTC_PRICE_DATA_H_
#define BTC_PRICE_DATA_H_

#include <stdbool.h>
#include <stdint.h>

#define BTC_PRICE_JSON_MAX_BYTES 2048U
#define BTC_PRICE_JSON_MAX_DEPTH 8U
#define BTC_PRICE_STALE_SECONDS 900U
#define BTC_PRICE_MIN_VALID_EPOCH 1704067200LL
#define BTC_PRICE_MAX_VALID_EPOCH 253402300799LL
#define BTC_PRICE_FUTURE_TOLERANCE_SECONDS 120LL
#define BTC_PRICE_MAX_USD 1000000000.0

typedef struct {
    double usd;
    double change_24h;
    bool has_change_24h;
    int64_t provider_timestamp;
    uint32_t age_at_fetch_seconds;
} btc_price_quote_t;

/* Fail closed: no usable timestamp or an old quote is not a new price. */
bool btc_price_parse_coingecko(const char *json, int64_t now_epoch, btc_price_quote_t *quote);
bool btc_price_parse_mempool(const char *json, int64_t now_epoch, btc_price_quote_t *quote);

/* Continue aging using a monotonic clock, including the provider's initial age. */
uint32_t btc_price_age_seconds(uint32_t initial_age, int64_t fetched_at_us, int64_t now_us);
bool btc_price_clock_valid(int64_t now_epoch);

#endif /* BTC_PRICE_DATA_H_ */
