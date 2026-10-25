/*
 * A2DP source. In pairing mode it scans continuously and collects nearby
 * audio devices (requesting full names when the scan reply has none or a
 * shortened one) for the UI to list. Once the user picks one, it connects,
 * remembers it in NVS and reconnects to it on later boots. The stream
 * carries silence while idle so a trigger is heard immediately.
 *
 * Bluedroid callbacks only forward events into a queue; all state lives in
 * one app task, so there are no races between GAP, A2DP, UI and heartbeat.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "bt_a2dp.h"
#include "creeper_audio.h"

static const char *TAG = "bt";

#define LOCAL_NAME         "Creeper"
#define NVS_NS             "creeper"
#define NVS_KEY_PEER       "peer"
#define NVS_KEY_NAME       "name"
#define HEARTBEAT_MS       1000
#define CONNECT_TIMEOUT_S  12
#define RETRY_DELAY_S      3
#define NAME_TIMEOUT_S     8
#define INQUIRY_LEN        5 /* x 1.28 s, short so the list refreshes quickly */
#define MAX_DEVICES        16

typedef enum {
    EVT_HEARTBEAT,
    EVT_DISC_STOPPED,
    EVT_NAME_DONE,
    EVT_A2D,
    EVT_SCAN_ON,
    EVT_SCAN_OFF,
    EVT_SELECT,
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    esp_a2d_cb_event_t a2d_evt;
    esp_a2d_cb_param_t a2d;
    bt_device_t dev;
} app_evt_t;

typedef enum {
    ST_IDLE,            /* no speaker saved, not scanning */
    ST_DISCOVERING,
    ST_NAMING,          /* asking devices for their full names */
    ST_UNCONNECTED,     /* speaker saved, waiting to retry */
    ST_CONNECTING,
    ST_CONNECTED,
} app_state_t;

typedef enum {
    MEDIA_IDLE,
    MEDIA_CHECKING,
    MEDIA_STARTING,
    MEDIA_STARTED,
} media_state_t;

typedef struct {
    bt_device_t dev;
    uint32_t cod;
    bool name_complete;
    bool name_asked;
} scan_entry_t;

static QueueHandle_t s_queue;
static app_state_t s_state = ST_IDLE;
static media_state_t s_media = MEDIA_IDLE;
static int s_ticks;                     /* heartbeats spent in the current state */
static volatile bool s_streaming;
static volatile bool s_scan;            /* pairing mode requested by the UI */

/* The speaker we connect to, saved to NVS once it has connected. */
static esp_bd_addr_t s_peer;
static char s_peer_name[BT_NAME_MAX + 1];
static bool s_have_peer;

/* Scan results, written by the GAP callback and read by the UI. */
static portMUX_TYPE s_scan_lock = portMUX_INITIALIZER_UNLOCKED;
static scan_entry_t s_found[MAX_DEVICES];
static int s_found_count;

static char *bda2str(const uint8_t *bda, char *str)
{
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

static void post(app_evt_type_t type)
{
    app_evt_t e = { .type = type };
    xQueueSend(s_queue, &e, 0);
}

/* ---------- NVS: remember the chosen speaker ---------- */

static void peer_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_peer);
    size_t name_len = sizeof(s_peer_name);

    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        s_have_peer = nvs_get_blob(h, NVS_KEY_PEER, s_peer, &len) == ESP_OK && len == sizeof(s_peer);
        if (nvs_get_str(h, NVS_KEY_NAME, s_peer_name, &name_len) != ESP_OK) {
            s_peer_name[0] = '\0';
        }
        nvs_close(h);
    }
}

static void peer_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_PEER, s_peer, sizeof(s_peer));
        nvs_set_str(h, NVS_KEY_NAME, s_peer_name);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Drop pairings with every device except the current speaker, so an old
 * speaker cannot reconnect to us on its own. */
