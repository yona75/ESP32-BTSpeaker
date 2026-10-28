/*
 * M5Stack Core2 UI. The screen is only for choosing a speaker; once audio is
 * streaming it goes dark (backlight off, panel asleep) to save battery.
 *
 *   dark screen, tap        -> Creeper hiss + explosion
 *   any screen, hold 2 s    -> speaker list (pairing mode)
 *   list, tap a speaker     -> connect and remember it
 */
#include <M5Unified.h>

#include "esp_log.h"

#include "bt_a2dp.h"
#include "creeper_audio.h"
#include "ui.h"

static const char *TAG = "ui";

#define FRAME_MS          30
#define HOLD_MS           2000
#define STATUS_TIMEOUT_MS 60000 /* status screen goes dark on its own */
#define LIST_TIMEOUT_MS   120000
#define CONNECTED_SHOW_MS 1500
#define BRIGHTNESS        120

#define ROW_Y0   44
#define ROW_H    38
#define MAX_ROWS 5
#define CANCEL_Y (ROW_Y0 + MAX_ROWS * ROW_H + 2)

#define COL_BG     TFT_BLACK
#define COL_FG     TFT_WHITE
#define COL_GREEN  0x3DE7 /* Creeper green */
#define COL_DIM    TFT_DARKGREY

#define BATTERY_POLL_MS 5000
#define BATTERY_LOW     20

enum ui_mode_t { MODE_STATUS, MODE_LIST, MODE_DARK };

static ui_mode_t s_mode;
static uint32_t s_mode_since;
static uint32_t s_last_touch;
static uint32_t s_streaming_since;
static bool s_dirty = true;

static int s_batt_level = -1;     /* percent, -1 = no reading / no battery */
static bool s_batt_charging;
static uint32_t s_batt_polled;

static bt_device_t s_rows[MAX_ROWS];
static int s_row_count;
static uint32_t s_list_sig;

static void power_save_setup(void)
{
    /* No LEDs, no vibration, no internal speaker, no 5 V on the side ports. */
    M5.Power.setLed(0);
    M5.Power.setVibration(0);
    M5.Power.setExtOutput(false);
    M5.Speaker.end();
    M5.Mic.end();
}

static void screen_on(void)
{
    M5.Display.wakeup();
    M5.Display.setBrightness(BRIGHTNESS);
}

static void screen_off(void)
{
    M5.Display.fillScreen(COL_BG);
    M5.Display.setBrightness(0);
    M5.Display.sleep();
}

static void set_mode(ui_mode_t m)
{
    if (m == MODE_DARK && s_mode != MODE_DARK) {
        screen_off();
    } else if (m != MODE_DARK && s_mode == MODE_DARK) {
        screen_on();
    }
    if (m == MODE_LIST) {
        s_list_sig = 0;
        bt_a2dp_scan(true);
    } else if (s_mode == MODE_LIST) {
        bt_a2dp_scan(false);
    }
    ESP_LOGI(TAG, "mode %d -> %d", s_mode, m);
    s_mode = m;
    s_mode_since = lgfx::millis();
    s_dirty = true;
}

/* Battery icon with percentage in the top-right corner of the header. */
static void draw_battery(void)
{
    const int x = 282, y = 10, w = 28, h = 16;
    char pct[8];

    M5.Display.fillRect(228, 0, 92, 36, COL_GREEN);
    if (s_batt_level < 0) {
        return;
    }

    uint16_t fill = s_batt_level < BATTERY_LOW && !s_batt_charging ? TFT_RED : TFT_BLACK;
    M5.Display.drawRect(x, y, w, h, TFT_BLACK);
    M5.Display.fillRect(x + w, y + 4, 3, h - 8, TFT_BLACK);
    M5.Display.fillRect(x + 2, y + 2, (w - 4) * s_batt_level / 100, h - 4, fill);

    if (s_batt_charging) {
        /* Lightning bolt, outlined so it shows on both full and empty. */
        const int cx = x + w / 2, cy = y + h / 2;
        M5.Display.fillTriangle(cx + 2, cy - 7, cx - 5, cy + 1, cx, cy + 1, COL_GREEN);
        M5.Display.fillTriangle(cx - 2, cy + 7, cx + 5, cy - 1, cx, cy - 1, COL_GREEN);
        M5.Display.drawTriangle(cx + 2, cy - 7, cx - 5, cy + 1, cx, cy + 1, TFT_BLACK);
        M5.Display.drawTriangle(cx - 2, cy + 7, cx + 5, cy - 1, cx, cy - 1, TFT_BLACK);
    }

    snprintf(pct, sizeof(pct), "%d%%", s_batt_level);
    M5.Display.setFont(&fonts::FreeSansBold9pt7b);
    M5.Display.setTextColor(fill, COL_GREEN);
    M5.Display.setTextDatum(middle_right);
    M5.Display.drawString(pct, x - 5, 18);
}

