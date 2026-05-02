//https://beelogger.de/sensoren/temperatursensor-ds18b20/ für Pinning und Anregung
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include "secrets.h"
#include "hardware.h"

static byte debug = 0;
static String lastError = "";

uint16_t volatile distance = 0;          // Distanz gemessen mit TOF-Sensor zur Oberkante des Schwimmers [mm]
bool volatile disStatus = 0;             // Status der Distanzmessung: 0=Fehlmessung; 1=ok
uint8_t volatile presicion = 0;          // Präzision der Messung [mm]
float volatile groundwaterLevel = 0.0;   // Grundwasserspiegel: grounwaterlevel = distance + GOK_OFFSET

// Definition der Zugangsdaten WiFi
WiFiClient myWiFiClient;

//Definition der Zugangsdaten MQTT
#define MQTT_CLIENTID "ESP32_Brunnenwassersensor" //Name muss eineindeutig auf dem MQTT-Broker sein!
#define BWS_MQTT_KEEPALIVE 90
#define BWS_MQTT_SOCKETTIMEOUT 30
#define MQTT_SERIAL_PUBLISH_STATUS "SmartHome/Garten/ESP32_Brunnenwassersensor/status"
#define MQTT_SERIAL_RECEIVER_COMMAND "SmartHome/Garten/ESP32_Brunnenwassersensor/command"
#define MQTT_SERIAL_PUBLISH_DS18B20 "SmartHome/Garten/ESP32_Brunnenwassersensor/Temperatur/"
#define MQTT_SERIAL_PUBLISH_WATER "SmartHome/Garten/ESP32_Brunnenwassersensor/Wasserhöhe/"
#define MQTT_SERIAL_PUBLISH_STATE "SmartHome/Garten/ESP32_Brunnenwassersensor/state/"
#define MQTT_SERIAL_PUBLISH_CONFIG "SmartHome/Garten/ESP32_Brunnenwassersensor/config/"
#define MQTT_SERIAL_PUBLISH_BASIS "SmartHome/Garten/ESP32_Brunnenwassersensor/"
DeviceAddress myDS18B20Address;
String Adresse;
unsigned long MQTTReconnect = 0;
#define MQTT_QUEUEDEPTH 50                // Tiefe der MQTT-Queue - 50 Botschaften
#define MQTT_QUEUEMAXWAITTIME 3           // Wartezeit für das Senden in eine Queue - danach Error!
struct MqttJob {                          // Struktur der MQTT-Queue
  char topic[128];                        // topic:   Topic auf den die Botschaft gesendet werden soll -> 180 Zeichen lang
  char payload[256];                      // payload: Botschaft, die an das Topic gesendet werden soll. -> 256 Zeichen max.
  bool retain;                            // retain:  true, wenn die Botschaft im Broker gespeichert bleibt und false, wenn
};                                        // nur die angemeldeten User die Botschaft erhalten - diese dann vergessen wird.
PubSubClient mqttClient(myWiFiClient);
static QueueHandle_t mqttQueue;           // Queuedefinition für die MQTT-Queue
static TaskHandle_t hmqtt;                // handler für den MQTT-Sender-Task

// Anzahl der angeschlossenen DS18B20 - Sensoren
int DS18B20_Count = 0;
float volatile tempWater = 0.0;
float volatile tempAir = 0.0;
float DS18B20_minValue = -55.0;   //unterster Messwert im Messbereich [°C]
float DS18B20_maxValue = 125.0;   //oberster Messwert im Messbereich [°C]
int volatile tempTSensorFail = 0; // Anzahl der aktuell hintereinanderfolgenden Fehler beim Auslesen der Temp-Sensoren
int maxTSensorFail = 3;           // 3 gestattete Fehler in Folge beim Auslesen der Temperatursensoren
int volatile tofFailCount = 0;    // Anzahl der aktuell hintereinanderfolgenden TOF I2C Fehler
int maxTofFail = 5;               // 5 gestattete Fehler in Folge beim Auslesen des TOF-Sensors

//Initialisiere OneWire und Thermosensor(en)
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature myDS18B20(&oneWire);

//Mutexdefinitionen
static SemaphoreHandle_t mutexTemp;
static SemaphoreHandle_t mutexTOF;
static SemaphoreHandle_t mutexMQTT;
static SemaphoreHandle_t mutexLastError;

//TaskRefrechTime
#define MQTTStateRefresh 60000         // Alle 60.000 Ticks = 60sec
#define DistanceRefresh 30000          // Alle 30.000 Ticks = 30sec

//TaskHandler zur Verwendung mit ESP watchdog
static TaskHandle_t htempSensor;
static TaskHandle_t hMQTTwatchdog;

//erforderliche Funtions-Prototypen
bool mqttPublishQueue(const char*, const char*, bool);
String formatDS18B20Address(const DeviceAddress);

