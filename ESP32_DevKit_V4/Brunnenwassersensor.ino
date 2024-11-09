//https://beelogger.de/sensoren/temperatursensor-ds18b20/ für Pinning und Anregung
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include "secrets.h"

#define LED_ERROR 23                     // Pin für die Fehler-LED
#define LED_MSG 4                        // Pin für die Botschafts-LED (blinkt bei übertragener Botschft über MQTT)
#define LED_OK 19                        // Pin für die OK-LED
#define ONE_WIRE_BUS 25                  // Pin für die OneWire-Verbindung du den Temperatursensoren
static byte debug = 1;
static String lastError = "";

//Verbindung zum TOF-Sensor via i2c (Standard-Adresse 0x52)
#define I2C_ADDRESS 0x08                 // Default I2C address of the sensor
#define SDA_PIN 21                       // Pin für das SDA-Signal
#define SCL_PIN 22                       // Pin für das SCL-Signal
#define GOK_OFFSET 2.50                  // Distanz zwischen GOK (GeländeOberKante) und Messsystem 0-Punkt [m]
uint16_t volatile distance = 0;          // Distanz gemessen mit TOF-Sensor zur Oberkante des Schwimmers [mm]
bool volatile disStatus = 0;             // Status der Distanzmessung: 0=Fehlmessung; 1=ok
uint8_t volatile presicion = 0;          // Präzision der Messung [mm]
float volatile groundwaterLevel = 0.0;   // Grundwasserspiegel: grounwaterlevel = distance + GOK_OFFSET

// Definition der Zugangsdaten WiFi
WiFiClient myWiFiClient;

//Definition der Zugangsdaten MQTT
#define MQTT_CLIENTID "ESP32_Brunnenwassersensor" //Name muss eineindeutig auf dem MQTT-Broker sein!
#define MQTT_KEEPALIVE 90
#define MQTT_SOCKETTIMEOUT 30
#define MQTT_SERIAL_PUBLISH_STATUS "SmartHome/Garten/ESP32_Brunnenwassersensor/status"
#define MQTT_SERIAL_RECEIVER_COMMAND "SmartHome/Garten/ESP32_Brunnenwassersensor/command"
#define MQTT_SERIAL_PUBLISH_DS18B20 "SmartHome/Garten/ESP32_Brunnenwassersensor/Temperatur/"
#define MQTT_SERIAL_PUBLISH_WATER "SmartHome/Garten/ESP32_Brunnenwassersensor/Wasserhöhe/"
#define MQTT_SERIAL_PUBLISH_STATE "SmartHome/Garten/ESP32_Brunnenwassersensor/state/"
#define MQTT_SERIAL_PUBLISH_CONFIG "SmartHome/Garten/ESP32_Brunnenwassersensor/config/"
#define MQTT_SERIAL_PUBLISH_BASIS "SmartHome/Garten/ESP32_Brunnenwassersensor/"
String mqttTopic;
String mqttJson;
String mqttPayload;
DeviceAddress myDS18B20Address;
String Adresse;
unsigned long MQTTReconnect = 0;
PubSubClient mqttClient(myWiFiClient);

// Anzahl der angeschlossenen DS18B20 - Sensoren
int DS18B20_Count = 0; //Anzahl der erkannten DS18B20-Sensoren
//Beispiel Sensorsetting (Ausgabe im Debugmodus (debug = 1) auf dem serial Monitor)
  //LastError: nicht spezifizierter Temperatursensor gefunden! Reboot! (Adresse: 0x28, 0xd4, 0x28, 0x43, 0xd4, 0x25, 0x6a, 0x0a)
float volatile tempWater = 0.0;   //Sensor in Slot 1
float volatile tempAir = 0.0;     //Sensor in Slot 2
const char* AdresseWater = "0x28, 0xd4, 0x28, 0x43, 0xd4, 0x25, 0x6a, 0x0a"; // Wassertemperatur - Adresee kann über den Debugmodus (debug = 1) ermittelt werden aus dem serial Monitor
const char* AdresseAir = "0x28, 0x9b, 0xa2, 0x57, 0x04, 0xe1, 0x3c, 0x8b";   // Lufttemperatur - Adresee kann über den Debugmodus (debug = 1) ermittelt werden aus dem serial Monitor
float DS18B20_minValue = -55.0;   //unterster Messwert im Messbereich [°C]
float DS18B20_maxValue = 125.0;   //unterster Messwert im Messbereich [°C]
#define DS18B20_RESOLUTION 12     // 9bit: +-0.5°C @ 93.75 ms; 10bit: +-0.25°C @ 187.5 ms; 11bit: +-0.125°C @ 375 ms; 12bit: +-0.0625°C @ 750 ms
#define DS18B20_DELAY 752         // Wartezeit nach angetriggerter Messung [ms]
int tempTSensorFail = 0;          // Anzahl der aktuell hintereinanderfolgenden Fehler beim Auslesen der Temp-Sensoren
int maxTSensorFail = 3;           // 3 gestattete Fehler in Folge beim Auslesen der Temperatursensoren

