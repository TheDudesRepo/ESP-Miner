#ifndef BTC_PRICE_H_
#define BTC_PRICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct GlobalState GlobalState;

typedef enum {
    BTC_PRICE_SOURCE_NONE = 0,
    BTC_PRICE_SOURCE_COINGECKO,
    BTC_PRICE_SOURCE_MEMPOOL,
} btc_price_source_t;

typedef struct {
    double usd;
    double change_24h;
    bool has_change_24h;
    bool valid;
    bool stale;
    uint32_t age_seconds;
    btc_price_source_t source;
} btc_price_snapshot_t;

/**
 * @brief Start the background Bitcoin-price fetcher.
 *
 * The task waits for Wi-Fi, fetches no more often than every five minutes,
 * and keeps the last successful value when a provider is unavailable.
 */
esp_err_t btc_price_start(GlobalState *global_state);

/**
 * @brief Copy the latest cached price into snapshot.
 */
void btc_price_get_snapshot(btc_price_snapshot_t *snapshot);

/**
 * @brief Return a display-friendly provider name.
 */
const char *btc_price_source_name(btc_price_source_t source);

#endif /* BTC_PRICE_H_ */