//-------------------------------------
// Basisfunktion zum sicheren Reset
void safeReset() {
  Serial.println("ESP32 Reset wird vorbereitet...");
  // Sauberes MQTT-Disconnect nur wenn Mutexe existieren (d.h. nicht während setup()-Phase).
  // Ohne diese Prüfung: xSemaphoreTake(NULL) → Assert-Crash wenn safeReset() aus mqttConnect()
  // heraus aufgerufen wird, bevor die Mutex-Initialisierung in setup() stattgefunden hat.
  if (mutexMQTT != nullptr) {
    Serial.println("MQTT Disconnect...");
    xSemaphoreTake(mutexMQTT, pdMS_TO_TICKS(1000));  // best-effort; kein assert - Reboot folgt
    mqttClient.disconnect();
    // kein xSemaphoreGive - blockiert alle MQTT-Operationen anderer Tasks bis Reboot
    delay(50);
    Serial.println("Flush TCP-Buffer...");
    myWiFiClient.clear();
    delay(50);
  }
  Serial.println("ESP32 Reset!");
  ESP.restart();
}

//-------------------------------------
// Callback für MQTT
void mqttCallback(char* topic, byte* message, unsigned int length) {
  String mqttTopic;
  String str;
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
    mqttPublishQueue(mqttTopicAC.c_str(), "Test OK", false);
    tx_ac = 0;
  }

  //debug-Modifikation
  if ((tx_ac) && (str.startsWith("debug="))) {
    if (str[6] >= '0' && str[6] <= '3') {
      debug = str[6] - '0';
      mqttPublishQueue(mqttTopicAC.c_str(), ("debug=" + String(debug) + " umgesetzt").c_str(), false);
      tx_ac = 0;
    }
  }
  //ErrorLED aus
    if ((tx_ac) && (str.startsWith("ErrorLED aus"))) {
    mqttMessage = "ErrorLED ausgeschaltet";
    digitalWrite(LED_ERROR, LOW);
    if (debug > 2) Serial.println(mqttMessage);
    mqttPublishQueue(mqttTopicAC.c_str(), mqttMessage.c_str(), false);
    tx_ac = 0;
  }
  if ((tx_ac) && ((str.startsWith("restart")) || (str.startsWith("reboot")))) {
    // Kein Mutex nötig: Callback wird von mqttSender aufgerufen, das mutexMQTT bereits hält
    mqttClient.publish(mqttTopicAC.c_str(), "reboot in einer Sekunde!");
    if (debug) Serial.println("für Restart: alles aus & restart in 1s!");
    digitalWrite(LED_OK, LOW);
    digitalWrite(LED_ERROR, HIGH);
    vTaskDelay(1000);
    if (debug) Serial.println("führe Restart aus!");
    tx_ac = 0;
    safeReset();
  }
}