//Initialisiere OneWire und Thermosensor(en)
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature myDS18B20(&oneWire);

//Mutexdefinitionen
static SemaphoreHandle_t mutexTemp;
static SemaphoreHandle_t mutexTOF;
static SemaphoreHandle_t mutexTempSensor;
static SemaphoreHandle_t mutexTOFSensor;

//TaskRefrechTime
#define MQTTStateRefresh 20000         // Alle 20.000 Ticks = 20sec
#define DistanceRefresh 2000          // Alle 20.000 Ticks = 20sec

//TaskHandler zur Verwendung mit ESP watchdog
static TaskHandle_t htempSensor;
static TaskHandle_t htofSensor;
static TaskHandle_t hMQTTwatchdog;

//-------------------------------------
// Callback für MQTT
void mqttCallback(char* topic, byte* message, unsigned int length) {
  BaseType_t rc;
  String str;
  unsigned long mqttValue;
  String mqttMessage;
  String mqttTopicAC;
  byte tx_ac = 1;
  for (int i = 0; i < length; i++)
  {
    str += (char)message[i];
  }
  if (debug > 1) {
    Serial.print("Nachricht aus dem Topic: ");
    Serial.print(topic);
    Serial.print(". Nachricht: ");
    Serial.println(str);
  }
  //Test-Botschaften  
  mqttTopicAC = MQTT_SERIAL_PUBLISH_BASIS;
  mqttTopicAC += "ac";
  if (str.startsWith("Test")) {
    if (debug) Serial.println("Test -> Test OK");
    mqttClient.publish(mqttTopicAC.c_str(), "Test OK");
    tx_ac = 0;
  }

  //debug-Modfikation  
  if ((tx_ac) && (str.startsWith("debug=0"))) {
    debug = 0;
    mqttClient.publish(mqttTopicAC.c_str(), "debug=0 umgesetzt");
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("debug=1"))) {
    debug = 1;
    mqttClient.publish(mqttTopicAC.c_str(), "debug=1 umgesetzt");
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("debug=2"))) {
    debug = 2;
    mqttClient.publish(mqttTopicAC.c_str(), "debug=2 umgesetzt");
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("debug=3"))) {
    debug = 3;
    mqttClient.publish(mqttTopicAC.c_str(), "debug=3 umgesetzt");
    tx_ac = 0;
  }
  //ErrorLED aus
    if ((tx_ac) && (str.startsWith("ErrorLED aus"))) {
    mqttMessage = "ErrorLED ausgeschaltet";
    digitalWrite(LED_ERROR, LOW);
    if (debug > 2) Serial.println(mqttMessage);
    mqttClient.publish(mqttTopicAC.c_str(), mqttMessage.c_str());
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("restart"))) {
    mqttClient.publish(mqttTopicAC.c_str(), "reboot in einer Sekunde!");
    if (debug) Serial.println("für Restart: alles aus & restart in 1s!");
    digitalWrite(LED_OK, LOW);
    digitalWrite(LED_ERROR, HIGH);
    vTaskDelay(1000);
    if (debug) Serial.println("führe Restart aus!");
    ESP.restart();
  }
}

