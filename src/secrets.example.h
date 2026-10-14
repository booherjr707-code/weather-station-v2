// Only needed for the "home" build (pio run -e home), which publishes to
// Home Assistant. Copy this file to secrets.h (same folder) and fill in your
// MQTT login. secrets.h is git-ignored so it never gets uploaded.
// Wi-Fi is NOT set here: it's entered on the phone setup page.
#pragma once

// Home Assistant MQTT broker (the Mosquitto add-on). "" turns MQTT off.
const char* MQTT_HOST     = "";   // e.g. "192.168.0.50" or "homeassistant.local"
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "";
const char* MQTT_PASSWORD = "";