//-------------------------------------
//Subfunktionen für MQTT-Status-Task
// MQTT DS18B20 Status senden
void printDS18B20MQTT() {
  String mqttTopic;
  String mqttJson;
  String mqttPayload;
  int i;
  for (i = 0; i < DS18B20_Count; i++) {
    //MQTT-Botschaften
    //JSON
    myDS18B20.getAddress(myDS18B20Address,i);
    Adresse = formatDS18B20Address(myDS18B20Address);
    float tempVal = myDS18B20.getTempCByIndex(i);
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/JSON";
    mqttJson = "{\"ID\":\"" + String(i) + "\"";
    mqttJson += ",\"Temperatur\":\"" + String(tempVal) + "\"";
    mqttJson += ",\"Adresse\":\"(" + Adresse + ")\"";
    if (Adresse == DS18B20_ADDR_WATER)     mqttJson += ",\"Ort\":\"Temperatur Wasser\"}";
    else if (Adresse == DS18B20_ADDR_AIR)  mqttJson += ",\"Ort\":\"Temperatur Luft\"}";
    else                             mqttJson += ",\"Ort\":\"unbekannt\"}";
    if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
    mqttPublishQueue(mqttTopic.c_str(), mqttJson.c_str(),false);
    //Temperatur
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Temperatur";
    mqttPayload = String(tempVal);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
    if (debug > 2) Serial.print("MQTT ID: ");
    if (debug > 2) Serial.println(mqttPayload);
    //ID
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/ID";
    mqttPayload = String(i);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
    if (debug > 2) Serial.print("MQTT Temperatur: ");
    if (debug > 2) Serial.println(mqttPayload);
    //Adresse
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Adresse";
    mqttPublishQueue(mqttTopic.c_str(), Adresse.c_str(),false);
    if (debug > 2) Serial.print("MQTT Adresse: ");
    if (debug > 2) Serial.println(Adresse);
    //Ort
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Ort";
    if (Adresse == DS18B20_ADDR_WATER)     mqttPayload = "Temperatur Wasser";
    else if (Adresse == DS18B20_ADDR_AIR)  mqttPayload = "Temperatur Luft";
    else                             mqttPayload = "unbekannt";
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
    if (debug > 2) Serial.print("MQTT Ort: ");
    if (debug > 2) Serial.println(mqttPayload);
  }
}
// MQTT Wasserhöhe / Distanz Status senden
void printGroundwaterLevelMQTT() {
  String mqttTopic;
  String mqttPayload;
  if (groundwaterLevel != 0) {
    mqttTopic = MQTT_SERIAL_PUBLISH_WATER + String("Grundwasserspiegel_unter_GOK");
    mqttPayload = groundwaterLevel;
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), false);
    if (debug > 2) Serial.print("Grundwasserspiegel unter GOK: ");
    if (debug > 2) Serial.println(mqttPayload);
  }
  //Distanz gemessen
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "Distanz_gemessen";
  mqttPayload = distance;
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), false);
  if (debug > 2) Serial.print("Distanz gemessen: ");
  if (debug > 2) Serial.println(mqttPayload);
  //Distanz Status
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "Distanz_Status";
  mqttPayload = (disStatus != 0) ? "true" : "false";
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), false);
  if (debug > 2) Serial.print("Distanz Status: ");
  if (debug > 2) Serial.println(mqttPayload);
  //Präzision gemessen [mm]
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "Präzision_Distanz";
  mqttPayload = presicion;
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), false);
  if (debug > 2) Serial.print("Präzision Distanz [mm]: ");
  if (debug > 2) Serial.println(mqttPayload);
}
// MQTT Status Betrieb senden
void printStateMQTT() {
  BaseType_t rc;
  String mqttTopic;
  String mqttJson;
  String mqttPayload;
  rc = xSemaphoreTake(mutexLastError, portMAX_DELAY);
  assert(rc == pdPASS);
  String lastErrorSnap = lastError;
  rc = xSemaphoreGive(mutexLastError);
  assert(rc == pdPASS);
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "JSON";
  mqttJson = "{\"WiFi_Signal_Strength\":\"" + ((WiFi.status() == WL_CONNECTED) ? String(WiFi.RSSI()) : String("--")) + "\"";
  mqttJson += ",\"WiFi_IP_Adress\":\"" + WiFi.localIP().toString() + "\"";
  mqttJson += ",\"WiFi_MAC_Adress\":\"" + WiFi.macAddress() + "\"";
  mqttJson += ",\"lastError\":\"" + lastErrorSnap + "\"}";
  if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
  mqttPublishQueue(mqttTopic.c_str(), mqttJson.c_str(),false);
  //lastError
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "lastError";
  mqttPayload = lastErrorSnap;
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("LastError: ");
  if (debug > 2) Serial.println(mqttPayload);
  //WiFi Signalstärke
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "WiFi_Signal_Strength";
  mqttPayload = (WiFi.status() == WL_CONNECTED) ? String(WiFi.RSSI()) : String("--");
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("WiFi Signalstärke: ");
  if (debug > 2) Serial.println(mqttPayload);
  //WiFi IP-Adresse
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "WiFi_IP_Adress";
  mqttPayload = WiFi.localIP().toString();
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("WiFi IP-Adresse: ");
  if (debug > 2) Serial.println(mqttPayload);
  //WiFi MAC-Adresse
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "WiFi_MAC_Adress";
  mqttPayload = WiFi.macAddress();
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("WiFi MAC-Adresse: ");
  if (debug > 2) Serial.println(mqttPayload);
}
// MQTT Config und Parameter senden
void printConfigMQTT() {
  String mqttTopic;
  String mqttJson;
  //Teil 1
  mqttTopic = MQTT_SERIAL_PUBLISH_CONFIG;
  mqttTopic += "JSON_0";
  mqttJson = "{\"GOK_Offset\":\"" + String(GOK_OFFSET) + "\"}";
  if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
  mqttPublishQueue(mqttTopic.c_str(), mqttJson.c_str(),false);
}
//LED-Blik-OK
void LEDblinkMSG(){
  digitalWrite(LED_MSG, HIGH);
  delay(150);
  digitalWrite(LED_MSG, LOW);
}
//-------------------------------------
//MQTT-Status-Task
static void MQTTstate(void* args) {
  BaseType_t rc;
  esp_err_t er;
  TickType_t ticktime;

  //ticktime initialisieren
  ticktime = xTaskGetTickCount();

  er = esp_task_wdt_add(NULL);
  assert(er == ESP_OK);

  for (;;) {                        // Dauerschleife des Tasks
    esp_task_wdt_reset();
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | MQTT-Status-Task gestartet");
    rc = xSemaphoreTake(mutexMQTT, portMAX_DELAY);
    assert(rc == pdPASS);
    bool mqttConn = mqttClient.connected();
    rc = xSemaphoreGive(mutexMQTT);
    assert(rc == pdPASS);
    if (mqttConn) {
      rc = xSemaphoreTake(mutexTemp, portMAX_DELAY);
      assert(rc == pdPASS);
        printDS18B20MQTT();
      rc = xSemaphoreGive(mutexTemp);
      assert(rc == pdPASS);

      rc = xSemaphoreTake(mutexTOF, portMAX_DELAY);
      assert(rc == pdPASS);
        printGroundwaterLevelMQTT();
      rc = xSemaphoreGive(mutexTOF);
      assert(rc == pdPASS);

      printStateMQTT();

      printConfigMQTT();
    }

    if (debug > 1) Serial.println("Stack frei MQTTstate: " + String(uxTaskGetStackHighWaterMark(NULL) * 4) + " Bytes");

    // Task schlafen legen - restart MQTTStateRefresh ticks
    LEDblinkMSG();
    vTaskDelayUntil(&ticktime, MQTTStateRefresh);
  }
}

