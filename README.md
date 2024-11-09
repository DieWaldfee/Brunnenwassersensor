# Brunnenwassersensor
to measure groundwater level and temperature

Der Brunnenwassersensor sitzt in einem KG-Rohr DN50 (Abwasserrohr) und misst per Laser die Wasserhöhe, die Wassertemperatur und die Lufttemperatur im Brunnenschacht. Die Konstruktion ist für ein 3.5m langes Brunnenrohr ausgelegt, sodass die normale Grundwasseränderung (Sommer/Winter) wie auch die dynamische Grundwasserabsenkung im Brunnen bei Betrieb einer Brunnenpumpe erfasst werden kann. Übertragen werden die Messdaten via MQTT an einen MQTT-Broker. So kann der Messwert in einer Hausautomation erfasst und dargestellt werden.

Wozu braucht man das: keine Ahnung, aber es geht :-)

Aufgebaut ist die Elektronik mit einem ESP32, zwei DS18B20 Temperatursensoren und einem Waveshare TOF Laser Sensor B. Der Lasersensor ist besonders, da er eine Laserfokussierung von 1-2° hat - ideal, um im dünnen KG-Rohr DN50 zu messen!
Hier der Link zum Sensor-Datasheet: https://www.waveshare.com/wiki/TOF_Laser_Range_Sensor_(B)
Um den Sensor korrekt zu zentrieren nehmt ihr euer Handy mit dessen Kamera. Die CCDs sehen den Laser - das Auge kann den Laser nicht direkt sehen. => Linse mittig am Ende des Rohres positionieren / halten. Sensor in seiner Halterung ausrichten - fertig.

Im Repository liegen die STL-Dateien, um die Komponenten mit einem 3D-Drucker auszudrucken, der C++-Code für den ESP32, das Eagle-file für das Platinendesign auf Lochrasterkarte (zum Nachlöten) und die Fusiom360-Dateien.

viel Spaß beim Nachbauen :-)

![grafik](https://github.com/user-attachments/assets/f121caf4-d01a-4349-abe1-66f7afae5f39)
