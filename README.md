# LightMatrix144

Arduino-Projekt zur Ansteuerung einer 12 × 12 LED-Matrix mit einem Arduino UNO
R4 WiFi. Die 144 LEDs werden über 144 echte, optisch entkoppelte Relais
geschaltet. Die Relais-Eingänge sind aktiv HIGH: Ein HIGH-Signal schaltet das
Relais und damit die LED ein.

## Funktionen

- 144 Ausgänge über neun MCP23017-Portexpander an zwei I²C-Bussen
- Tägliches Einschalten um 00:00 Uhr in einer Spirale von außen nach innen
- Gestaffeltes Einschalten mit 75 ms Abstand pro Relais (ca. 10,8 Sekunden für
  die gesamte Matrix), ohne blockierende Wartezeit
- Zufällige, für den jeweiligen Tageszyklus reproduzierbare Abschaltreihenfolge
  bis 23:50 Uhr; anschließend sind alle LEDs aus
- Uhrzeit über NTP und die interne RTC des UNO R4 WiFi; automatische Umrechnung
  auf mitteleuropäische Zeit inklusive Sommer-/Winterzeit
- Offline-Zyklus, falls keine gültige Uhrzeit verfügbar ist
- Weboberfläche mit Uhrzeit, LED-Zustand, 12 × 12 Matrix und Status der
  MCP23017-Expander

## Hardware

- Arduino UNO R4 WiFi
- 9 × MCP23017
- 144 optisch entkoppelte Relais-Eingänge beziehungsweise geeignete
  Relaismodule/-treiber
- Separate, ausreichend dimensionierte 5-V-Versorgung für die Relais

Die Relais werden nicht aus dem Arduino gespeist. Dimensioniere die externe
Versorgung anhand des Spulenstroms der verwendeten Relaismodule und der Anzahl
der gleichzeitig angezogenen Relais. MCP23017-Ausgänge dürfen Relais-Spulen
nicht direkt treiben; verwende dafür geeignete Relaismodule oder Treiber.

Während Reset und Initialisierung sind die MCP-Ausgänge zunächst nicht
garantiert aktiv LOW. Sorge dafür, dass die Relais-Eingänge in dieser Phase
sicher ausgeschaltet bleiben, zum Beispiel durch eine passende
Pulldown-Beschaltung, falls sie nicht bereits auf den Modulen vorhanden ist.
Beachte die Datenblätter und die elektrische Sicherheit, insbesondere bei
Relaiskontakten, die Netzspannung schalten.

### I²C-Adressen und Zuordnung

Die Zuordnung im Sketch (`mcpConfig`) ist aktuell:

| I²C-Bus | Arduino-Schnittstelle | MCP23017-Adressen |
|---------|-----------------------|-------------------|
| Bus 0   | `Wire` (SDA/SCL, laut Sketch A4/A5) | `0x20`–`0x23` |
| Bus 1   | `Wire1` (Qwiic)       | `0x20`–`0x24` |

Gleiche I²C-Adressen sind möglich, wenn die Bausteine an getrennten Bussen
liegen. Prüfe die Busbelegung und die Adress-Jumper deiner konkreten Hardware.

Die Standard-LED-Zuordnung läuft zeilenweise über alle 144 logischen
Matrixpositionen: jeweils 16 aufeinanderfolgende LEDs liegen auf einem
MCP23017, von MCP 0 bis MCP 8. Die Funktion `initializeLedMap()` in
`LightMatrix144.ino` ist der Ort, um die logische Matrix an die tatsächliche
Verdrahtung anzupassen.

## Software und Installation

1. Installiere in der Arduino IDE das Board-Paket für **Arduino UNO R4 WiFi**
   und wähle dieses Board aus.
2. Installiere die Bibliotheken **Adafruit MCP23017 Arduino Library**,
   **Adafruit BusIO** und **NTPClient** (Fabrice Weinberg).
3. Öffne `LightMatrix144.ino`.
4. Trage bei `WIFI_SSID` und `WIFI_PASS` die Zugangsdaten ein, wenn WLAN,
   NTP-Zeitsynchronisierung und Weboberfläche verwendet werden sollen. Ohne
   konfigurierte Zugangsdaten läuft der Sketch offline.
5. Prüfe vor dem Anschluss aller Relais die I²C-Adressen, die Matrixzuordnung
   und den definierten Aus-Zustand der Relais-Eingänge.
6. Lade den Sketch auf das Board. Die serielle Ausgabe läuft mit **115200
   Baud**.

WLAN-Zugangsdaten gehören nicht in ein öffentliches Repository. Hinterlege sie
lokal im Sketch oder verwende eine separate, nicht eingecheckte
Konfigurationsdatei.

## Zeitplan und Verhalten

Der tägliche Zyklus beginnt standardmäßig um 00:00 Uhr. Zwischen 00:00 und
23:50 werden die LEDs gleichmäßig über den Tag verteilt ausgeschaltet. Die
Reihenfolge wird pro Zyklus deterministisch gemischt; nach einem Neustart
innerhalb desselben Tages wird daher der zur aktuellen Uhrzeit passende Zustand
wiederhergestellt. Die letzten zehn Minuten des Tages sind vollständig dunkel.

Zum Beginn eines Zyklus wird die Einschaltspirale nur dann gestartet, wenn die
Uhrzeit verfügbar ist und der Start innerhalb der ersten 20 Sekunden erkannt
wird. Beim ersten Start ohne gültige Uhrzeit beginnt ein Offline-Zyklus mit
Spirale; danach läuft dieser Zyklus anhand der Laufzeit seit dem Einschalten.

Die wichtigsten Parameter stehen am Anfang des Sketches:

| Konstante | Standardwert | Bedeutung |
|-----------|--------------|-----------|
| `ROWS`, `COLS` | `12`, `12` | Matrixgröße |
| `START_HOUR`, `START_MINUTE` | `0`, `0` | Lokale Startzeit des Tageszyklus |
| `DARK_MINUTES` | `10` | Dauer der vollständig dunklen Schlussphase |
| `RELAY_ON_DELAY_MS` | `75` | Mindestabstand zwischen Einschalt-Schritten |
| `LED_ACTIVE_HIGH` | `true` | HIGH schaltet Relais/LED ein |

## Weboberfläche

Nach erfolgreicher WLAN-Verbindung startet ein HTTP-Server auf Port 80. Die
IP-Adresse wird im seriellen Monitor ausgegeben. Rufe im lokalen Netzwerk
`http://<IP-Adresse-des-Boards>/` auf. Die Seite aktualisiert sich automatisch
alle zehn Sekunden.
