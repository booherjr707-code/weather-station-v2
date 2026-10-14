// Copy this file to secrets.h (in the same folder) and fill in your own
// Wi-Fi name and password. secrets.h is git-ignored so it never gets uploaded.
// The ESP32 only works on 2.4 GHz Wi-Fi.
#pragma once

const char* WIFI_SSID     = "your-wifi-name";
const char* WIFI_PASSWORD = "your-wifi-password";

// Home Assistant MQTT broker (the Mosquitto add-on on the Raspberry Pi).
// Leave MQTT_HOST empty ("") to turn MQTT off.
const char* MQTT_HOST     = "";   // e.g. "192.168.0.50" or "homeassistant.local"
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "";
const char* MQTT_PASSWORD = "";