//-------------------------------------
//Subfunktionen für MQTT-Status-Task
// MQTT DS18B20 Status senden
void printDS18B20MQTT() {
  int i;
  for (i = 0; i < DS18B20_Count; i++) {
    //MQTT-Botschaften
    //JSON        
    myDS18B20.getAddress(myDS18B20Address,i);
    Adresse="";
    for (uint8_t j = 0; j < 8; j++)
    {
      Adresse += "0x";
      if (myDS18B20Address[j] < 0x10) Adresse += "0";
      Adresse += String(myDS18B20Address[j], HEX);
      if (j < 7) Adresse += ", ";
    }
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/JSON"; 
    mqttJson = "{\"ID\":\"" + String(i) + "\"";
    mqttJson += ",\"Temperatur\":\"" + String(myDS18B20.getTempCByIndex(i)) + "\"";
    mqttJson += ",\"Adresse\":\"(" + Adresse + ")\"";
    if (Adresse == AdresseWater) mqttJson += ",\"Ort\":\"Temperatur Wasser\"}";
    if (Adresse == AdresseAir) mqttJson += ",\"Ort\":\"Temperatur Luft\"}";
    if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
    mqttClient.publish(mqttTopic.c_str(), mqttJson.c_str());
    //Temperatur
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Temperatur";
    mqttPayload = String(myDS18B20.getTempCByIndex(i));
    mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
    if (debug > 2) Serial.print("MQTT ID: ");
    if (debug > 2) Serial.println(mqttPayload);
    //ID
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/ID";
    mqttPayload = String(i);
    mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
    if (debug > 2) Serial.print("MQTT Temperatur: ");
    if (debug > 2) Serial.println(mqttPayload);
    //Adresse
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Adresse";
    mqttClient.publish(mqttTopic.c_str(), Adresse.c_str());
    if (debug > 2) Serial.print("MQTT Adresse: ");
    if (debug > 2) Serial.println(Adresse);
    //Ort
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Ort";
    if (Adresse == AdresseWater) mqttPayload = "Temperatur Wasser";
    if (Adresse == AdresseAir) mqttPayload = "Temperatur Luft";
    mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
    if (debug > 2) Serial.print("MQTT Ort: ");
    if (debug > 2) Serial.println(mqttPayload);
  }
}
// MQTT Wasserhöhe / Distanz Status senden
void printGroundwaterLevelMQTT() {
  if (groundwaterLevel != 0) {
    //Grundwasserspiegel
    mqttTopic = MQTT_SERIAL_PUBLISH_WATER + String("Grundwasserspiegel_unter_GOK");
    mqttPayload = groundwaterLevel;
    mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
    if (debug > 2) Serial.print("Grundwasserspiegel unter GOK: ");
    if (debug > 2) Serial.println(mqttPayload);
  }
}
// MQTT Status Betrieb senden
void printStateMQTT() {
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "JSON";
  mqttJson = "{\"WiFi_Signal_Strength\":\"" + String(WiFi.RSSI()) + "\"";
  mqttJson += ",\"lastError\":\"" + String(lastError) + "\"}";
  if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
  mqttClient.publish(mqttTopic.c_str(), mqttJson.c_str());
  //lastError
  if (lastError != ""){
    mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
    mqttTopic += "lastError";
    mqttPayload = lastError;
    mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
    if (debug > 2) Serial.print("LastError: ");
    if (debug > 2) Serial.println(mqttPayload);
  }
  //WiFi Signalstärke
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "WiFi_Signal_Strength";
  mqttPayload = WiFi.RSSI();
  mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
  if (debug > 2) Serial.print("WiFi Signalstärke: ");
  if (debug > 2) Serial.println(mqttPayload);
  //Distanz gemessen
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "Distanz_gemessen";
  mqttPayload = distance;
  mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
  if (debug > 2) Serial.print("Distanz gemessen: ");
  if (debug > 2) Serial.println(mqttPayload);
  //Distanz gemessen [mm]
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "Distanz_gemessen";
  mqttPayload = distance;
  mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
  if (debug > 2) Serial.print("Distanz gemessen [mm]: ");
  if (debug > 2) Serial.println(mqttPayload);
  //Distanz Status
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "Distanz_Status";
  if (disStatus == 0) {
    mqttPayload = true;
  } else {
    mqttPayload = false;
  }
  mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
  if (debug > 2) Serial.print("Distanz Status: ");
  if (debug > 2) Serial.println(mqttPayload);
  //Präzision gemessen [mm]
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "Präzision_Distanz";
  mqttPayload = presicion;
  mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
  if (debug > 2) Serial.print("Präzision Distanz [mm]: ");
  if (debug > 2) Serial.println(mqttPayload);
}
// MQTT Config und Parameter senden
void printConfigMQTT() {
  //Teil 1
  mqttTopic = MQTT_SERIAL_PUBLISH_CONFIG;
  mqttTopic += "JSON_0";
  mqttJson = "{\"GOK_Offset\":\"" + String(GOK_OFFSET) + "\"}";
  if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
  mqttClient.publish(mqttTopic.c_str(), mqttJson.c_str());
}
//LED-Blik-OK
void LEDblinkMSG(){
  digitalWrite(LED_MSG, HIGH);
  delay(150);
  digitalWrite(LED_MSG, LOW);
}
//-------------------------------------
//MQTT-Status-Task
static void MQTTstate (void *args){
  BaseType_t rc;
  float a1;
  float a2;
  float a3;
  float p1on;
  float p2on;
  float p3on;
  TickType_t ticktime;

  //ticktime initialisieren
  ticktime = xTaskGetTickCount();

  for (;;){                        // Dauerschleife des Tasks
    //Lesen der Temperaturen
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | MQTT-Status-Task gestartet");
    if (mqttClient.connected()) {
      rc = xSemaphoreTake(mutexTempSensor, portMAX_DELAY);
      assert(rc == pdPASS);
        rc = xSemaphoreTake(mutexTemp, portMAX_DELAY);
        assert(rc == pdPASS);
          printDS18B20MQTT();
        rc = xSemaphoreGive(mutexTemp);
        assert(rc == pdPASS);
      rc = xSemaphoreGive(mutexTempSensor);
      assert(rc == pdPASS);

      rc = xSemaphoreTake(mutexTOFSensor, portMAX_DELAY);
      assert(rc == pdPASS);
        rc = xSemaphoreTake(mutexTOF, portMAX_DELAY);
        assert(rc == pdPASS);
          printGroundwaterLevelMQTT();
        rc = xSemaphoreGive(mutexTOF);
        assert(rc == pdPASS);
      rc = xSemaphoreGive(mutexTOFSensor);
      assert(rc == pdPASS);

      printStateMQTT();

      printConfigMQTT();
    }

    // Task schlafen legen - restart MQTTStateRefresh ticks
    LEDblinkMSG();
    vTaskDelayUntil(&ticktime, MQTTStateRefresh);
  }
}

