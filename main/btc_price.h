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
    uint32_t age_seconds;         /* Age of the provider's quote, not the request. */
    uint32_t fetched_age_seconds;
    int64_t provider_timestamp;
    btc_price_source_t source;
} btc_price_snapshot_t;

/* Five-minute normal polling; failures back off from 30 seconds to five minutes.
 * Unsynchronized time or unavailable providers never block the display/miner. */
esp_err_t btc_price_start(GlobalState *global_state);
void btc_price_get_snapshot(btc_price_snapshot_t *snapshot);
const char *btc_price_source_name(btc_price_source_t source);

#endif /* BTC_PRICE_H_ */