//-------------------------------------
//Subfunktionen für MQTTwatchdog-Task
// MQTT Verbindung herstellen (wird auch von setup verwendet!)
void mqttConnect() {
  int i = 0;
  // Sicherstellen dass WiFi verbunden ist bevor MQTT-Verbindung versucht wird
  int wifiWait = 0;
  while (WiFi.status() != WL_CONNECTED) {
    if (++wifiWait > 60) {
      Serial.println("WiFi nicht erreichbar! Reboot!!");
      safeReset();
    }
    Serial.print("W");
    esp_task_wdt_reset();
    delay(1000);
  }
  Serial.print("Verbindungsaufbau zu MQTT Server ");
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
    } else {
      if (++i > 20) {
        Serial.println("MQTT scheint nicht mehr erreichbar! Reboot!!");
        safeReset();
      }
      Serial.print("fehlgeschlagen rc=");
      Serial.print(mqttClient.state());
      Serial.println(" erneuter Versuch in 5 Sekunden.");
      esp_task_wdt_reset();
      delay(5000);
    }
  }
  mqttClient.subscribe(MQTT_SERIAL_RECEIVER_COMMAND);
}
// MQTT Verbindungsprüfung 
void checkMQTTconnetion() {
  String mqttTopic;
  String mqttPayload;
  if (!mqttClient.connected()) {
    if (debug) Serial.println("MQTT Server Verbindung verloren...");
    if (debug) Serial.print("Disconnect Errorcode: ");
    if (debug) Serial.println(mqttClient.state());
    //Vorbereitung errorcode MQTT (https://pubsubclient.knolleary.net/api#state)
    mqttTopic = MQTT_SERIAL_PUBLISH_BASIS + String("error");
    mqttPayload = String(String(++MQTTReconnect) + ". reconnect: ") + String("; MQTT disconnect rc=" + String(mqttClient.state()));
    // 0	MQTT_CONNECTED	        Erfolgreich verbunden.
    // 1	MQTT_CONNECTION_TIMEOUT	Verbindung zum Broker hat zu lange gedauert (Timeout).
    // 2	MQTT_CONNECTION_LOST	  Verbindung ging verloren (nach dem Connect).
    // 3	MQTT_CONNECT_FAILED	    Verbindung konnte nicht hergestellt werden (Socket fehlerhaft).
    // 4	MQTT_DISCONNECTED	      Client ist aktuell nicht verbunden.
    // 5	MQTT_CONNECTED_FAILED	  Broker hat die Verbindung abgelehnt (z. B. Authentifizierung)
    //reconnect
    mqttConnect();
    //sende Fehlerstatus (retain=true: Broker speichert letzten Fehler)
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), true);
    //reconnect zurückmelden
    mqttTopic = MQTT_SERIAL_PUBLISH_BASIS + String("ac");
    mqttPayload = String("MQTT reconnect durchgeführt!");
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), false);
  }
  // kein mqttClient.loop() hier - wird in mqttSender unter mutexMQTT aufgerufen
}
//-------------------------------------
//MQTT-MQTTwatchdog-Task
static void MQTTwatchdog(void* args) {
  BaseType_t rc;
  esp_err_t er;
  TickType_t ticktime;

  //ticktime initialisieren
  ticktime = xTaskGetTickCount();

  er = esp_task_wdt_add(NULL);  // Task zur Überwachung hinzugefügt
  assert(er == ESP_OK);

  for (;;) {                        // Dauerschleife des Tasks
    // Watchdog zurücksetzen
    esp_task_wdt_reset();
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | MQTTonlinePrüf-Task gestartet");
    rc = xSemaphoreTake(mutexMQTT, portMAX_DELAY);
    assert(rc == pdPASS);
    checkMQTTconnetion();
    rc = xSemaphoreGive(mutexMQTT);
    assert(rc == pdPASS);

    if (debug > 1)
      Serial.println("Stack frei MQTTwatchdog: " + String(uxTaskGetStackHighWaterMark(NULL) * 4) + " Bytes");

    // Task schlafen legen - restart alle 2s = 2*1000 ticks = 2000 ticks
    // mit mqttClient.loop() wird auch der MQTTcallback ausgeführt!
    vTaskDelayUntil(&ticktime, 2000);
  }
}