/* Read the battery every few seconds; true if what we show has changed. */
static bool poll_battery(bool force)
{
    uint32_t now = lgfx::millis();

    if (!force && now - s_batt_polled < BATTERY_POLL_MS) {
        return false;
    }
    s_batt_polled = now;

    int level = M5.Power.getBatteryLevel();
    bool charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
    if (level > 100) {
        level = 100;
    }
    if (level == s_batt_level && charging == s_batt_charging) {
        return false;
    }
    s_batt_level = level;
    s_batt_charging = charging;
    return true;
}

static void draw_header(const char *title)
{
    M5.Display.fillScreen(COL_BG);
    M5.Display.fillRect(0, 0, 320, 36, COL_GREEN);
    M5.Display.setTextColor(TFT_BLACK, COL_GREEN);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextDatum(middle_left);
    M5.Display.drawString(title, 10, 18);
    draw_battery();
}

static void draw_status(void)
{
    char name[BT_NAME_MAX + 1];
    char line[64];
    bool saved = bt_a2dp_saved_name(name, sizeof(name));

    draw_header("Creeper");
    M5.Display.setTextColor(COL_FG, COL_BG);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.setTextDatum(top_left);

    switch (bt_a2dp_status()) {
    case BT_NO_SPEAKER:
        M5.Display.drawString("No speaker chosen", 10, 60);
        break;
    case BT_WAITING:
        M5.Display.drawString("Waiting for", 10, 60);
        snprintf(line, sizeof(line), "%s...", saved ? name : "speaker");
        M5.Display.drawString(line, 10, 90);
        break;
    case BT_CONNECTING:
    case BT_CONNECTED:
        M5.Display.drawString("Connecting to", 10, 60);
        snprintf(line, sizeof(line), "%s...", saved ? name : "speaker");
        M5.Display.drawString(line, 10, 90);
        break;
    case BT_STREAMING:
        M5.Display.setTextColor(COL_GREEN, COL_BG);
        M5.Display.drawString("Connected!", 10, 60);
        M5.Display.setTextColor(COL_FG, COL_BG);
        M5.Display.drawString("Tap the dark screen", 10, 100);
        M5.Display.drawString("to explode.", 10, 128);
        break;
    }

    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(COL_DIM, COL_BG);
    M5.Display.drawString("Hold the screen 2 s to choose a speaker", 10, 210);
}

static uint32_t list_signature(const bt_device_t *rows, int n)
{
    uint32_t h = n;
    for (int i = 0; i < n; i++) {
        h = h * 31 + rows[i].bda[5] + rows[i].bda[4] * 7;
        for (const char *c = rows[i].name; *c; c++) {
            h = h * 31 + *c;
        }
    }
    return h;
}

static void draw_list(void)
{
    char label[48];

    draw_header("Choose a speaker");
    M5.Display.setTextDatum(middle_left);

    if (s_row_count == 0) {
        M5.Display.setFont(&fonts::FreeSans12pt7b);
        M5.Display.setTextColor(COL_FG, COL_BG);
        M5.Display.drawString("Searching...", 10, ROW_Y0 + 30);
        M5.Display.setFont(&fonts::FreeSans9pt7b);
        M5.Display.setTextColor(COL_DIM, COL_BG);
        M5.Display.drawString("Put your speaker in pairing mode", 10, ROW_Y0 + 70);
    }

    for (int i = 0; i < s_row_count; i++) {
        const bt_device_t *d = &s_rows[i];
        int y = ROW_Y0 + i * ROW_H;
        if (d->name[0]) {
            snprintf(label, sizeof(label), "%s", d->name);
        } else {
            snprintf(label, sizeof(label), "%02X:%02X:%02X:%02X:%02X:%02X",
                     d->bda[0], d->bda[1], d->bda[2], d->bda[3], d->bda[4], d->bda[5]);
        }
        M5.Display.drawRoundRect(4, y + 2, 312, ROW_H - 4, 6, COL_GREEN);
        M5.Display.setFont(&fonts::FreeSans12pt7b);
        M5.Display.setTextColor(d->name[0] ? COL_FG : COL_DIM, COL_BG);
        M5.Display.drawString(label, 14, y + ROW_H / 2);
    }

    if (bt_a2dp_saved_name(label, sizeof(label))) {
        M5.Display.setFont(&fonts::FreeSans9pt7b);
        M5.Display.setTextColor(COL_DIM, COL_BG);
        M5.Display.setTextDatum(middle_center);
        M5.Display.drawString("Cancel", 160, CANCEL_Y + 8);
    }
}

