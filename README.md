# AquaPrecise ESP32 Wettersteuerung

Stoppt die Gardena AquaPrecise automatisch via BLE wenn Regen vorhergesagt ist.

## Wie es funktioniert

Der ESP32 wacht täglich kurz vor der geplanten Bewässerungszeit auf, fragt die [Open-Meteo API](https://open-meteo.com) nach dem Niederschlag ab und sendet bei Regen einen BLE-Stop-Befehl an die AquaPrecise. Danach schläft er wieder bis zum nächsten Tag (Deep Sleep).

## Hardware

- ESP32 (z.B. ESP32 DevKit V1)
- Gardena AquaPrecise Bewässerungscomputer

## Voraussetzungen

- [arduino-cli](https://arduino.github.io/arduino-cli/) oder Arduino IDE
- ESP32 Board Support (`esp32:esp32`)
- Libraries:
  - [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) v2.5.0
  - [ArduinoJson](https://arduinojson.org/) v7.x

## Einrichtung

1. **MAC-Adresse ermitteln:** `AquaPrecise_Scanner` flashen, serielle Ausgabe lesen, MAC notieren
2. **Konfiguration anpassen:** Platzhalter im Sketch ersetzen (siehe Tabelle unten)
3. **Wettersteuerung flashen:** `AquaPrecise_Wettersteuerung` auf den ESP32 laden
4. **Einmaliges Pairing:** LED blinkt langsam → AquaPrecise-Knopf 3 Sekunden halten → 5x schnelles Blinken = Erfolg

Der Bond wird im NVS-Flash gespeichert und überlebt weitere Firmware-Updates.

## Konfiguration

Vor dem Flashen im Sketch anpassen:

| Variable | Beschreibung |
|---|---|
| `DEIN_WLAN_NAME` | Name deines WLAN-Netzwerks |
| `DEIN_WLAN_PASSWORT` | WLAN-Passwort |
| `xx:xx:xx:xx:xx:xx` | Bluetooth-MAC der AquaPrecise (mit `AquaPrecise_Scanner` ermitteln) |
| `LATITUDE / LONGITUDE` | GPS-Koordinaten deines Standorts — z.B. über [maps.google.com](https://maps.google.com) ermitteln |
| `BEWAESSERUNG_HOUR/MINUTE` | Uhrzeit wann deine AquaPrecise normalerweise startet |
| `REGEN_SCHWELLE_MM` | Ab wieviel mm Regen die Bewässerung gestoppt wird (Standard: 3.0) |

## Flash-Befehl

```bash
arduino-cli upload --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs" --port PORT AquaPrecise_Wettersteuerung
```

Port je nach Betriebssystem:
- macOS: `/dev/tty.usbserial-XXXX`
- Linux: `/dev/ttyUSB0`
- Windows: `COM3` (o.ä.)

## Bekannte Tücken

- **Falsche Service-UUID:** Die AquaPrecise bewirbt beim BLE-Scan `98bd0001-...`, der tatsächliche GATT-Service ist `98bd0d10-0b0e-421a-84e5-ddbf75dc6de4`
- **NimBLE v2.x:** `setConnectTimeout()` erwartet Millisekunden (nicht Sekunden wie in v1.x)
- **Partitionsschema:** Standard-Schema ist zu klein für WiFi + BLE gleichzeitig — `min_spiffs` verwenden

## LED-Signale

| Signal | Bedeutung |
|---|---|
| Langsames Blinken (1s) | Pairing nötig — Knopf halten! |
| 5x schnell | Pairing erfolgreich |
| 1x kurz | WLAN verbunden |
| 2x kurz | BLE-Stop gesendet |
| 20x sehr schnell | Fehler |