//-------------------------------------
//MQTT-MQTTSender-Task
static void mqttSender(void* args) {
  MqttJob job;
  BaseType_t rc;
  esp_err_t er;
  TickType_t ticktime;

  //ticktime initialisieren
  ticktime = xTaskGetTickCount();

  er = esp_task_wdt_add(NULL);  // Task zur Überwachung hinzugefügt
  assert(er == ESP_OK);

  for (;;) {                        // Dauerschleife des Tasks
    // Watchdog zurücksetzen - vor Mutex-Take, damit WDT auch bei langer Wartezeit bedient wird
    // WDT-sicheres Warten auf Mutex — verhindert WDT-Timeout wenn MQTTwatchdog
    // den Mutex während eines Reconnects (~160s) hält
    while (xSemaphoreTake(mutexMQTT, pdMS_TO_TICKS(5000)) != pdPASS) {
      esp_task_wdt_reset();
    }
    esp_task_wdt_reset();
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | MQTT-Sender-Task gestartet");
    if (mqttClient.connected()) {
      // Sendebereit -> MQTT-Queue kann geleert werden
      while (xQueueReceive(mqttQueue, &job, 0) == pdPASS) {
        mqttClient.publish(job.topic, job.payload, job.retain);
        if (debug > 2) Serial.print("Topic: ");
        if (debug > 2) Serial.println(job.topic);
        if (debug > 2) Serial.print("Payload: ");
        if (debug > 2) Serial.println(job.payload);
        if (debug > 2) Serial.print("retain: ");
        if (debug > 2) Serial.println(job.retain);
      }
    }
    mqttClient.loop();  // Keepalive
    rc = xSemaphoreGive(mutexMQTT);
    assert(rc == pdPASS);

    if (debug > 1) Serial.println("Stack frei mqttSender: " + String(uxTaskGetStackHighWaterMark(NULL) * 4) + " Bytes");

    // Task schlafen legen - restart alle 0.5s = 0.5*1000 ticks = 500 ticks
    // mit mqttClient.loop() wird auch der MQTTcallback ausgeführt!
    vTaskDelayUntil(&ticktime, 500);
  }
}
//MQTT-Queue befüllen
bool mqttPublishQueue(const char* topic, const char* payload, bool retain = false) {
  MqttJob job;
  strncpy(job.topic, topic, sizeof(job.topic) - 1);           // Absicherung gegen Buffer-Overflow
  job.topic[sizeof(job.topic) - 1] = '\0';                    // garantierte Null-Terminierung
  strncpy(job.payload, payload, sizeof(job.payload) - 1);     // Absicherung gegen Buffer-Overflow
  job.payload[sizeof(job.payload) - 1] = '\0';                // garantierte Null-Terminierung
  job.retain = retain;
  return xQueueSend(mqttQueue, &job, MQTT_QUEUEMAXWAITTIME) == pdPASS;
}

