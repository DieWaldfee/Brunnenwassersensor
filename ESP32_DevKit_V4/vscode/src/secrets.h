// secrets.h
#pragma once

// Definition der Zugangsdaten WiFi
#define HOSTNAME "ESP32_Brunnenwassersensor"
const char* ssid = "DEIN_WLAN_SSID";
const char* password = "DEIN_WLAN_PASSWORT";

// MQTT-Broker
#define MQTT_SERVER "192.168.x.x"
#define MQTT_PORT 1883
#define MQTT_USER "mqttbroker"
#define MQTT_PASSWORD "DEIN_MQTT_PASSWORT"