static void forget_other_bonds(void)
{
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) {
        return;
    }
    esp_bd_addr_t *list = malloc(n * sizeof(esp_bd_addr_t));
    if (list && esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            if (memcmp(list[i], s_peer, ESP_BD_ADDR_LEN) != 0) {
                esp_bt_gap_remove_bond_device(list[i]);
            }
        }
    }
    free(list);
}

/* ---------- Scan results ---------- */

/* Audio class, or class unknown (better to list it than to hide it). */
static bool is_audio_cod(uint32_t cod)
{
    return cod == 0 || !esp_bt_gap_is_valid_cod(cod) ||
           esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV ||
           (esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING);
}

static scan_entry_t *find_entry(const uint8_t *bda, bool create)
{
    for (int i = 0; i < s_found_count; i++) {
        if (memcmp(s_found[i].dev.bda, bda, ESP_BD_ADDR_LEN) == 0) {
            return &s_found[i];
        }
    }
    if (!create || s_found_count >= MAX_DEVICES) {
        return NULL;
    }
    scan_entry_t *e = &s_found[s_found_count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->dev.bda, bda, ESP_BD_ADDR_LEN);
    return e;
}

static void handle_disc_result(esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];
    uint8_t *name = NULL;
    uint8_t name_len = 0;
    bool complete = false;
    uint32_t cod = 0;
    int8_t rssi = -127;

    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_COD:
            cod = *(uint32_t *)p->val;
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            rssi = *(int8_t *)p->val;
            break;
        case ESP_BT_GAP_DEV_PROP_EIR:
            if (!name) {
                name = esp_bt_gap_resolve_eir_data(p->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &name_len);
                complete = name != NULL;
                if (!name) {
                    name = esp_bt_gap_resolve_eir_data(p->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &name_len);
                }
            }
            break;
        case ESP_BT_GAP_DEV_PROP_BDNAME:
            name = p->val;
            name_len = strnlen((char *)p->val, p->len);
            complete = true;
            break;
        default:
            break;
        }
    }

    portENTER_CRITICAL(&s_scan_lock);
    scan_entry_t *e = find_entry(param->disc_res.bda, true);
    if (e) {
        if (cod) {
            e->cod = cod;
        }
        e->dev.rssi = rssi;
        if (name && !e->name_complete) {
            if (name_len > BT_NAME_MAX) {
                name_len = BT_NAME_MAX;
            }
            memcpy(e->dev.name, name, name_len);
            e->dev.name[name_len] = '\0';
            e->name_complete = complete;
        }
    }
    portEXIT_CRITICAL(&s_scan_lock);

    ESP_LOGD(TAG, "seen %s \"%.*s\" cod 0x%" PRIx32, bda2str(param->disc_res.bda, bda_str),
             name ? name_len : 0, name ? (char *)name : "", cod);
}

static void handle_remote_name(esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];

    if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "full name of %s is \"%s\"", bda2str(param->read_rmt_name.bda, bda_str),
                 param->read_rmt_name.rmt_name);
        portENTER_CRITICAL(&s_scan_lock);
        scan_entry_t *e = find_entry(param->read_rmt_name.bda, false);
        if (e) {
            strlcpy(e->dev.name, (char *)param->read_rmt_name.rmt_name, sizeof(e->dev.name));
            e->name_complete = true;
        }
        portEXIT_CRITICAL(&s_scan_lock);
    }
    post(EVT_NAME_DONE);
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        handle_disc_result(param);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            post(EVT_DISC_STOPPED);
        }
        break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        handle_remote_name(param);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "paired with %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "pairing failed, status %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        /* Legacy pairing fallback: most speakers use 0000. */
        esp_bt_pin_code_t pin = { '0', '0', '0', '0' };
        ESP_LOGI(TAG, "PIN requested, replying 0000");
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    }
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        ESP_LOGI(TAG, "link up (status %d)", param->acl_conn_cmpl_stat.stat);
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        ESP_LOGI(TAG, "link down (reason 0x%x)", param->acl_disconn_cmpl_stat.reason);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    default:
        break;
    }
}

