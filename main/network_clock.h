#ifndef NETWORK_CLOCK_H_
#define NETWORK_CLOCK_H_

#include "esp_err.h"

/* Nonblocking SNTP initialization, independent of the optional LCD dashboard.
 * app_main calls this before launching consumers. The price worker may retry
 * only after startup, so initialization callers are serialized. */
esp_err_t network_clock_start(void);

#endif /* NETWORK_CLOCK_H_ */