//-------------------------------------
//Subfunktionen für MQTTwatchdog-Task
// MQTT Verbindung herstellen (wird auch von setup verwendet!)
void mqttConnect () {
  int i = 0;
  Serial.print("Verbindungsaubfau zu MQTT Server ");
  Serial.print(MQTT_SERVER);
  Serial.print(" Port ");  
  Serial.print(MQTT_PORT);
  Serial.print(" wird aufgebaut ");  
  while (!mqttClient.connected()) {
    Serial.print(".");
    if (mqttClient.connect(MQTT_CLIENTID, MQTT_USER, MQTT_PASSWORD, MQTT_SERIAL_PUBLISH_STATUS, 0, true, "false")) {
      mqttClient.publish(MQTT_SERIAL_PUBLISH_STATUS, "true", true);
      Serial.println("");
      Serial.print("MQTT verbunden!");
    } 
    else {
      if (++i > 20) {
        Serial.println("MQTT scheint nicht mehr erreichbar! Reboot!!");
        ESP.restart();
      }
      Serial.print("fehlgeschlagen rc=");
      Serial.print(mqttClient.state());
      Serial.println(" erneuter Versuch in 5 Sekunden.");
      delay(5000);      
    }    
  }
  mqttClient.subscribe(MQTT_SERIAL_RECEIVER_COMMAND);
}
// MQTT Verbindungsprüfung 
void checkMQTTconnetion() {
  BaseType_t rc;
  if (!mqttClient.connected()) {
    if (debug) Serial.println("MQTT Server Verbindung verloren...");
    if (debug) Serial.print("Disconnect Errorcode: ");
    if (debug) Serial.println(mqttClient.state());  
    //Vorbereitung errorcode MQTT (https://pubsubclient.knolleary.net/api#state)
    mqttTopic = MQTT_SERIAL_PUBLISH_BASIS + String("error");
    mqttPayload = String(String(++MQTTReconnect) + ". reconnect: ") + String("; MQTT disconnect rc=" + String(mqttClient.state()));
    //reconnect
    mqttConnect();
    //sende Fehlerstatus
    mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
    //thermalLimits wieder einschalten
  }
  mqttClient.loop();
}
//-------------------------------------
//MQTT-MQTTwatchdog-Task
static void MQTTwatchdog (void *args){
  BaseType_t rc;
  esp_err_t er;
  TickType_t ticktime;

  //ticktime initialisieren
  ticktime = xTaskGetTickCount();

  er = esp_task_wdt_add(NULL);   // Task zur Überwachung hinzugefügt  
  assert(er == ESP_OK); 

  for (;;){                        // Dauerschleife des Tasks
    // Watchdog zurücksetzen
    esp_task_wdt_reset();
    //Lesen der Temperaturen
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | MQTTonlinePrüf-Task gestartet");
    checkMQTTconnetion();

    // Task schlafen legen - restart alle 2s = 2*1000 ticks = 2000 ticks
    // mit mqttClient.loop() wird auch der MQTTcallback ausgeführt!
    vTaskDelayUntil(&ticktime, 2000);
  }
}

