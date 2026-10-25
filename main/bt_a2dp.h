#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BT_NAME_MAX 32

typedef enum {
    BT_NO_SPEAKER,      /* nothing saved, waiting for the user to pick one */
    BT_WAITING,         /* saved speaker not reachable, retrying */
    BT_CONNECTING,
    BT_CONNECTED,       /* link up, starting the audio stream */
    BT_STREAMING,
} bt_status_t;

typedef struct {
    uint8_t bda[6];
    char name[BT_NAME_MAX + 1]; /* empty until known */
    int8_t rssi;
} bt_device_t;

/* Bring up Classic BT and reconnect to the saved speaker, if any. */
esp_err_t bt_a2dp_start(void);

bt_status_t bt_a2dp_status(void);
bool bt_a2dp_streaming(void);

/* Name of the saved speaker; false if none. */
bool bt_a2dp_saved_name(char *out, int len);

/* Pairing mode: keep scanning and collect nearby audio devices. */
void bt_a2dp_scan(bool on);
bool bt_a2dp_scanning(void);

/* Copy the devices found so far, strongest signal first. Returns the count. */
int bt_a2dp_devices(bt_device_t *out, int max);

/* Connect to (and remember) a device from the scan list. */
void bt_a2dp_select(const bt_device_t *dev);

#ifdef __cplusplus
}
#endif
