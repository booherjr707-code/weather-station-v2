# Weather Station v2

ESP32 + 1.8" TFT weather display. It shows current conditions from the National
Weather Service and a **scrolling forecast line** that spells out tonight and
tomorrow, for example *"WEDNESDAY: Showers And Thunderstorms Likely, high 90,
72% rain, rainfall amounts between a quarter and half of an inch possible"*.
The line turns yellow when rain or storms are in it.

Also on screen: big temperature, clock, today's high/low, feels-like, humidity,
wind, dew point, barometer reading and a barometer gauge, a header that
rotates through the next 3 days, a flashing banner for NWS severe weather
alerts, and a 15-minute rain countdown screen (Open-Meteo) when rain is less
than an hour away.

No API keys needed. Optional Home Assistant support over MQTT.

## Parts
- ESP32 DevKit V1 (ESP32-WROOM-32)
- 1.8" 128x160 SPI TFT, ST7735 (blue board, 8-pin header)

## Wiring
| Display | ESP32 |
|---|---|
| RST | D4 |
| CS | D15 |
| D/C | D2 |
| DIN | D23 |
| CLK | D18 |
| VCC | VIN (5V, J1 open on the display) |
| BL | 3V3 |
| GND | GND |

The ESP32's blue LED is on D2, so it flickers while the screen updates. That's normal.

## Setting up a station (no computer needed)
1. Plug it into any USB phone charger.
2. The screen says to join the Wi-Fi network **Weather-Setup**. Join it on a
   phone (no password).
3. A setup page pops up (if not, open a browser to `192.168.4.1`). Tap
   **Configure WiFi**, tap your home Wi-Fi in the list, type its password and
   your **ZIP code**, then **Save**.
4. The station connects and finds your town, weather location and time zone
   from the ZIP code.

If the home Wi-Fi changes, the setup network comes back by itself. To start
over (for example before giving a station away), hold the **BOOT** button on
the ESP32 for 5 seconds.

## Building and uploading
- Gift units: `pio run -e gift -t upload` (no Home Assistant, nothing to edit).
- Home Assistant station: copy `src/secrets.example.h` to `src/secrets.h`, fill
  in the MQTT login, then `pio run -e home -t upload`.

NWS weather covers the US only.

## Display settings
The startup screen shows RED, GREEN, BLUE and YELLOW bars. If they're wrong,
see the comments next to `PANEL_BGR` / `PANEL_INVERT` in `main.cpp`. (A Lonely
Binary TFT needs `PANEL_INVERT = true`.)

## Home Assistant
With MQTT set up, the station creates a "Weather Alert Display" device
(temperature, feels like, high/low, dew point, humidity, pressure, wind,
condition, alerts, Wi-Fi signal, and a Severe Weather binary sensor). Each
unit gets its own device id from its MAC address.

## Credits
Created by [Claude Code](https://claude.com/claude-code), Anthropic's AI coding
assistant, for James Booher's ESP32 weather projects.