//-------------------------------------
//Subfunktionen für den TempSensor-Task
// Temperatursensorenwerte auf die Limits prüfen
bool checkDS18B20Value (float t){
  bool res = true;     // true = im Messbereich; false = außerhalb des Messbereichs
  if ((t < DS18B20_minValue) || (t > DS18B20_maxValue)){
    //Sensorwert außerhalb des Messbereichs
    res = false;
  }
  if (debug > 2) Serial.print("Prüfe t-Wert auf Gültigkeit: ");
  if (debug > 2) Serial.print(t);
  if (debug > 2) Serial.print("°C [");
  if (debug > 2) Serial.print(DS18B20_minValue);
  if (debug > 2) Serial.print(",");
  if (debug > 2) Serial.print(DS18B20_maxValue);
  if (debug > 2) Serial.print("]; Ergebnis: ");
  if (debug > 2) Serial.println(res);
  return res;
}
// Temperatursensoren auslesen
void readDS18B20() {
  float tAir = 0.0;
  float tWater = 0.0;
  bool res1 = false;
  bool res2 = false;
  if (debug > 2) Serial.print("Anfrage der Temperatursensoren... ");
  myDS18B20.requestTemperatures();                    //Anfrage zum Auslesen der Temperaturen
  delay(DS18B20_DELAY);                               // Wartezeit bis Messung abgeschlossen ist
  if (debug > 2) Serial.println("fertig");
  for (int i = 0; i < DS18B20_Count; i++) {
    myDS18B20.getAddress(myDS18B20Address,i);
    Adresse="";
    for (uint8_t j = 0; j < 8; j++)
    {
      Adresse += "0x";
      if (myDS18B20Address[j] < 0x10) Adresse += "0";
      Adresse += String(myDS18B20Address[j], HEX);
      if (j < 7) Adresse += ", ";
    }
    if (Adresse == AdresseWater) {
      tWater = myDS18B20.getTempCByIndex(i);
    } else if (Adresse == AdresseAir) {
      tAir = myDS18B20.getTempCByIndex(i);
    } else {
      Adresse="";
      for (uint8_t j = 0; j < 8; j++)
      {
        Adresse += "0x";
        if (myDS18B20Address[j] < 0x10) Adresse += "0";
        Adresse += String(myDS18B20Address[j], HEX);
        if (j < 7) Adresse += ", ";
      }
      mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
      mqttTopic += "lastError";
      mqttPayload = "nicht spezifizierter Temperatursensor gefunden! Reboot! (Adresse: " + Adresse + ")";
      mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
      if (debug > 2) Serial.print("LastError: ");
      if (debug > 2) Serial.println(mqttPayload);
      delay(500);
      ESP.restart();
    }
  }
  //Plausibilitätscheck
  if (checkDS18B20Value(tWater)) {
    tempWater = tWater;
    res1 = true;
  }
  else {
    ++tempTSensorFail;
    res1 = false;
    mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
    mqttTopic += "lastError";
    mqttPayload = "Temperatursensor TWater außerhalb des Messbereichts: " + String(tWater) + "[C]; Wiederholung: " + String(tempTSensorFail);
    mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
    if (debug > 2) Serial.print("LastError: ");
    if (debug > 2) Serial.println(mqttPayload); //(debug > 2)
  }
  if (checkDS18B20Value(tAir)) {
    tempAir = tAir;
    res2 = true;
  }
  else {
    ++tempTSensorFail;
    res2 = false;
    mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
    mqttTopic += "lastError";
    mqttPayload = "Temperatursensor TAir außerhalb des Messbereichts: " + String(tAir) + "[C]; Wiederholung: " + String(tempTSensorFail);
    mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
    if (debug > 2) Serial.print("LastError: ");
    if (debug > 2) Serial.println(mqttPayload);
  }
  if (tempTSensorFail > maxTSensorFail) {
    Serial.println("zu viele Fehler (out of range) beim Auslesen der DS18B20! Reboot!!");
    ESP.restart();
  }
}
//Debug-Ausgabe der Temp-Sensorwerte
void printDS18B20() {
  if (debug > 2) {
    for (int i = 0; i < DS18B20_Count; i++) {
      //print to Serial
      Serial.print("DS18B20[");
      Serial.print(i);
      Serial.print("]: ");
      Serial.print(myDS18B20.getTempCByIndex(i));
      Serial.print(" *C (");
      myDS18B20.getAddress(myDS18B20Address,i);
      Adresse="";
      for (uint8_t j = 0; j < 8; j++)
      {
        Adresse += "0x";
        if (myDS18B20Address[j] < 0x10) Adresse += "0";
        Adresse += String(myDS18B20Address[j], HEX);
        if (j < 7) Adresse += ", ";
      }
      Serial.println(Adresse + ")");
    }
  }
}

