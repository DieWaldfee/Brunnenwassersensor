// secrets.h

#ifndef SECRETS_H
#define SECRETS_H

// Definition der Zugangsdaten WiFi
#define HOSTNAME "ESP32_Heizstabsteuerung"
const char* ssid = "YourSSID";
const char* password = "YourPassword";

//Definition der Zugangsdaten MQTT
#define MQTT_SERVER "x.x.x.x Your IP"
#define MQTT_PORT 1883
#define MQTT_USER "Your MQTT User"
#define MQTT_PASSWORD "Your MQTT Password"

#endif // SECRETS_H

