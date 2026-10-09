/*
 * A2DP source: finds the sink by name (or reconnects to the last one stored
 * in NVS), connects, and keeps the media stream running. The stream carries
 * silence while idle so a button press is heard immediately.
 *
 * Bluedroid callbacks only forward events into a queue; all state lives in
 * one app task, so there are no races between GAP, A2DP and the heartbeat.
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
#include "sdkconfig.h"

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
#define MAX_DIRECT_RETRIES 3
#define INQUIRY_LEN        10 /* x 1.28 s */
#define MAX_CANDIDATES     8
#define NAME_TIMEOUT_S     8

typedef enum {
    EVT_HEARTBEAT,
    EVT_FOUND,          /* discovery matched the target, peer address in s_peer */
    EVT_DISC_STOPPED,
    EVT_NAME_DONE,      /* remote name request finished without a match */
    EVT_A2D,
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    esp_a2d_cb_event_t a2d_evt;
    esp_a2d_cb_param_t a2d;
} app_evt_t;

typedef enum {
    ST_IDLE,
    ST_DISCOVERING,
    ST_NAMING,          /* asking candidates for their full name */
    ST_UNCONNECTED,
    ST_CONNECTING,
    ST_CONNECTED,
} app_state_t;

typedef enum {
    MEDIA_IDLE,
    MEDIA_CHECKING,
    MEDIA_STARTING,
    MEDIA_STARTED,
} media_state_t;

static QueueHandle_t s_queue;
static app_state_t s_state = ST_IDLE;
static media_state_t s_media = MEDIA_IDLE;
static esp_bd_addr_t s_peer;
static bool s_have_peer;
static bool s_found;            /* set by GAP callback when target is seen */
static int s_ticks;             /* heartbeats spent in the current state */
static int s_failures;          /* consecutive failed direct connects */
static volatile bool s_streaming;

/* Devices whose scan reply had no name or a shortened one that could be the
 * target. Their full name is requested once the scan ends. */
static esp_bd_addr_t s_cand[MAX_CANDIDATES];
static int s_cand_count;
static int s_cand_next;

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

/* ---------- NVS: remember the last connected sink ---------- */

/* Drop all pairings so a previously used sink cannot reconnect to us. */
static void forget_bonds(void)
{
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) {
        return;
    }
    esp_bd_addr_t *list = malloc(n * sizeof(esp_bd_addr_t));
    if (list && esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            esp_bt_gap_remove_bond_device(list[i]);
        }
    }
    free(list);
}

static void peer_load(void)
{
    nvs_handle_t h;
    char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = "";
    size_t len = sizeof(s_peer);
    size_t name_len = sizeof(name);

    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        s_have_peer = nvs_get_blob(h, NVS_KEY_PEER, s_peer, &len) == ESP_OK && len == sizeof(s_peer);
        nvs_get_str(h, NVS_KEY_NAME, name, &name_len);
        nvs_close(h);
    }

    /* The stored sink belongs to a different configured name: start over. */
    if (strcmp(name, CONFIG_CREEPER_SINK_NAME) != 0) {
        if (s_have_peer || name[0]) {
            ESP_LOGI(TAG, "sink name changed from \"%s\", forgetting old sink", name);
        }
        s_have_peer = false;
        forget_bonds();
    }
}

static void peer_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_PEER, s_peer, sizeof(s_peer));
        nvs_set_str(h, NVS_KEY_NAME, CONFIG_CREEPER_SINK_NAME);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------- GAP ---------- */

static bool name_matches(const uint8_t *name, uint8_t len)
{
    size_t want = strlen(CONFIG_CREEPER_SINK_NAME);
    return len >= want && memcmp(name, CONFIG_CREEPER_SINK_NAME, want) == 0;
}

/* A shortened name such as "Mobile sp" that the target name starts with. */
static bool name_is_prefix(const uint8_t *name, uint8_t len)
{
    return len > 0 && len < strlen(CONFIG_CREEPER_SINK_NAME) &&
           memcmp(name, CONFIG_CREEPER_SINK_NAME, len) == 0;
}

static void add_candidate(const uint8_t *bda)
{
    for (int i = 0; i < s_cand_count; i++) {
        if (memcmp(s_cand[i], bda, ESP_BD_ADDR_LEN) == 0) {
            return;
        }
    }
    if (s_cand_count < MAX_CANDIDATES) {
        memcpy(s_cand[s_cand_count++], bda, ESP_BD_ADDR_LEN);
    }
}