//-------------------------------------
// Task zur Ermittlung der Temperaturen
static void getTempFromSensor (void *args){
  BaseType_t rc;
  esp_err_t er;
  TickType_t ticktime;

  //ticktime initialisieren
  ticktime = xTaskGetTickCount();

  er = esp_task_wdt_add(NULL);   // Task zur Überwachung hinzugefügt  
  assert(er == ESP_OK); 

  for (;;){                        // Dauerschleife des Tasks
    // Watchdog zurücksetzen
    esp_task_wdt_reset();
    //Lesen der Temperaturen
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | TempSensor-Task liest DS18B20-Sensoren aus");
    rc = xSemaphoreTake(mutexTempSensor, portMAX_DELAY);
    assert(rc == pdPASS);
      rc = xSemaphoreTake(mutexTemp, portMAX_DELAY);
      assert(rc == pdPASS);
        readDS18B20();                // Sensoren auslesen und den Variablen zuordnen
        printDS18B20();               // DebugInfo auf Serial (thermale Infos)
      rc = xSemaphoreGive(mutexTemp);
      assert(rc == pdPASS);
    rc = xSemaphoreGive(mutexTempSensor);
    assert(rc == pdPASS);

    // Task schlafen legen - restart alle 5s = 5*1000 ticks = 5000 ticks
    vTaskDelayUntil(&ticktime, 5000);
  }
}

//-------------------------------------
//Subfunktionen für die Abstandsmessung
// Distanzsensorenwerte auslesen
void readDistance (){
  // Anfrage der Register 0x24 = Distanz
  Wire.beginTransmission(I2C_ADDRESS);
  Wire.write(0x24);
  Wire.endTransmission(I2C_ADDRESS);
  // Auslesen der Antwort und zusammensetzen zu einer Zahl
  Wire.requestFrom(I2C_ADDRESS, 2);  // Request 2 bytes
  if (Wire.available() == 2) {
   distance = Wire.read() | (Wire.read() << 8);
  } else {
    distance = 0;
  }

  // Anfrage der Register 0x28 = Distanz Status und Signalstärke
  Wire.beginTransmission(I2C_ADDRESS);
  Wire.write(0x28);
  Wire.endTransmission(I2C_ADDRESS);
 // Auslesen der Antwort
  Wire.requestFrom(I2C_ADDRESS, 1);  // Request 2 bytes
  if (Wire.available() == 1) {
    disStatus = Wire.read();
    // signStrength = Wire.read();
  } else {
    disStatus = 0;
    // signStrength = 99;
  }

  // Anfrage der Register 0x2C = prezision
  Wire.beginTransmission(I2C_ADDRESS);
  Wire.write(0x2C);
  Wire.endTransmission(I2C_ADDRESS);
  // Auslesen der Antwort
  Wire.requestFrom(I2C_ADDRESS, 1);  // Request 1 bytes
  if (Wire.available() == 1) {
    presicion = Wire.read() & 0x00FF;  // nur bits [0-7]
  } else {
    presicion = 0;
  }
  //Änderung des Grundwasserspiegels nur nach gültiger Messung
  if (disStatus != 0){
    groundwaterLevel = ((float)distance / 1000.0) + GOK_OFFSET;
  }
}
// Debug-Ausgabe derDistanzsensorenwerte
void printDistance (){
  if (debug > 0) {
    Serial.printf("Gemessene Distanz: %5d mm - Status: ", distance);
    if (disStatus == 0) {
      Serial.print("falsch");
    } else {
      Serial.print("ok");
    }
    Serial.printf(" - Präzision: %d mm\n", presicion);
  }
}

