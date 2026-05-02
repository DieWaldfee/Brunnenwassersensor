// hardware.h
#pragma once

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  KALIBRIERUNGSWERTE                                                       ║
// ║  Nach Sensorwechsel oder Neukalibrierung anpassen.                        ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

// ─── DS18B20-Sensoradressen ───────────────────────────────────────────────────
// Adresse per Debug-Modus (debug=1) auf dem Serial Monitor ermitteln:
//   DS18B20[n]: <Temp> °C (<Adresse>)
// DS18B20_ADDR_WATER: Sensor im Brunnen unter Wasser – Wassertemperatur
// DS18B20_ADDR_AIR:   Sensor oberhalb des Wasserspiegels – Lufttemperatur
#define DS18B20_ADDR_WATER "0x28, 0xd4, 0x28, 0x43, 0xd4, 0x25, 0x6a, 0x0a"
#define DS18B20_ADDR_AIR   "0x28, 0x9b, 0xa2, 0x57, 0x04, 0xe1, 0x3c, 0x8b"

// ─── TOF-Sensor: Installationsoffset ─────────────────────────────────────────
// Distanz zwischen GOK (GeländeOberKante) und dem mechanischen 0-Punkt des
// Messsystems (Montageposition des TOF-Sensors) [m].
// Nach Einbau/Umbau neu vermessen und anpassen.
#define GOK_OFFSET 2.50

// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Pinning und Hardwareconfig                                                ║
// ║  DO NOT MODIFY UNLESS YOU KNOW WHAT YOU ARE DOING                         ║
// ╚════════════════════════════════════════════════════════════════════════════╝

// ─── GPIO-Pinbelegung ─────────────────────────────────────────────────────────
#define LED_ERROR    23
#define LED_MSG       4
#define LED_OK       19
#define ONE_WIRE_BUS 25

// ─── TOF-Sensor via I2C ───────────────────────────────────────────────────────
#define I2C_ADDRESS    0x08  // I2C-Adresse des TOF-Sensors (Werkseinstellung)
#define SDA_PIN        21
#define SCL_PIN        22
#define TOF_BOOT_DELAY 50    // Wartezeit nach Power-Up bis Sensor bereit [ms]

// ─── DS18B20-Sensor-Konfiguration ─────────────────────────────────────────────
// 9 bit: ±0,5 °C / 93,75 ms  | 10 bit: ±0,25 °C / 187,5 ms
// 11 bit: ±0,125 °C / 375 ms | 12 bit: ±0,0625 °C / 750 ms
#define DS18B20_RESOLUTION 12
#define DS18B20_DELAY     752  // Wartezeit nach getriggerter Messung [ms]