static void target_found(const uint8_t *bda)
{
    ESP_LOGI(TAG, "target found");
    memcpy(s_peer, bda, ESP_BD_ADDR_LEN);
    s_have_peer = true;
    s_found = true;
}

static void handle_disc_result(esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];
    uint8_t *name = NULL;
    uint8_t name_len = 0;
    uint32_t cod = 0;

    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_COD) {
            cod = *(uint32_t *)p->val;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR && !name) {
            name = esp_bt_gap_resolve_eir_data(p->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &name_len);
            if (!name) {
                name = esp_bt_gap_resolve_eir_data(p->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &name_len);
            }
        } else if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
            name = p->val;
            name_len = strnlen((char *)p->val, p->len);
        }
    }

    bool is_av = esp_bt_gap_is_valid_cod(cod) && esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV;

    if (!name) {
        if (is_av) {
            ESP_LOGI(TAG, "seen %s (audio device, no name)", bda2str(param->disc_res.bda, bda_str));
            add_candidate(param->disc_res.bda);
        }
        return;
    }
    ESP_LOGI(TAG, "seen %s \"%.*s\"", bda2str(param->disc_res.bda, bda_str), name_len, name);

    if (s_found) {
        return;
    }
    if (name_matches(name, name_len)) {
        target_found(param->disc_res.bda);
        esp_bt_gap_cancel_discovery();
    } else if (name_is_prefix(name, name_len)) {
        add_candidate(param->disc_res.bda);
    }
}

static void handle_remote_name(esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];
    const uint8_t *name = param->read_rmt_name.rmt_name;

    if (param->read_rmt_name.stat != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "%s: name request failed (%d)", bda2str(param->read_rmt_name.bda, bda_str),
                 param->read_rmt_name.stat);
        post(EVT_NAME_DONE);
        return;
    }
    ESP_LOGI(TAG, "full name of %s is \"%s\"", bda2str(param->read_rmt_name.bda, bda_str), name);
    if (name_matches(name, strlen((const char *)name))) {
        target_found(param->read_rmt_name.bda);
        post(EVT_FOUND);
    } else {
        post(EVT_NAME_DONE);
    }
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        handle_disc_result(param);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            post(s_found ? EVT_FOUND : EVT_DISC_STOPPED);
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
        /* Legacy pairing fallback: most headsets use 0000. */
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
 * Not used for anything, but headsets open an AVRCP channel right after
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

static void start_discovery(void)
{
    ESP_LOGI(TAG, "searching for \"%s\" (put it in pairing mode)...", CONFIG_CREEPER_SINK_NAME);
    s_found = false;
    s_cand_count = 0;
    set_state(ST_DISCOVERING);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, INQUIRY_LEN, 0);
}

/* Ask the next candidate for its full name; false when none are left. */
static bool request_next_name(void)
{
    char bda_str[18];

    if (s_cand_next >= s_cand_count) {
        return false;
    }
    ESP_LOGI(TAG, "asking %s for its full name...", bda2str(s_cand[s_cand_next], bda_str));
    set_state(ST_NAMING);
    esp_bt_gap_read_remote_name(s_cand[s_cand_next++]);
    return true;
}

/* Scan and name lookups found nothing: retry. */
static void search_failed(void)
{
    if (s_have_peer) {
        /* Alternate: one direct attempt to the known sink, then scan again. */
        s_failures = MAX_DIRECT_RETRIES - 1;
        set_state(ST_UNCONNECTED);
    } else {
        ESP_LOGI(TAG, "not found yet, scanning again...");
        start_discovery();
    }
}

static void connect_peer(void)
{
    char bda_str[18];
    ESP_LOGI(TAG, "connecting to %s...", bda2str(s_peer, bda_str));
    set_state(ST_CONNECTING);
    esp_a2d_source_connect(s_peer);
}

static void on_connected(const uint8_t *bda)
{
    char bda_str[18];
    ESP_LOGI(TAG, "connected to %s", bda2str(bda, bda_str));
    memcpy(s_peer, bda, ESP_BD_ADDR_LEN);
    s_have_peer = true;
    peer_save();
    s_failures = 0;
    s_media = MEDIA_IDLE;
    set_state(ST_CONNECTED);
}

