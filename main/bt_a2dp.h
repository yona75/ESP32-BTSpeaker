#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Bring up Classic BT, then find/connect the configured A2DP sink and stream. */
esp_err_t bt_a2dp_start(void);

/* True while audio is streaming to the sink. */
bool bt_a2dp_streaming(void);