static void refresh_list(void)
{
    bt_device_t all[16];
    int n = bt_a2dp_devices(all, 16);
    s_row_count = n < MAX_ROWS ? n : MAX_ROWS;
    memcpy(s_rows, all, s_row_count * sizeof(bt_device_t));

    uint32_t sig = list_signature(s_rows, s_row_count);
    if (sig != s_list_sig) {
        s_list_sig = sig;
        s_dirty = true;
    }
}

static void list_tap(int x, int y)
{
    char name[BT_NAME_MAX + 1];

    if (y >= ROW_Y0 && y < ROW_Y0 + s_row_count * ROW_H) {
        const bt_device_t *d = &s_rows[(y - ROW_Y0) / ROW_H];
        ESP_LOGI(TAG, "picked \"%s\"", d->name);
        bt_a2dp_select(d);
        set_mode(MODE_STATUS);
    } else if (y >= CANCEL_Y && bt_a2dp_saved_name(name, sizeof(name))) {
        set_mode(MODE_STATUS);
    }
}

static void handle_touch(void)
{
    auto t = M5.Touch.getDetail();

    if (t.wasPressed() || t.isPressed()) {
        s_last_touch = lgfx::millis();
    }

    if (t.wasHold()) {
        set_mode(MODE_LIST);
        return;
    }
    if (!t.wasClicked()) {
        return;
    }

    switch (s_mode) {
    case MODE_DARK:
        if (bt_a2dp_streaming()) {
            if (!creeper_trigger()) {
                ESP_LOGI(TAG, "already exploding");
            }
        } else {
            set_mode(MODE_STATUS); /* show why nothing happens */
        }
        break;
    case MODE_LIST:
        list_tap(t.x, t.y);
        break;
    case MODE_STATUS:
        if (bt_a2dp_streaming()) {
            set_mode(MODE_DARK);
        }
        break;
    }
}

static void update(void)
{
    static bt_status_t last_status = BT_NO_SPEAKER;
    bt_status_t st = bt_a2dp_status();
    uint32_t now = lgfx::millis();

    if (st == BT_STREAMING && last_status != BT_STREAMING) {
        s_streaming_since = now;
    }
    if (st != last_status && s_mode == MODE_STATUS) {
        s_dirty = true;
    }
    last_status = st;

    switch (s_mode) {
    case MODE_STATUS:
        if (st == BT_STREAMING && now - s_streaming_since > CONNECTED_SHOW_MS &&
            now - s_last_touch > CONNECTED_SHOW_MS) {
            set_mode(MODE_DARK);
        } else if (st == BT_NO_SPEAKER) {
            set_mode(MODE_LIST);
        } else if (now - s_last_touch > STATUS_TIMEOUT_MS && now - s_mode_since > STATUS_TIMEOUT_MS) {
            set_mode(MODE_DARK); /* keep retrying in the background, screen off */
        }
        break;
    case MODE_LIST:
        refresh_list();
        if (now - s_last_touch > LIST_TIMEOUT_MS && now - s_mode_since > LIST_TIMEOUT_MS) {
            char name[BT_NAME_MAX + 1];
            if (bt_a2dp_saved_name(name, sizeof(name))) {
                set_mode(MODE_STATUS);
            }
        }
        break;
    case MODE_DARK:
        break;
    }

    /* Battery: only while the screen is on, and redraw just the icon. */
    if (s_mode != MODE_DARK && poll_battery(s_dirty) && !s_dirty) {
        M5.Display.startWrite();
        draw_battery();
        M5.Display.endWrite();
    }

    if (s_dirty && s_mode != MODE_DARK) {
        s_dirty = false;
        M5.Display.startWrite();
        if (s_mode == MODE_LIST) {
            draw_list();
        } else {
            draw_status();
        }
        M5.Display.endWrite();
    }
}

extern "C" void ui_run(void)
{
    auto cfg = M5.config();
    cfg.output_power = false;   /* 5 V boost for the side ports off */
    cfg.internal_imu = false;
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    cfg.led_brightness = 0;
    M5.begin(cfg);
    power_save_setup();

    M5.Touch.setHoldThresh(HOLD_MS);
    M5.Display.setRotation(1);
    M5.Display.setBrightness(BRIGHTNESS);
    ESP_LOGI(TAG, "board %d, battery %d%%", (int)M5.getBoard(), (int)M5.Power.getBatteryLevel());

    s_mode = MODE_STATUS;
    s_mode_since = s_last_touch = lgfx::millis();

    for (;;) {
        M5.update();
        handle_touch();
        update();
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}