/* ---------- AVRCP ----------
 * Not used for anything, but speakers open an AVRCP channel right after
 * connecting and some stall the audio link if it is refused.
 */

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
        ESP_LOGI(TAG, "AVRCP %s", param->conn_stat.connected ? "connected" : "disconnected");
    }
}

static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
}

/* ---------- A2DP ---------- */

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    app_evt_t e = { .type = EVT_A2D, .a2d_evt = event, .a2d = *param };
    xQueueSend(s_queue, &e, 0);
}

static int32_t a2d_data_cb(uint8_t *data, int32_t len)
{
    if (!data || len <= 0) {
        return 0;
    }
    creeper_audio_fill((int16_t *)data, len / 4);
    return len;
}

/* ---------- State machine ---------- */

static void set_state(app_state_t st)
{
    s_state = st;
    s_ticks = 0;
}

/* Where to go when nothing is in progress. */
static void settle(void)
{
    set_state(s_have_peer ? ST_UNCONNECTED : ST_IDLE);
}

static void start_discovery(void)
{
    set_state(ST_DISCOVERING);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, INQUIRY_LEN, 0);
}

/* Ask the next audio device with a missing or shortened name for its full
 * name. Returns false when there is nothing left to ask. */
static bool request_next_name(void)
{
    char bda_str[18];
    esp_bd_addr_t bda;
    bool found = false;

    portENTER_CRITICAL(&s_scan_lock);
    for (int i = 0; i < s_found_count; i++) {
        scan_entry_t *e = &s_found[i];
        if (!e->name_complete && !e->name_asked && is_audio_cod(e->cod)) {
            e->name_asked = true;
            memcpy(bda, e->dev.bda, ESP_BD_ADDR_LEN);
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_scan_lock);

    if (!found) {
        return false;
    }
    ESP_LOGI(TAG, "asking %s for its full name...", bda2str(bda, bda_str));
    set_state(ST_NAMING);
    esp_bt_gap_read_remote_name(bda);
    return true;
}

static void connect_peer(void)
{
    char bda_str[18];
    ESP_LOGI(TAG, "connecting to %s \"%s\"...", bda2str(s_peer, bda_str), s_peer_name);
    set_state(ST_CONNECTING);
    esp_a2d_source_connect(s_peer);
}

static void on_connected(const uint8_t *bda)
{
    char bda_str[18];

    if (!s_have_peer || memcmp(bda, s_peer, ESP_BD_ADDR_LEN) != 0) {
        /* An old speaker reconnected on its own; we only want the chosen one. */
        ESP_LOGW(TAG, "rejecting connection from %s", bda2str(bda, bda_str));
        esp_a2d_source_disconnect((uint8_t *)bda);
        return;
    }
    ESP_LOGI(TAG, "connected to %s", bda2str(bda, bda_str));
    peer_save();
    forget_other_bonds();
    s_media = MEDIA_IDLE;
    set_state(ST_CONNECTED);
}

static void on_disconnected(void)
{
    s_streaming = false;
    s_media = MEDIA_IDLE;
    settle();
}

static void media_step(app_evt_t *e)
{
    if (e->type == EVT_HEARTBEAT && s_media == MEDIA_IDLE) {
        s_media = MEDIA_CHECKING;
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        return;
    }
    if (e->type != EVT_A2D || e->a2d_evt != ESP_A2D_MEDIA_CTRL_ACK_EVT) {
        return;
    }

    bool ok = e->a2d.media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS;
    switch (e->a2d.media_ctrl_stat.cmd) {
    case ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY:
        if (ok) {
            s_media = MEDIA_STARTING;
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else {
            s_media = MEDIA_IDLE; /* retry on next heartbeat */
        }
        break;
    case ESP_A2D_MEDIA_CTRL_START:
        if (ok) {
            ESP_LOGI(TAG, "streaming");
            s_media = MEDIA_STARTED;
        } else {
            ESP_LOGW(TAG, "media start failed, retrying");
            s_media = MEDIA_IDLE;
        }
        break;
    default:
        break;
    }
}

static void handle_a2d(app_evt_t *e)
{
    switch (e->a2d_evt) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        esp_a2d_connection_state_t cs = e->a2d.conn_stat.state;
        if (cs == ESP_A2D_CONNECTION_STATE_CONNECTED && s_state != ST_CONNECTED) {
            on_connected(e->a2d.conn_stat.remote_bda);
        } else if (cs == ESP_A2D_CONNECTION_STATE_DISCONNECTED &&
                   memcmp(e->a2d.conn_stat.remote_bda, s_peer, ESP_BD_ADDR_LEN) == 0 &&
                   (s_state == ST_CONNECTED || s_state == ST_CONNECTING)) {
            ESP_LOGW(TAG, "%s", s_state == ST_CONNECTING ? "connect failed" : "disconnected");
            on_disconnected();
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
        s_streaming = e->a2d.audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        if (!s_streaming && s_state == ST_CONNECTED && s_media == MEDIA_STARTED) {
            /* Speaker suspended the stream; start it again. */
            s_media = MEDIA_IDLE;
        }
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (s_state == ST_CONNECTED) {
            media_step(e);
        }
        break;
    default:
        break;
    }
}

static void handle_heartbeat(app_evt_t *e)
{
    s_ticks++;
    switch (s_state) {
    case ST_IDLE:
        if (s_scan) {
            start_discovery();
        }
        break;
    case ST_UNCONNECTED:
        if (s_scan) {
            start_discovery(); /* pairing mode takes priority over reconnecting */
        } else if (s_ticks >= RETRY_DELAY_S) {
            connect_peer();
        }
        break;
    case ST_NAMING:
        if (s_ticks >= NAME_TIMEOUT_S && !request_next_name()) {
            settle();
        }
        break;
    case ST_CONNECTING:
        if (s_ticks >= CONNECT_TIMEOUT_S) {
            ESP_LOGW(TAG, "connect timeout");
            esp_a2d_source_disconnect(s_peer);
            settle();
        }
        break;
    case ST_CONNECTED:
        media_step(e);
        break;
    default:
        break;
    }
}

static void handle_select(const bt_device_t *dev)
{
    char bda_str[18];
    bool same = s_have_peer && memcmp(dev->bda, s_peer, ESP_BD_ADDR_LEN) == 0;

    s_scan = false;
    if (s_state == ST_DISCOVERING) {
        esp_bt_gap_cancel_discovery();
    }
    if (same && s_state == ST_CONNECTED) {
        return;
    }
    if (s_state == ST_CONNECTED || s_state == ST_CONNECTING) {
        ESP_LOGI(TAG, "dropping %s for the new speaker", bda2str(s_peer, bda_str));
        esp_a2d_source_disconnect(s_peer);
        s_streaming = false;
    }

    memcpy(s_peer, dev->bda, ESP_BD_ADDR_LEN);
    strlcpy(s_peer_name, dev->name, sizeof(s_peer_name));
    s_have_peer = true;
    connect_peer();
}

static void app_task(void *arg)
{
    app_evt_t e;

    if (s_have_peer) {
        connect_peer();
    }

    for (;;) {
        if (!xQueueReceive(s_queue, &e, portMAX_DELAY)) {
            continue;
        }
        switch (e.type) {
        case EVT_HEARTBEAT:
            handle_heartbeat(&e);
            break;
        case EVT_DISC_STOPPED:
            if (s_state == ST_DISCOVERING && !request_next_name()) {
                settle(); /* rescans on the next heartbeat while s_scan */
            }
            break;
        case EVT_NAME_DONE:
            if (s_state == ST_NAMING && !request_next_name()) {
                settle();
            }
            break;
        case EVT_A2D:
            handle_a2d(&e);
            break;
        case EVT_SCAN_ON:
            ESP_LOGI(TAG, "pairing mode on");
            portENTER_CRITICAL(&s_scan_lock);
            s_found_count = 0;
            portEXIT_CRITICAL(&s_scan_lock);
            if (s_state == ST_IDLE || s_state == ST_UNCONNECTED) {
                start_discovery();
            } else if (s_state == ST_CONNECTING) {
                esp_a2d_source_disconnect(s_peer); /* scan instead of paging */
                start_discovery();
            }
            /* While connected, scan alongside the stream. */
            else if (s_state == ST_CONNECTED) {
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, INQUIRY_LEN, 0);
            }
            break;
        case EVT_SCAN_OFF:
            ESP_LOGI(TAG, "pairing mode off");
            if (s_state == ST_DISCOVERING) {
                esp_bt_gap_cancel_discovery();
                settle();
            } else if (s_state == ST_NAMING) {
                esp_bt_gap_cancel_discovery();
                settle();
            }
            break;
        case EVT_SELECT:
            handle_select(&e.dev);
            break;
        }

        /* While connected and in pairing mode, keep the list refreshing. */
        if (e.type == EVT_DISC_STOPPED && s_state == ST_CONNECTED && s_scan) {
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, INQUIRY_LEN, 0);
        }
    }
}

