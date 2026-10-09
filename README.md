# Creeper Bluetooth sound box (ESP32-DevKitC)

Plays a Minecraft-Creeper-style **hiss → explosion** on a Bluetooth speaker or
headphones when you press the **BOOT** button. An optional WS2812 strip pulses
green during the hiss and flickers bright white during the explosion.

The sounds are synthesized live from filtered noise and a sine wave, so the
firmware contains no audio files.

## Build and flash (ESP-IDF v5.5)

```sh
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbserial-110 flash monitor
```

The board must be an original ESP32: S2/S3/C3/C6 chips have no Classic
Bluetooth, so they cannot do A2DP.

## Pairing

On first boot the firmware scans for `OpenRun Pro 2 by Shokz`. Put the
headphones in pairing mode: power them off, then hold **Volume +** until the
LED flashes red/blue. After connecting, the board stores the headphones'
address in NVS and reconnects to it directly on later boots. If three direct
attempts fail, it goes back to scanning.

Expected log:

```
bt: seen a8:f5:e1:7e:fb:e7 "OpenRun Pro 2 by Shokz"
bt: connected to a8:f5:e1:7e:fb:e7
bt: streaming - press BOOT to summon the Creeper
creeper: hisssss...
creeper: BOOM
creeper: done
```

To use a different speaker, change the name in `idf.py menuconfig` →
*Creeper Configuration* and run `idf.py erase-flash` to forget the stored
device.

## Settings (`idf.py menuconfig` → Creeper Configuration)

| Option | Default |
|---|---|
| Speaker name (prefix match) | `OpenRun Pro 2 by Shokz` |
| Trigger button GPIO (active low) | 0 (BOOT) |
| Volume % | 80 |
| Enable WS2812 strip | off |
| LED data GPIO / count | 18 / 30 |
| Max brightness | 255 |
| LED delay to match BT latency | 150 ms (the Shokz report 150 ms) |

## WS2812 wiring

```
5 V supply  ──── strip 5V
GND ─────────┬── strip GND
ESP32 GND ───┘   (common ground is required)
GPIO18 ── 330 Ω ── strip DIN
```

* At full white each LED draws about 60 mA, so 30 LEDs need about 1.8 A.
  Use a separate 5 V supply, or lower *Max brightness* if the strip runs
  from USB.
* WS2812 often accepts 3.3 V data. If LEDs glitch, add a 74AHCT125 level
  shifter or a sacrificial first pixel.

## Code layout

| File | Purpose |
|---|---|
| `main/bt_a2dp.c` | Classic BT A2DP source: discovery, pairing, reconnect, streaming |
| `main/creeper_audio.c` | Hiss and explosion synthesis, IDLE → HISS → EXPLODE state machine |
| `main/button.c` | Debounced BOOT button that triggers the sequence |
| `main/leds.c` | WS2812 effects over RMT, delayed to match audio latency |