//-------------------------------------
// Task zur Ermittlung der Distanz
static void getDistanceFromSensor (void *args){
  BaseType_t rc;
  esp_err_t er;
  TickType_t ticktime;

  //ticktime initialisieren
  ticktime = xTaskGetTickCount();

  er = esp_task_wdt_add(NULL);   // Task zur Überwachung hinzugefügt  
  assert(er == ESP_OK); 

  for (;;){                        // Dauerschleife des Tasks
    // Watchdog zurücksetzen
    esp_task_wdt_reset();
    //Lesen der Distanz
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | Distanz-Sensor-Task liest TOF-Sensor aus");
    rc = xSemaphoreTake(mutexTOFSensor, portMAX_DELAY);
    assert(rc == pdPASS);
      rc = xSemaphoreTake(mutexTOF, portMAX_DELAY);
      assert(rc == pdPASS);
        readDistance();                    // Sensoren auslesen und den Variablen zuordnen
        printDistance();                   // DebugInfo auf Serial (Distanz-Infos)
      rc = xSemaphoreGive(mutexTOF);
      assert(rc == pdPASS);
    rc = xSemaphoreGive(mutexTOFSensor);
    assert(rc == pdPASS);

    // Task schlafen legen - restart alle DistanceRefresh Ticks
    vTaskDelayUntil(&ticktime, DistanceRefresh);
  }
}

