#ifndef SCREEN_H_
#define SCREEN_H_

#include "esp_err.h"
#include "lvgl.h"

typedef struct GlobalState GlobalState;

esp_err_t screen_start(GlobalState * GLOBAL_STATE);
void screen_button_press(void);
/* LVGL context only. Allows optional overlays to target the stats screen exactly. */
lv_obj_t *screen_get_stats_screen(void);

#endif /* SCREEN_H_ */