static void heartbeat_cb(void *arg)
{
    post(EVT_HEARTBEAT);
}

/* ---------- Public API ---------- */

bt_status_t bt_a2dp_status(void)
{
    switch (s_state) {
    case ST_CONNECTED:
        return s_streaming ? BT_STREAMING : BT_CONNECTED;
    case ST_CONNECTING:
        return BT_CONNECTING;
    default:
        return s_have_peer ? BT_WAITING : BT_NO_SPEAKER;
    }
}

bool bt_a2dp_streaming(void)
{
    return s_streaming;
}

bool bt_a2dp_saved_name(char *out, int len)
{
    if (!s_have_peer) {
        return false;
    }
    strlcpy(out, s_peer_name[0] ? s_peer_name : "speaker", len);
    return true;
}

void bt_a2dp_scan(bool on)
{
    if (s_scan == on) {
        return;
    }
    s_scan = on;
    post(on ? EVT_SCAN_ON : EVT_SCAN_OFF);
}

bool bt_a2dp_scanning(void)
{
    return s_scan;
}

int bt_a2dp_devices(bt_device_t *out, int max)
{
    int n = 0;

    portENTER_CRITICAL(&s_scan_lock);
    for (int i = 0; i < s_found_count && n < max; i++) {
        if (is_audio_cod(s_found[i].cod)) {
            out[n++] = s_found[i].dev;
        }
    }
    portEXIT_CRITICAL(&s_scan_lock);

    /* Strongest signal (nearest device) first. */
    for (int i = 1; i < n; i++) {
        bt_device_t d = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < d.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = d;
    }
    return n;
}

