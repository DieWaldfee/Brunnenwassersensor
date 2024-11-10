# Brunnenwassersensor
to measure groundwater level and temperature

Der Brunnenwassersensor sitzt in einem KG-Rohr DN50 (Abwasserrohr) und misst per Laser die Wasserhöhe, die Wassertemperatur und die Lufttemperatur im Brunnenschacht. Die Konstruktion ist für ein 3.5m langes Brunnenrohr ausgelegt, sodass die normale Grundwasseränderung (Sommer/Winter) wie auch die dynamische Grundwasserabsenkung im Brunnen bei Betrieb einer Brunnenpumpe erfasst werden kann. Übertragen werden die Messdaten via MQTT an einen MQTT-Broker. So kann der Messwert in einer Hausautomation erfasst und dargestellt werden.

Wozu braucht man das: keine Ahnung, aber es geht :-)
Scherz beiseite: Messung der dynamischen Wasserabsenkung bei Einsatz einer Brunnenpumpe, Messung des saisonalen Grundwasserspiegels und dessen Temperatur.

Aufgebaut ist die Elektronik mit einem ESP32, zwei DS18B20 Temperatursensoren und einem Waveshare TOF Laser Sensor B. Der Lasersensor ist besonders, da er eine Laserfokussierung von 1-2° hat - ideal, um im dünnen KG-Rohr DN50 zu messen!
Hier der Link zum Sensor-Datasheet: https://www.waveshare.com/wiki/TOF_Laser_Range_Sensor_(B)
Um den Sensor korrekt zu zentrieren nehmt ihr euer Handy mit dessen Kamera. Die CCDs sehen den Laser - das Auge kann den Laser nicht direkt sehen. => Linse mittig am Ende des Rohres positionieren / halten. Sensor in seiner Halterung ausrichten - fertig.

Im Repository liegen die STL-Dateien, um die Komponenten mit einem 3D-Drucker auszudrucken, der C++-Code für den ESP32, das Eagle-file für das Platinendesign auf Lochrasterkarte (zum Nachlöten) und die Fusiom360-Dateien.

Einrichtung:
Einstellung für euer eingesetzten DS18B20-Sensoren - die haben natürlich andere Adressen...
![grafik](https://github.com/user-attachments/assets/56b18717-67ae-4c47-8625-beab1ef32bdd)
Offset-Einstellung zwischen Oberkante Sensor und Oberkante Gelände (GOK):
![grafik](https://github.com/user-attachments/assets/dca3b6ed-e2bb-40b2-8c95-7d2f8bebc820)
...und danach natürlich eure User/Passwörter in der secrets.h:<p>
![grafik](https://github.com/user-attachments/assets/ab2d421e-74e0-4cce-a1cb-a920563d0784)

viel Spaß beim Nachbauen :-)

![grafik](https://github.com/user-attachments/assets/f121caf4-d01a-4349-abe1-66f7afae5f39)

Ausgabe im serial Monitor unter debug=1:
![grafik](https://github.com/user-attachments/assets/d68d20de-d053-4121-8ac8-8014c8adbd25)

Aufbau des Sonsorrohrs:
![20241110_125235](https://github.com/user-attachments/assets/3688fb63-cdaa-4906-afc4-edb28c99e2fc)
![20241110_125336](https://github.com/user-attachments/assets/b8728986-d2a6-4608-98a4-d24b29194bf9)
![20241110_133428](https://github.com/user-attachments/assets/8a31be5b-f2f9-43b2-a917-cb40a6633b5d)
![20241110_130808](https://github.com/user-attachments/assets/ddf1ea04-8d0c-4330-ba61-a4a08efc7992)
![20241110_133442](https://github.com/user-attachments/assets/a945720c-713f-4ac1-8442-f2923508358f)