//-------------------------------------
//Subfunktionen für den TempSensor-Task
// Formatiert eine DS18B20-Geräteadresse als lesbaren Hex-String
String formatDS18B20Address(const DeviceAddress addr) {
  String result = "";
  for (uint8_t j = 0; j < 8; j++) {
    result += "0x";
    if (addr[j] < 0x10) result += "0";
    result += String(addr[j], HEX);
    if (j < 7) result += ", ";
  }
  return result;
}
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
  BaseType_t rc;
  String mqttTopic;
  String mqttPayload;
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
    Adresse = formatDS18B20Address(myDS18B20Address);
    if (Adresse == DS18B20_ADDR_WATER) {
      tWater = myDS18B20.getTempCByIndex(i);
    } else if (Adresse == DS18B20_ADDR_AIR) {
      tAir = myDS18B20.getTempCByIndex(i);
    } else {
      mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
      mqttTopic += "lastError";
      mqttPayload = "nicht spezifizierter Temperatursensor gefunden! Reboot! (Adresse: " + Adresse + ")";
      rc = xSemaphoreTake(mutexLastError, portMAX_DELAY);
      assert(rc == pdPASS);
      lastError = mqttPayload;
      rc = xSemaphoreGive(mutexLastError);
      assert(rc == pdPASS);
      mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), true);
      if (debug > 2) Serial.print("LastError: ");
      if (debug > 2) Serial.println(mqttPayload);
      delay(500);
      safeReset();
    }
  }
  //Plausibilitätscheck
  if (checkDS18B20Value(tWater)) {
    tempWater = tWater;
    res1 = true;
  }
  else {
    tempTSensorFail = tempTSensorFail + 1;
    res1 = false;
    mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
    mqttTopic += "lastError";
    mqttPayload = "Temperatursensor TWater außerhalb des Messbereichts: " + String(tWater) + "[C]; Wiederholung: " + String(tempTSensorFail);
    rc = xSemaphoreTake(mutexLastError, portMAX_DELAY);
    assert(rc == pdPASS);
    lastError = mqttPayload;
    rc = xSemaphoreGive(mutexLastError);
    assert(rc == pdPASS);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), true);
    if (debug > 2) Serial.print("LastError: ");
    if (debug > 2) Serial.println(mqttPayload); //(debug > 2)
  }
  if (checkDS18B20Value(tAir)) {
    tempAir = tAir;
    res2 = true;
    if (res1 && res2) tempTSensorFail = 0;
  }
  else {
    tempTSensorFail = tempTSensorFail + 1;
    res2 = false;
    mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
    mqttTopic += "lastError";
    mqttPayload = "Temperatursensor TAir außerhalb des Messbereichts: " + String(tAir) + "[C]; Wiederholung: " + String(tempTSensorFail);
    rc = xSemaphoreTake(mutexLastError, portMAX_DELAY);
    assert(rc == pdPASS);
    lastError = mqttPayload;
    rc = xSemaphoreGive(mutexLastError);
    assert(rc == pdPASS);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), true);
    if (debug > 2) Serial.print("LastError: ");
    if (debug > 2) Serial.println(mqttPayload);
  }
  if (tempTSensorFail > maxTSensorFail) {
    Serial.println("zu viele Fehler (out of range) beim Auslesen der DS18B20! Reboot!!");
    safeReset();
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
      Serial.println(formatDS18B20Address(myDS18B20Address) + ")");
    }
  }
}

//-------------------------------------
// Task zur Ermittlung der Temperaturen
static void getTempFromSensor(void* args) {
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
    rc = xSemaphoreTake(mutexTemp, portMAX_DELAY);
    assert(rc == pdPASS);
      readDS18B20();                // Sensoren auslesen und den Variablen zuordnen
      printDS18B20();               // DebugInfo auf Serial (thermale Infos)
    rc = xSemaphoreGive(mutexTemp);
    assert(rc == pdPASS);

    if (debug > 1)
      Serial.println("Stack frei getTempFromSensor: " + String(uxTaskGetStackHighWaterMark(NULL) * 4) + " Bytes");

    // Task schlafen legen - restart alle 5s = 5*1000 ticks = 5000 ticks
    vTaskDelayUntil(&ticktime, 5000);
  }
}

//-------------------------------------
//Subfunktionen für die Abstandsmessung
// Distanzsensorenwerte auslesen
void readDistance() {
  BaseType_t rc;
  bool commOK = true;
  String commError = "";

  // Register 0x24 = Distanz
  Wire.beginTransmission(I2C_ADDRESS);
  Wire.write(0x24);
  Wire.endTransmission();
  Wire.requestFrom(I2C_ADDRESS, 2);
  if (Wire.available() == 2) {
    uint8_t lo = Wire.read();
    uint8_t hi = Wire.read();
    distance = (uint16_t)lo | ((uint16_t)hi << 8);
  } else {
    distance = 0;
    commOK = false;
    commError += "0x24(Distanz) ";
  }

  // Register 0x28 = Distanz Status
  Wire.beginTransmission(I2C_ADDRESS);
  Wire.write(0x28);
  Wire.endTransmission();
  Wire.requestFrom(I2C_ADDRESS, 1);
  if (Wire.available() == 1) {
    disStatus = Wire.read();
  } else {
    disStatus = 0;
    commOK = false;
    commError += "0x28(Status) ";
  }

  // Register 0x2C = Präzision
  Wire.beginTransmission(I2C_ADDRESS);
  Wire.write(0x2C);
  Wire.endTransmission();
  Wire.requestFrom(I2C_ADDRESS, 1);
  if (Wire.available() == 1) {
    presicion = Wire.read();
  } else {
    presicion = 0;
    commOK = false;
    commError += "0x2C(Präzision) ";
  }

  // Fehlerzähler und Grundwasserspiegel
  if (!commOK) {
    tofFailCount = tofFailCount + 1;
    String mqttTopic = MQTT_SERIAL_PUBLISH_STATE; mqttTopic += "lastError";
    String mqttPayload = "TOF I2C Lesefehler Register: " + commError + "Fehleranzahl: " + String(tofFailCount);
    rc = xSemaphoreTake(mutexLastError, portMAX_DELAY);
    assert(rc == pdPASS);
    lastError = mqttPayload;
    rc = xSemaphoreGive(mutexLastError);
    assert(rc == pdPASS);
    Serial.println(mqttPayload);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), true);
    if (tofFailCount > maxTofFail) {
      Serial.println("zu viele TOF I2C Fehler! Reboot!!");
      safeReset();
    }
  } else {
    tofFailCount = 0;
    if (disStatus != 0) {
      groundwaterLevel = ((float)distance / 1000.0) + GOK_OFFSET;
    }
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
static void getDistanceFromSensor(void* args) {
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
    rc = xSemaphoreTake(mutexTOF, portMAX_DELAY);
    assert(rc == pdPASS);
      readDistance();                    // Sensoren auslesen und den Variablen zuordnen
      printDistance();                   // DebugInfo auf Serial (Distanz-Infos)
    rc = xSemaphoreGive(mutexTOF);
    assert(rc == pdPASS);

    if (debug > 1)
      Serial.println("Stack frei getDistanceFromSensor: " + String(uxTaskGetStackHighWaterMark(NULL) * 4) + " Bytes");

    // Task schlafen legen - restart alle DistanceRefresh Ticks
    vTaskDelayUntil(&ticktime, DistanceRefresh);
  }
}

