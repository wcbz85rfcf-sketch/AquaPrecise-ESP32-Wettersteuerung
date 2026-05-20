/*
 * AquaPrecise BLE Scanner
 * ========================
 * Einmal ausführen um die MAC-Adresse der AquaPrecise zu finden.
 * Adresse dann in AquaPrecise_Wettersteuerung.ino eintragen.
 *
 * Benötigt: NimBLE-Arduino (Bibliotheken verwalten)
 */

#include <NimBLEDevice.h>

static NimBLEUUID aquaPreciseServiceUUID("98bd0001-0b0e-421a-84e5-ddbf75dc6de4");

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n====================================");
  Serial.println(" AquaPrecise BLE Scanner");
  Serial.println("====================================");
  Serial.println("Scanne 15 Sekunden...");
  Serial.println("AquaPrecise einschalten, Gardena App schliessen!");
  Serial.println();

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  // Blockierender 15-Sekunden-Scan (NimBLE v2.x API)
  NimBLEScanResults results = scan->getResults(15 * 1000, false);
  int count = results.getCount();

  Serial.printf("\n--- %d Geraete gefunden ---\n", count);
  bool found = false;

  for (int i = 0; i < count; i++) {
    const NimBLEAdvertisedDevice* device = results.getDevice(i);
    String name = String(device->getName().c_str());
    String addr = String(device->getAddress().toString().c_str());

    bool isAquaPrecise = (name.indexOf("AquaPrecise") >= 0) ||
                         (name.indexOf("Gardena") >= 0) ||
                         (device->isAdvertisingService(aquaPreciseServiceUUID));

    if (isAquaPrecise) {
      Serial.println("╔══════════════════════════════════╗");
      Serial.println("║  AQUAPRECISE GEFUNDEN!           ║");
      Serial.println("╠══════════════════════════════════╣");
      Serial.print  ("║  MAC-Adresse: "); Serial.println(addr);
      Serial.print  ("║  Name:        ");
      Serial.println(name.length() > 0 ? name : "(kein Name)");
      Serial.println("╚══════════════════════════════════╝");
      Serial.println("→ MAC-Adresse in Haupt-Sketch eintragen!");
      found = true;
    } else {
      Serial.print("  Geraet: "); Serial.print(addr);
      if (name.length() > 0) {
        Serial.print("  ("); Serial.print(name); Serial.print(")");
      }
      Serial.println();
    }
  }

  if (!found) {
    Serial.println("\n✗ AquaPrecise nicht gefunden.");
    Serial.println("  - AquaPrecise eingeschaltet?");
    Serial.println("  - Gardena App auf Handy geschlossen?");
    Serial.println("  - ESP32 naeher an AquaPrecise halten?");
    Serial.println("  → Sketch nochmals hochladen und wiederholen.");
  }

  NimBLEDevice::deinit();
  Serial.println("\nScan abgeschlossen.");
}

void loop() {}
