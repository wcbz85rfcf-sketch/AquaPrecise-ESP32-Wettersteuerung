/*
 * AquaPrecise Wettersteuerung (mit integriertem Pairing)
 * ======================================================
 * LED-Signale:
 *   Langsames Blinken (1s an/aus) = Pairing nötig
 *     → AquaPrecise-Knopf 3 Sek halten, sobald LED blinkt!
 *   5x schnell                    = Pairing erfolgreich
 *   1x kurz                       = WLAN verbunden
 *   2x kurz                       = BLE-Stop gesendet
 *   20x sehr schnell              = Fehler
 *
 * TESTMODUS: auf true setzen → sofortiger BLE-Reconnect-Test (kein Wettercheck)
 * Nach erfolgreichem Test wieder auf false setzen!
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <time.h>

// ============================================================
//   KONFIGURATION
// ============================================================
#define TESTMODUS false  // true = BLE-Reconnect sofort testen (für Produktion: false)

const char* WIFI_SSID           = "DEIN_WLAN_NAME";
const char* WIFI_PASSWORD       = "DEIN_WLAN_PASSWORT";
const char* AQUAPRECISE_MAC     = "xx:xx:xx:xx:xx:xx";  // MAC mit AquaPrecise_Scanner ermitteln
const int   BEWAESSERUNG_HOUR   = 20;
const int   BEWAESSERUNG_MINUTE = 30;
const float REGEN_SCHWELLE_MM   = 3.0;
const float LATITUDE            = 0.0000;  // GPS-Breitengrad (z.B. über maps.google.com)
const float LONGITUDE           = 0.0000;  // GPS-Längengrad

static const char* SERVICE_UUID    = "98bd0d10-0b0e-421a-84e5-ddbf75dc6de4";
static const char* POWER_CHAR_UUID = "98bd0d11-0b0e-421a-84e5-ddbf75dc6de4";

#define LED_PIN 2

// ============================================================
//   CLIENT-CALLBACKS (Verbindung + Pairing/Bonding)
// ============================================================
static volatile bool gPairingDone    = false;
static volatile bool gPairingSuccess = false;

class MeinClientCallback : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* client) override {
    Serial.println("  onConnect");
  }
  void onDisconnect(NimBLEClient* client, int reason) override {
    Serial.printf("  onDisconnect (Grund: %d)\n", reason);
  }
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    Serial.printf("  PIN %06lu -> auto-OK\n", pin);
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    Serial.println("  Passkey -> 0");
    NimBLEDevice::injectPassKey(connInfo, 0);
  }
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    gPairingDone    = true;
    gPairingSuccess = connInfo.isBonded();
    Serial.printf("  Auth abgeschlossen — Gebondet: %s\n",
                  gPairingSuccess ? "JA" : "NEIN");
  }
};

// ============================================================
//   LED-HELFER
// ============================================================
void blinken(int anzahl, int pause_ms) {
  for (int i = 0; i < anzahl; i++) {
    digitalWrite(LED_PIN, HIGH); delay(pause_ms);
    digitalWrite(LED_PIN, LOW);  delay(pause_ms);
  }
  delay(200);
}

static TaskHandle_t gBlinkTask = NULL;
static int gBlinkPause = 1000;

void blinkTaskFunc(void* param) {
  while (true) {
    digitalWrite(LED_PIN, HIGH);
    vTaskDelay(gBlinkPause / portTICK_PERIOD_MS);
    digitalWrite(LED_PIN, LOW);
    vTaskDelay(gBlinkPause / portTICK_PERIOD_MS);
  }
}

void startHintergrundBlinken(int pause_ms) {
  gBlinkPause = pause_ms;
  if (gBlinkTask == NULL) {
    xTaskCreate(blinkTaskFunc, "blink", 1024, NULL, 1, &gBlinkTask);
  }
}

void stopHintergrundBlinken() {
  if (gBlinkTask != NULL) {
    vTaskDelete(gBlinkTask);
    gBlinkTask = NULL;
    digitalWrite(LED_PIN, LOW);
  }
}

// ============================================================
//   VORWÄRTS-DEKLARATIONEN
// ============================================================
bool verbindeWLAN();
void zeitSynchronisieren();
void wartenBis(int stunde, int minute);
void schlafen();
float heutigeNiederschlagsmenge();
bool stoppeAquaPrecise();
void pairingModus();
bool pairingVersuch();

// ============================================================
//   SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  delay(500);

  Serial.println("\n====================================");
  Serial.println(" AquaPrecise Wettersteuerung");
  Serial.println("====================================");

  // Bond-Status prüfen (Bond liegt in NVS-Flash, überlebt Uploads)
  NimBLEDevice::init("");
  NimBLEAddress peerAddr(AQUAPRECISE_MAC, BLE_ADDR_RANDOM);
  bool bonded = NimBLEDevice::isBonded(peerAddr);
  NimBLEDevice::deinit(false);

  Serial.printf("Bond-Status: %s\n\n", bonded ? "vorhanden" : "NICHT vorhanden");

  if (!bonded) {
    pairingModus();
    return;
  }

  // ---- TESTMODUS: BLE-Reconnect sofort testen ----
#if TESTMODUS
  Serial.println("*** TESTMODUS: BLE-Reconnect ohne Knopfdruck ***");
  Serial.println("(Handy-Bluetooth aus lassen!)");
  bool testOk = stoppeAquaPrecise();
  Serial.println(testOk ? "\n✓ ERFOLG! Reconnect klappt ohne Knopf!"
                        : "\n✗ FEHLER: Reconnect fehlgeschlagen.");
  Serial.println("(Testmodus beendet)");
  while (true) delay(1000);
#endif

  // ---- NORMAL-MODUS ----
  if (!verbindeWLAN()) { schlafen(); return; }
  zeitSynchronisieren();

  struct tm timeinfo;
  getLocalTime(&timeinfo);
  Serial.printf("Aktuelle Zeit: %02d:%02d Uhr\n",
                timeinfo.tm_hour, timeinfo.tm_min);

  float regen = heutigeNiederschlagsmenge();
  if (regen < 0) {
    Serial.println("Wetter-API nicht erreichbar → kein Eingriff");
    schlafen(); return;
  }

  Serial.printf("Niederschlag heute: %.1f mm (Schwelle: %.1f mm)\n",
                regen, REGEN_SCHWELLE_MM);

  if (regen < REGEN_SCHWELLE_MM) {
    Serial.println("→ Kein Regen → Bewässerung läuft normal.");
    schlafen(); return;
  }

  Serial.println("→ Regen erkannt! Starte Schutzsequenz...");

  Serial.println("\n[1/2] Stop-Befehl VOR dem Bewässerungsstart...");
  bool ok1 = stoppeAquaPrecise();
  Serial.println(ok1 ? "Befehl gesendet" : "Nicht erreichbar");

  wartenBis(BEWAESSERUNG_HOUR, BEWAESSERUNG_MINUTE + 1);

  Serial.println("\n[2/2] Stop-Befehl als Sicherheitsnetz...");
  bool ok2 = stoppeAquaPrecise();
  Serial.println(ok2 ? "Befehl gesendet" : "Nicht erreichbar");

  if (!ok1 && !ok2) {
    Serial.println("AquaPrecise war nicht erreichbar!");
    blinken(20, 50);
  }

  schlafen();
}

void loop() {}

// ============================================================
//   PAIRING-MODUS
// ============================================================
void pairingModus() {
  Serial.println("=== PAIRING-MODUS ===");
  Serial.println(">>> AquaPrecise-Knopf 3 Sekunden halten! <<<");
  Serial.println("(LED blinkt langsam — 90 Sekunden Zeit)\n");

  // BLE einmal für die gesamte Pairing-Session initialisieren
  NimBLEDevice::init("");
  NimBLEDevice::setPower(9, NimBLETxPowerType::All);
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  // Alte Bonds löschen für sauberes Pairing
  int n = NimBLEDevice::deleteAllBonds();
  Serial.printf("Alte Bonds gelöscht: %d\n", n);

  startHintergrundBlinken(1000);

  unsigned long deadline = millis() + 90000UL;
  bool erfolg = false;

  while (millis() < deadline && !erfolg) {
    erfolg = pairingVersuch();
  }

  stopHintergrundBlinken();
  NimBLEDevice::deinit(false);  // Einmal am Ende

  if (erfolg) {
    Serial.println("\nPairing erfolgreich! Bond gespeichert.");
    blinken(5, 100);
    if (verbindeWLAN()) {
      zeitSynchronisieren();
      schlafen();
    } else {
      esp_sleep_enable_timer_wakeup(22ULL * 3600ULL * 1000000ULL);
      esp_deep_sleep_start();
    }
  } else {
    Serial.println("\nZeitfenster abgelaufen. Erneuter Versuch in 10 Min.");
    blinken(20, 50);
    esp_sleep_enable_timer_wakeup(600ULL * 1000000ULL);
    esp_deep_sleep_start();
  }
}

bool pairingVersuch() {
  // Scan (3 Sek) – Adresse holen
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  NimBLEScanResults results = scan->getResults(3 * 1000, false);
  scan->stop();
  while (NimBLEDevice::getScan()->isScanning()) delay(50);

  NimBLEAddress gefundeneAdresse("00:00:00:00:00:00", BLE_ADDR_PUBLIC);
  bool gefunden = false;
  NimBLEUUID serviceUUID(SERVICE_UUID);

  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = results.getDevice(i);
    String addr = String(d->getAddress().toString().c_str()); addr.toLowerCase();
    String ziel = String(AQUAPRECISE_MAC);                    ziel.toLowerCase();
    if (addr == ziel || d->isAdvertisingService(serviceUUID)) {
      gefundeneAdresse = d->getAddress();
      gefunden = true;
      Serial.printf("  Gefunden (RSSI: %d dBm)\n", d->getRSSI());
      break;
    }
  }
  scan->clearResults();

  if (!gefunden) return false;

  // Gerät gefunden → jetzt 10x schnell verbinden ohne erneuten Scan.
  // AquaPrecise-Knopf muss JETZT gehalten werden.
  Serial.println("  >>> KNOPF JETZT HALTEN! 10 Versuche <<<");
  for (int v = 1; v <= 10; v++) {
    gPairingDone    = false;
    gPairingSuccess = false;

    NimBLEClient* client = NimBLEDevice::createClient();
    client->setClientCallbacks(new MeinClientCallback(), true);
    client->setConnectTimeout(2000);  // 2s pro Versuch

    Serial.printf("  Connect %d/10...", v);
    unsigned long t0 = millis();
    bool verbunden = client->connect(gefundeneAdresse);
    Serial.printf(" %s (%lu ms)\n", verbunden ? "OK" : "FEHLER", millis() - t0);

    if (verbunden) {
      Serial.print("  Warte auf Bond");
      for (int i = 0; i < 100 && !gPairingDone; i++) {
        delay(100);
        if (i % 10 == 0) Serial.print(".");
      }
      Serial.printf(" %s\n", gPairingSuccess ? "OK" : "nicht gebondet");
      client->disconnect();
      NimBLEDevice::deleteClient(client);
      if (gPairingSuccess) return true;
    } else {
      NimBLEDevice::deleteClient(client);
    }
    delay(200);
  }
  return false;
}

// ============================================================
//   WLAN
// ============================================================
bool verbindeWLAN() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Verbinde mit WLAN");
  for (int i = 0; i < 30; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(" OK");
      blinken(1, 200);
      return true;
    }
    delay(500); Serial.print(".");
  }
  Serial.println(" FEHLER");
  blinken(20, 50);
  return false;
}

// ============================================================
//   ZEIT
// ============================================================
void zeitSynchronisieren() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  Serial.print("Synchronisiere Uhrzeit");
  struct tm t;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&t)) { Serial.println(" OK"); return; }
    delay(500); Serial.print(".");
  }
  Serial.println(" FEHLER");
}

void wartenBis(int stunde, int minute) {
  if (minute >= 60) { stunde += minute / 60; minute %= 60; }
  for (int i = 0; i < 600; i++) {
    struct tm t;
    if (getLocalTime(&t) &&
        (t.tm_hour > stunde || (t.tm_hour == stunde && t.tm_min >= minute))) {
      Serial.printf("Zeit erreicht: %02d:%02d\n", t.tm_hour, t.tm_min);
      return;
    }
    delay(1000);
  }
}

// ============================================================
//   WETTER
// ============================================================
float heutigeNiederschlagsmenge() {
  char url[300];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=%.4f&longitude=%.4f"
    "&daily=precipitation_sum"
    "&timezone=Europe%%2FZurich"
    "&forecast_days=1",
    LATITUDE, LONGITUDE);
  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) { http.end(); return -1.0; }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return -1.0;
  return doc["daily"]["precipitation_sum"][0].as<float>();
}

// ============================================================
//   BLUETOOTH – Stop-Befehl
// ============================================================
bool stoppeAquaPrecise() {
  NimBLEDevice::init("");
  NimBLEDevice::setPower(9, NimBLETxPowerType::All);
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  Serial.println("  Scanne (8 Sek)...");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  NimBLEScanResults results = scan->getResults(8 * 1000, false);
  scan->stop();
  while (NimBLEDevice::getScan()->isScanning()) delay(100);

  NimBLEAddress savedAddr("00:00:00:00:00:00", BLE_ADDR_PUBLIC);
  bool gefunden = false;
  NimBLEUUID serviceUUID(SERVICE_UUID);

  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = results.getDevice(i);
    String addr = String(d->getAddress().toString().c_str()); addr.toLowerCase();
    String ziel = String(AQUAPRECISE_MAC);                    ziel.toLowerCase();
    if (addr == ziel || d->isAdvertisingService(serviceUUID)) {
      savedAddr = d->getAddress();
      gefunden = true;
      Serial.printf("  Gefunden: %s (RSSI: %d dBm, AdvType: %d)\n",
        d->getAddress().toString().c_str(), d->getRSSI(), d->getAdvType());
      break;
    }
  }
  scan->clearResults();

  if (!gefunden) {
    Serial.println("  AquaPrecise nicht gefunden!");
    blinken(20, 50);
    NimBLEDevice::deinit(false);
    return false;
  }

  delay(2000);

  NimBLEClient* client = NimBLEDevice::createClient(savedAddr);
  client->setClientCallbacks(new MeinClientCallback(), true);
  client->setConnectTimeout(15000);  // ms in NimBLE v2.x

  bool verbunden = false;
  for (int v = 1; v <= 3; v++) {
    Serial.printf("  Verbinde (Versuch %d/3)...", v);
    unsigned long t0 = millis();
    bool ok = client->connect();
    Serial.printf(" %s (%lu ms)\n", ok ? "OK" : "FEHLER", millis() - t0);
    if (ok) { verbunden = true; break; }
    if (v < 3) delay(3000);
  }

  if (!verbunden) {
    Serial.println("  Verbindung fehlgeschlagen.");
    blinken(20, 50);
    NimBLEDevice::deleteClient(client);
    NimBLEDevice::deinit(false);
    return false;
  }

  blinken(2, 200);
  delay(500);  // Kurz warten bis Encryption fertig

  NimBLERemoteService* svc = client->getService(SERVICE_UUID);
  if (!svc) {
    Serial.println("  Service nicht gefunden!");
    blinken(20, 50);
    client->disconnect(); NimBLEDevice::deleteClient(client);
    NimBLEDevice::deinit(false); return false;
  }

  NimBLERemoteCharacteristic* ch = svc->getCharacteristic(POWER_CHAR_UUID);
  if (!ch) {
    Serial.println("  Characteristic nicht gefunden!");
    blinken(20, 50);
    client->disconnect(); NimBLEDevice::deleteClient(client);
    NimBLEDevice::deinit(false); return false;
  }

  uint8_t stopCmd = 0x00;
  bool erfolg = ch->writeValue(&stopCmd, 1, true);
  Serial.println(erfolg ? "  Stop-Befehl gesendet OK" : "  Senden fehlgeschlagen");
  if (!erfolg) blinken(20, 50);

  delay(500);
  client->disconnect();
  NimBLEDevice::deleteClient(client);
  NimBLEDevice::deinit(false);
  return erfolg;
}

// ============================================================
//   DEEP SLEEP bis 1 Minute vor Bewässerungsstart
// ============================================================
void schlafen() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  digitalWrite(LED_PIN, LOW);

  struct tm t;
  getLocalTime(&t);

  int weckMin = BEWAESSERUNG_MINUTE - 1;
  int weckStd = BEWAESSERUNG_HOUR;
  if (weckMin < 0) { weckMin = 59; weckStd--; }

  int jetztSek = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
  int zielSek  = weckStd * 3600 + weckMin * 60;
  if (jetztSek >= zielSek) zielSek += 24 * 3600;

  uint64_t schlafSek = (uint64_t)(zielSek - jetztSek);
  Serial.printf("\nSchlafe %.1f Stunden bis %02d:%02d Uhr\n",
                schlafSek / 3600.0, weckStd, weckMin);
  Serial.println("====================================\n");
  Serial.flush();

  esp_sleep_enable_timer_wakeup(schlafSek * 1000000ULL);
  esp_deep_sleep_start();
}