void setup() {
  //Watchdog starten
  esp_err_t er;
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 300000,  // 5 Minuten = 300000 ms
    .idle_core_mask = (1 << 1),  // Nur Kerne 1 überwachen
    .trigger_panic = true
  };
  er = esp_task_wdt_reconfigure(&wdt_config);  //restart nach 5min = 300s Inaktivität einer der 4 überwachten Tasks 
  assert(er == ESP_OK); 
  // Initialisierung und Plausibilitaetschecks
  Serial.begin(115200);
  while (!Serial)
  Serial.println("Start Setup");
  pinMode(LED_ERROR, OUTPUT);
  digitalWrite(LED_ERROR, HIGH);
  pinMode(LED_MSG, OUTPUT);
  digitalWrite(LED_MSG, HIGH);
  pinMode(LED_OK, OUTPUT);
  digitalWrite(LED_OK, HIGH);
  // Init TOF Sensor
  // Initialisiere den I2C-Bus mit definierten SDA und SCL Pins
  Wire.begin(SDA_PIN, SCL_PIN);
  // Setze die Taktfrequenz auf 100kHz (Standard ist 400kHz für den ESP32)
  Wire.setClock(100000);        // 100kHz I2C-Takt
  delay(10);                    // warte auf den Sensorboot
  //Scanne I2C nach Geräten
  Serial.println("\nI2C Scanner");
  uint8_t pieces = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      pieces++;
      Serial.print("I2C device found at address 0x");
      Serial.println(address, HEX);
    }
  }
  Serial.print("Scan beendet. Gefundene Geräte: ");
  Serial.print(pieces);
  Serial.println(".");
  //WiFi-Setup
  int i = 0;
  Serial.print("Verbindungsaufbau zu ");
  Serial.print(ssid);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid,password);
  while (WiFi.status() != WL_CONNECTED)
  {
    if (++i > 240) {
      // Reboot nach 2min der Fehlversuche
      Serial.println("WLAN scheint nicht mehr erreichbar! Reboot!!");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");    
  }
  Serial.println("");
  Serial.println("WiFi verbunden.");
  Serial.print("IP Adresse: ");
  Serial.print(WiFi.localIP());
  Serial.println("");
  //MQTT-Setup
  Serial.println("MQTT Server Initialisierung laeuft...");
  mqttClient.setServer(MQTT_SERVER,MQTT_PORT); 
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE);
  mqttClient.setSocketTimeout(MQTT_SOCKETTIMEOUT);
  mqttConnect();
  mqttTopic = MQTT_SERIAL_PUBLISH_BASIS + String("error");
  mqttPayload = String(String(MQTTReconnect) + ".: keine MQTT-Fehler seit Reboot!");
  mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
  Serial.println("");
  //DS18B20-Setup
  Serial.println("Auslesen der DS18B20-Sensoren...");
  myDS18B20.begin();
  Serial.print("Anzahl gefundener 1-Wire-Geraete:  ");
  Serial.println(myDS18B20.getDeviceCount());
  DS18B20_Count = myDS18B20.getDS18Count();
  Serial.print("Anzahl gefundener DS18B20-Geraete: ");
  Serial.println(DS18B20_Count);
  //Setzen der Auflösungen
  myDS18B20.setResolution(DS18B20_RESOLUTION);                  // globale Auflösung gesetzt
  Serial.print("Globale Aufloesung (Bit):        ");
  Serial.println(myDS18B20.getResolution());
  if (debug > 0){
  for (int i = 0; i < DS18B20_Count; i++) {
      //print to Serial
      Serial.print("DS18B20[");
      Serial.print(i);
      Serial.print("]: ");
      Serial.print(myDS18B20.getTempCByIndex(i));
      Serial.print(" *C (");
      myDS18B20.getAddress(myDS18B20Address,i);
      Adresse="";
      for (uint8_t j = 0; j < 8; j++)
      {
        Adresse += "0x";
        if (myDS18B20Address[j] < 0x10) Adresse += "0";
        Adresse += String(myDS18B20Address[j], HEX);
        if (j < 7) Adresse += ", ";
      }
      Serial.println(Adresse + ")");
    }
  }
  if (DS18B20_Count < 2) {
    Serial.println("... Anzahl DB18B20 < 2 => zu wenig! ... System angehalten!");
    digitalWrite(LED_OK, LOW);
    while (true) {
      //blinke bis zur Unendlichkeit...
      digitalWrite(LED_ERROR, HIGH);
      delay(250);
      digitalWrite(LED_ERROR, LOW);
      delay(250);
    }
  }
  //Mutex-Initialisierung
  mutexTemp = xSemaphoreCreateMutex();
  assert(mutexTemp);
  mutexTOF = xSemaphoreCreateMutex();
  assert(mutexTOF);
  mutexTempSensor = xSemaphoreCreateMutex();
  assert(mutexTempSensor);
  mutexTOFSensor = xSemaphoreCreateMutex();
  assert(mutexTOFSensor);
  Serial.println("Mutex-Einrichtung erforlgreich.");
  //Tasks starten
  int app_cpu = xPortGetCoreID();
  BaseType_t rc;
  rc = xTaskCreatePinnedToCore(
    getTempFromSensor,         //Taskroutine
    "getTempSensorTask",       //Taskname
    2048,                      //StackSize
    nullptr,                   //Argumente / Parameter
    2,                         //Priorität
    &htempSensor,              //handler
    app_cpu);                  //CPU_ID
  assert(rc == pdPASS);
  Serial.println("TempSensor-Task gestartet.");
  rc = xTaskCreatePinnedToCore(
    MQTTwatchdog,              //Taskroutine
    "MQTTwatchdog",            //Taskname
    2048,                      //StackSize
    nullptr,                   //Argumente / Parameter
    1,                         //Priorität
    &hMQTTwatchdog,            //handler
    app_cpu);                  //CPU_ID
  assert(rc == pdPASS);
  Serial.println("MQTT-Watchdog-Task gestartet.");
  rc = xTaskCreatePinnedToCore(
    MQTTstate,                 //Taskroutine
    "MQTTstate",               //Taskname
    2048,                      //StackSize
    nullptr,                   //Argumente / Parameter
    1,                         //Priorität
    nullptr,                   //handler
    app_cpu);                  //CPU_ID
  assert(rc == pdPASS);
  Serial.println("MQTT-State-Task gestartet.");
  rc = xTaskCreatePinnedToCore(
    getDistanceFromSensor,     //Taskroutine
    "getDistanceTask",         //Taskname
    2048,                      //StackSize
    nullptr,                   //Argumente / Parameter
    2,                         //Priorität
    nullptr,                   //handler
    app_cpu);                  //CPU_ID
  assert(rc == pdPASS);
  Serial.println("TOF-Distanz-Task gestartet.");
  //OK-Blinker
  digitalWrite(LED_ERROR, LOW);
  digitalWrite(LED_OK, LOW);
  delay(250);
  digitalWrite(LED_OK, HIGH);
  delay(250);
  digitalWrite(LED_OK, LOW);
  Serial.println("Normalbetrieb gestartet...");
  //Startmeldung via MQTT
  String mqttTopicAC;
  mqttTopicAC = MQTT_SERIAL_PUBLISH_BASIS;
  mqttTopicAC += "ac";
  mqttClient.publish(mqttTopicAC.c_str(), "Start durchgeführt.");
}

void loop() {
  //loop wird als Task nicht gebraucht
  vTaskDelete(nullptr);
}