static void on_disconnected(void)
{
    s_streaming = false;
    s_media = MEDIA_IDLE;
    set_state(ST_UNCONNECTED);
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
            ESP_LOGI(TAG, "streaming - press BOOT to summon the Creeper");
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
            if (s_state == ST_DISCOVERING) {
                esp_bt_gap_cancel_discovery(); /* sink reconnected to us on its own */
            }
            on_connected(e->a2d.conn_stat.remote_bda);
        } else if (cs == ESP_A2D_CONNECTION_STATE_DISCONNECTED &&
                   (s_state == ST_CONNECTED || s_state == ST_CONNECTING)) {
            if (s_state == ST_CONNECTING) {
                s_failures++;
                ESP_LOGW(TAG, "connect failed (%d)", s_failures);
            } else {
                ESP_LOGW(TAG, "disconnected");
            }
            on_disconnected();
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
        s_streaming = e->a2d.audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        if (!s_streaming && s_state == ST_CONNECTED && s_media == MEDIA_STARTED) {
            /* Sink suspended the stream; start it again. */
            s_media = MEDIA_IDLE;
        }
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (s_state == ST_CONNECTED) {
            media_step(e);
        }
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
        ESP_LOGI(TAG, "sink reports delay %u.%u ms",
                 e->a2d.a2d_report_delay_value_stat.delay_value / 10,
                 e->a2d.a2d_report_delay_value_stat.delay_value % 10);
        break;
    default:
        break;
    }
}

static void handle_heartbeat(app_evt_t *e)
{
    s_ticks++;
    switch (s_state) {
    case ST_UNCONNECTED:
        if (s_ticks < RETRY_DELAY_S) {
            break;
        }
        if (s_have_peer && s_failures < MAX_DIRECT_RETRIES) {
            connect_peer();
        } else {
            s_failures = 0;
            start_discovery();
        }
        break;
    case ST_CONNECTING:
        if (s_ticks >= CONNECT_TIMEOUT_S) {
            s_failures++;
            ESP_LOGW(TAG, "connect timeout (%d)", s_failures);
            esp_a2d_source_disconnect(s_peer);
            set_state(ST_UNCONNECTED);
        }
        break;
    case ST_NAMING:
        if (s_ticks >= NAME_TIMEOUT_S && !request_next_name()) {
            search_failed();
        }
        break;
    case ST_CONNECTED:
        media_step(e);
        break;
    default:
        break;
    }
}

static void app_task(void *arg)
{
    app_evt_t e;

    if (s_have_peer) {
        connect_peer();
    } else {
        start_discovery();
    }

    for (;;) {
        if (!xQueueReceive(s_queue, &e, portMAX_DELAY)) {
            continue;
        }
        switch (e.type) {
        case EVT_HEARTBEAT:
            handle_heartbeat(&e);
            break;
        case EVT_FOUND:
            if (s_state == ST_DISCOVERING || s_state == ST_NAMING) {
                s_failures = 0;
                connect_peer();
            }
            break;
        case EVT_DISC_STOPPED:
            if (s_state == ST_DISCOVERING) {
                s_cand_next = 0;
                if (!request_next_name()) {
                    search_failed();
                }
            }
            break;
        case EVT_NAME_DONE:
            if (s_state == ST_NAMING && !request_next_name()) {
                search_failed();
            }
            break;
        case EVT_A2D:
            handle_a2d(&e);
            break;
        }
    }
}

static void heartbeat_cb(void *arg)
{
    post(EVT_HEARTBEAT);
}

bool bt_a2dp_streaming(void)
{
    return s_streaming;
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

    /* SSP "just works": we have no display or keyboard. */
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

    /* Connectable so a bonded sink can reconnect to us, but not discoverable. */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    ESP_LOGI(TAG, "own address %s", bda2str(esp_bt_dev_get_address(), bda_str));

    peer_load();
    if (s_have_peer) {
        ESP_LOGI(TAG, "last sink %s", bda2str(s_peer, bda_str));
    }

    xTaskCreatePinnedToCore(app_task, "bt_app", 4096, NULL, 5, NULL, 0);

    const esp_timer_create_args_t targs = { .callback = heartbeat_cb, .name = "bt_hb" };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, HEARTBEAT_MS * 1000));
    return ESP_OK;
}