void setup() {
  // WDT sofort auf 5 Minuten setzen - verhindert WDT-Reset während langer Init-Phasen (WiFi, MQTT)
  const esp_task_wdt_config_t wdt_config = {.timeout_ms = 300000, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt_config);

  // Initialisierung und Plausibilitaetschecks
  Serial.begin(115200);
  delay(100);                                     // kurze Stabilisierungspause
  while (!Serial) Serial.println("Start Setup");
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
  Wire.setClock(100000);              // 100kHz I2C-Takt
  delay(TOF_BOOT_DELAY);              // warte auf den Sensorboot
  //Scanne I2C nach Geräten
  Serial.println("\nI2C Scanner");
  uint8_t pieces = 0;
  bool tofFound = false;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      pieces++;
      Serial.print("I2C device found at address 0x");
      Serial.println(address, HEX);
      if (address == I2C_ADDRESS) tofFound = true;
    }
  }
  Serial.print("Scan beendet. Gefundene Geräte: ");
  Serial.print(pieces);
  Serial.println(".");
  if (!tofFound) {
    Serial.println("FEHLER: TOF-Sensor nicht gefunden! (I2C 0x"
      + String(I2C_ADDRESS, HEX) + ", SDA=" + String(SDA_PIN) + ", SCL=" + String(SCL_PIN) + ")");
    digitalWrite(LED_OK, LOW);
    uint8_t loopReset = 0;
    while (true) {
      digitalWrite(LED_ERROR, HIGH); delay(250);
      digitalWrite(LED_ERROR, LOW);  delay(250);
      if (++loopReset >= 3) safeReset();
    }
  }
  //WiFi-Setup
  int i = 0;
  Serial.print("Verbindungsaufbau zu ");
  Serial.print(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    if (++i > 240) {
      // Reboot nach 2min der Fehlversuche - safeReset() nicht verwenden, Mutexe noch nicht initialisiert
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
  // Event-Handler erst nach erfolgreichem Connect binden — feuert nur bei späteren Verbindungsänderungen
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) Serial.println("WiFi: Verbindung verloren.");
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) Serial.println("WiFi: Verbindung wiederhergestellt.");
  });
  //MQTT-Setup
  String mqttTopic;
  String mqttPayload;
  Serial.println("MQTT Server Initialisierung laeuft...");
  mqttClient.setServer(MQTT_SERVER,MQTT_PORT); 
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(BWS_MQTT_KEEPALIVE);
  mqttClient.setSocketTimeout(BWS_MQTT_SOCKETTIMEOUT);
  mqttConnect();
  mqttTopic = MQTT_SERIAL_PUBLISH_BASIS + String("error");
  mqttPayload = String(String(MQTTReconnect) + ".: keine MQTT-Fehler seit Reboot!");
  mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str(), true);  // retain=true; direkt - mqttQueue existiert noch nicht
  Serial.println("");
  //DS18B20-Setup
  Serial.println("Auslesen der DS18B20-Sensoren...");
  myDS18B20.begin();
  Serial.print("Anzahl gefundener 1-Wire-Geraete:  ");
  Serial.println(myDS18B20.getDeviceCount());
  DS18B20_Count = myDS18B20.getDS18Count();
  Serial.print("Anzahl gefundener DS18B20-Geraete: ");
  Serial.println(DS18B20_Count);
  if (DS18B20_Count < 2) {
    Serial.println("... Anzahl DB18B20 < 2 => zu wenig! ... System angehalten!");
    digitalWrite(LED_OK, LOW);
    uint8_t loopReset = 0;
    while (true) {
      //blinke 3x... dann reboot
      digitalWrite(LED_ERROR, HIGH);
      delay(250);
      digitalWrite(LED_ERROR, LOW);
      delay(250);
      if (++loopReset >= 3) {
        safeReset();
      }
    }
  }
  // Prüfung: sind beide konfigurierten Adressen unter den gefundenen Sensoren?
  bool foundWater = false, foundAir = false;
  for (int i = 0; i < DS18B20_Count; i++) {
    myDS18B20.getAddress(myDS18B20Address, i);
    String adresse = formatDS18B20Address(myDS18B20Address);
    if (adresse == DS18B20_ADDR_WATER) foundWater = true;
    if (adresse == DS18B20_ADDR_AIR)   foundAir   = true;
  }
  if (!foundWater || !foundAir) {
    Serial.println("DS18B20-Adressfehler! Erwartete Adressen nicht gefunden:");
    if (!foundWater) Serial.println("  FEHLT Water: " + String(DS18B20_ADDR_WATER));
    if (!foundAir)   Serial.println("  FEHLT Air:   " + String(DS18B20_ADDR_AIR));
    Serial.println("Gefundene Adressen:");
    for (int i = 0; i < DS18B20_Count; i++) {
      myDS18B20.getAddress(myDS18B20Address, i);
      Serial.println("  DS18B20[" + String(i) + "]: " + formatDS18B20Address(myDS18B20Address));
    }
    digitalWrite(LED_OK, LOW);
    uint8_t loopReset = 0;
    while (true) {
      digitalWrite(LED_ERROR, HIGH); delay(250);
      digitalWrite(LED_ERROR, LOW);  delay(250);
      if (++loopReset >= 3) safeReset();
    }
  }
  myDS18B20.setResolution(DS18B20_RESOLUTION);
  Serial.print("Globale Aufloesung (Bit):        ");
  Serial.println(myDS18B20.getResolution());
  myDS18B20.requestTemperatures();
  delay(DS18B20_DELAY);
  if (debug > 0){
    for (int i = 0; i < DS18B20_Count; i++) {
      Serial.print("DS18B20[");
      Serial.print(i);
      Serial.print("]: ");
      Serial.print(myDS18B20.getTempCByIndex(i));
      Serial.print(" *C (");
      myDS18B20.getAddress(myDS18B20Address,i);
      Serial.println(formatDS18B20Address(myDS18B20Address) + ")");
    }
  }
  //Mutex-Initialisierung
  mutexTemp = xSemaphoreCreateMutex();
  assert(mutexTemp);
  mutexTOF = xSemaphoreCreateMutex();
  assert(mutexTOF);
  mutexMQTT = xSemaphoreCreateMutex();
  assert(mutexMQTT);
  mutexLastError = xSemaphoreCreateMutex();
  assert(mutexLastError);
  Serial.println("Mutex-Einrichtung erforlgreich.");
  //Queue für MQTT anlegen
  mqttQueue = xQueueCreate(MQTT_QUEUEDEPTH, sizeof(MqttJob));
  assert(mqttQueue);
  //Tasks starten
  int app_cpu = xPortGetCoreID();
  BaseType_t rc;
  rc = xTaskCreatePinnedToCore(
    mqttSender,                 //Taskroutine
    "MQTTSenderTask",           //Taskname
    3072,                       //StackSize
    nullptr,                    //Argumente / Parameter
    4,                          //Priorität
    &hmqtt,                     //handler
    app_cpu);                   //CPU_ID
  assert(rc == pdPASS);
  Serial.println("MQTT Sendertask gestartet.");
  rc = xTaskCreatePinnedToCore(
    getTempFromSensor,         //Taskroutine
    "getTempSensorTask",       //Taskname
    4096,                      //StackSize
    nullptr,                   //Argumente / Parameter
    2,                         //Priorität
    &htempSensor,              //handler
    app_cpu);                  //CPU_ID
  assert(rc == pdPASS);
  Serial.println("TempSensor-Task gestartet.");
  rc = xTaskCreatePinnedToCore(
    MQTTwatchdog,              //Taskroutine
    "MQTTwatchdog",            //Taskname
    4096,                      //StackSize
    nullptr,                   //Argumente / Parameter
    1,                         //Priorität
    &hMQTTwatchdog,            //handler
    app_cpu);                  //CPU_ID
  assert(rc == pdPASS);
  Serial.println("MQTT-Watchdog-Task gestartet.");
  rc = xTaskCreatePinnedToCore(
    MQTTstate,                 //Taskroutine
    "MQTTstate",               //Taskname
    6144,                      //StackSize
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
  mqttPublishQueue(mqttTopicAC.c_str(), "Start durchgeführt.",false);
}

void loop() {
  //loop wird als Task nicht gebraucht
  vTaskDelete(nullptr);
}
