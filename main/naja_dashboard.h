#ifndef NAJA_DASHBOARD_H_
#define NAJA_DASHBOARD_H_

#include "esp_err.h"

typedef struct GlobalState GlobalState;

/**
 * @brief Add the four-page 320x170 status dashboard to the large Bitaxe display.
 *
 * The dashboard is attached to ESP-Miner's existing large stats screen, so
 * provisioning, disconnect, firmware-update, self-test, and overheat screens
 * remain authoritative and unobstructed.
 */
esp_err_t naja_dashboard_start(GlobalState *global_state);

#endif /* NAJA_DASHBOARD_H_ */