void bt_a2dp_select(const bt_device_t *dev)
{
    app_evt_t e = { .type = EVT_SELECT, .dev = *dev };
    xQueueSend(s_queue, &e, portMAX_DELAY);
}

esp_err_t bt_a2dp_start(void)
{
    char bda_str[18];

    s_queue = xQueueCreate(16, sizeof(app_evt_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* SSP "just works": we have no keyboard. */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_pin_code_t pin = { 0 };
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin);

    esp_bt_gap_set_device_name(LOCAL_NAME);
    esp_bt_gap_register_callback(gap_cb);
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(avrc_ct_cb));
    ESP_ERROR_CHECK(esp_avrc_tg_init());
    ESP_ERROR_CHECK(esp_avrc_tg_register_callback(avrc_tg_cb));
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2d_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(a2d_data_cb));

    /* Connectable so the saved speaker can reconnect to us, but not discoverable. */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    ESP_LOGI(TAG, "own address %s", bda2str(esp_bt_dev_get_address(), bda_str));

    peer_load();
    if (s_have_peer) {
        ESP_LOGI(TAG, "saved speaker %s \"%s\"", bda2str(s_peer, bda_str), s_peer_name);
    }

    xTaskCreatePinnedToCore(app_task, "bt_app", 4096, NULL, 5, NULL, 0);

    const esp_timer_create_args_t targs = { .callback = heartbeat_cb, .name = "bt_hb" };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, HEARTBEAT_MS * 1000));
    return ESP_OK;
}
