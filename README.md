# Creeper Bluetooth sound box (M5Stack Core2)

Port for the **M5Stack Core2 ESP32 IoT Kit for AWS IoT EduKit**. Plays a
Minecraft-Creeper-style **hiss → explosion** on a Bluetooth speaker when you
tap the dark screen. The DevKitC version is on the `main` branch.

The sounds are synthesized live from filtered noise and a sine wave, so the
firmware contains no audio files.

## Build and flash (ESP-IDF v5.5)

```sh
. ~/esp/esp-idf/export.sh
idf.py build                      # fetches M5Unified / M5GFX on first build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

## Using it

| Screen | Action | Result |
|---|---|---|
| "Choose a speaker" list | tap a speaker | connects and remembers it |
| any | hold 2 s | opens the speaker list (pairing mode) |
| dark | tap | Creeper hiss + explosion |
| dark, not connected | tap | shows connection status |

* On first boot (or with no saved speaker) the list opens automatically. Put
  the speaker in pairing mode; only audio devices are listed, nearest first.
  Devices that only report a shortened name are asked for their full name.
* After connecting, the screen shows "Connected!" briefly, then goes dark.
* On later boots it reconnects to the saved speaker; the status screen goes
  dark on its own after 60 s while it keeps retrying in the background.
* Choosing a new speaker removes the pairing with the old one, so the old
  speaker cannot reconnect to the Core2 on its own.

## Battery saving

The display backlight is off and the panel asleep whenever the screen is
dark. The green power LED, vibration motor, internal speaker/mic and the 5 V
output to the side ports (which also feeds the AWS kit's LED bars) are all
switched off. The LED bars are never driven.

The Bluetooth audio stream stays open (sending silence) while idle so a tap
plays instantly; this is the main remaining power draw.

## Settings (`idf.py menuconfig` → Creeper Configuration)

| Option | Default |
|---|---|
| Volume % | 80 |

## Code layout

| File | Purpose |
|---|---|
| `main/bt_a2dp.c` | Classic BT A2DP source: scan list, pairing, reconnect, streaming |
| `main/ui.cpp` | Core2 power setup, pairing screen, touch handling (M5Unified) |
| `main/creeper_audio.c` | Hiss and explosion synthesis, IDLE → HISS → EXPLODE state machine |
