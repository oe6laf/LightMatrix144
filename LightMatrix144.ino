
#include <Arduino.h>
#include <Wire.h>
#include <WiFiS3.h>
#include <RTC.h>
#include <Adafruit_MCP23X17.h>
#include <time.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

// ============================================================
// LED MATRIX CONTROLLER - UNO R4 WiFi
// Version 1.0
//
// 144 LEDs, 9 MCP23017, 2 I2C buses
// HIGH = relay ON = LED ON
// ============================================================

// ---------------------- CONFIGURATION ------------------------

const char WIFI_SSID[] = "";
const char WIFI_PASS[] = "";

constexpr uint8_t ROWS = 12;
constexpr uint8_t COLS = 12;
constexpr uint16_t LED_COUNT = ROWS * COLS;
constexpr uint8_t MCP_COUNT = 9;

// Local start time for a new daily cycle
constexpr uint8_t START_HOUR = 0;
constexpr uint8_t START_MINUTE = 0;

// All LEDs must be off before the last 10 minutes
constexpr uint16_t DARK_MINUTES = 10;

// Relay behavior
constexpr uint16_t RELAY_ON_DELAY_MS = 75;
constexpr bool LED_ACTIVE_HIGH = true;

// Network configuration
constexpr unsigned long WIFI_RETRY_MS = 300000UL;
constexpr unsigned long NTP_RETRY_MS = 60000UL;
constexpr unsigned long NTP_SYNC_MS = 21600000UL;

WiFiUDP ntpUDP;
NTPClient timeClient(
    ntpUDP,
    "pool.ntp.org",
    0,        // UTC offset in seconds
    60000     // Update interval in ms
);

// ---------------------- MCP CONFIG ---------------------------

// Bus 0 = Wire  (A4 / A5)
// Bus 1 = Wire1 (Qwiic)

struct McpConfig {
  uint8_t bus;
  uint8_t address;
};

const McpConfig mcpConfig[MCP_COUNT] = {
  {0, 0x20},
  {0, 0x21},
  {0, 0x22},
  {0, 0x23},
  {1, 0x20},
  {1, 0x21},
  {1, 0x22},
  {1, 0x23},
  {1, 0x24}
};

Adafruit_MCP23X17 mcp[MCP_COUNT];
bool mcpAvailable[MCP_COUNT] = {false};

uint16_t outputCache[MCP_COUNT] = {0};

// ---------------------- LED MAPPING --------------------------

struct LedPin {
  uint8_t mcp;
  uint8_t pin;
};

// Logical row/column to physical MCP/pin
LedPin ledMap[ROWS][COLS];

// Daily randomized shutdown order
uint8_t shutdownOrder[LED_COUNT];

// Spiral startup order
uint8_t spiralOrder[LED_COUNT];

// Logical LED state
bool ledOn[LED_COUNT] = {false};

// ---------------------- RUNTIME ------------------------------

WiFiServer server(80);

bool clockValid = false;
bool webServerStarted = false;

unsigned long lastWiFiAttempt = 0;
unsigned long lastNtpAttempt = 0;
unsigned long lastNtpSuccess = 0;
unsigned long lastScheduleUpdate = 0;

bool firstWiFiAttempt = true;

int64_t currentCycleKey = INT64_MIN;
uint16_t currentOffCount = LED_COUNT;
uint16_t currentOnCount = 0;

bool spiralActive = false;
uint16_t spiralStep = 0;
unsigned long lastSpiralStep = 0;

uint32_t currentElapsedSeconds = 0;

char clockText[32] = "No valid time";

// ============================================================
// MAPPING
// ============================================================

void initializeLedMap() {

  // Default mapping:
  // MCP 0: LED   0 -  15
  // MCP 1: LED  16 -  31
  // ...
  // MCP 8: LED 128 - 143

  for (uint8_t row = 0; row < ROWS; row++) {
    for (uint8_t col = 0; col < COLS; col++) {

      uint16_t index = row * COLS + col;

      ledMap[row][col] = {
        (uint8_t)(index / 16),
        (uint8_t)(index % 16)
      };
    }
  }

  // --------------------------------------------------------
  // CUSTOM PHYSICAL MAPPING
  //
  // Example: swap two logical matrix positions.
  // This avoids assigning the same output twice.
  //
  // LedPin temp = ledMap[0][0];
  // ledMap[0][0] = ledMap[5][7];
  // ledMap[5][7] = temp;
  //
  // --------------------------------------------------------
}

bool validateLedMap() {

  bool used[MCP_COUNT][16] = {};
  bool valid = true;

  for (uint8_t row = 0; row < ROWS; row++) {
    for (uint8_t col = 0; col < COLS; col++) {

      LedPin p = ledMap[row][col];

      if (p.mcp >= MCP_COUNT || p.pin >= 16) {
        Serial.println("ERROR: Invalid LED mapping");
        valid = false;
        continue;
      }

      if (used[p.mcp][p.pin]) {
        Serial.println("ERROR: Duplicate LED mapping");
        valid = false;
      }

      used[p.mcp][p.pin] = true;
    }
  }

  return valid;
}

// ============================================================
// MCP HARDWARE
// ============================================================

void initializeMcps() {

  Wire.begin();
  Wire1.begin();

  Wire.setClock(100000);
  Wire1.setClock(100000);

  for (uint8_t i = 0; i < MCP_COUNT; i++) {

    TwoWire *bus = (mcpConfig[i].bus == 0)
                     ? &Wire
                     : &Wire1;

    mcpAvailable[i] =
      mcp[i].begin_I2C(mcpConfig[i].address, bus);

    Serial.print("MCP ");
    Serial.print(i);
    Serial.print(" (Bus ");
    Serial.print(mcpConfig[i].bus);
    Serial.print(", Address 0x");
    Serial.print(mcpConfig[i].address, HEX);
    Serial.print("): ");

    if (!mcpAvailable[i]) {
      Serial.println("NOT FOUND");
      continue;
    }

    // Preload LOW into output registers.
    mcp[i].writeGPIOAB(0x0000);
    outputCache[i] = 0;

    // Enable outputs one by one.
    for (uint8_t pin = 0; pin < 16; pin++) {
      mcp[i].pinMode(pin, OUTPUT);
    }

    Serial.println("OK");
  }
}

void setSingleLed(uint8_t led, bool on) {

  if (led >= LED_COUNT) return;
  if (ledOn[led] == on) return;

  uint8_t row = led / COLS;
  uint8_t col = led % COLS;

  LedPin p = ledMap[row][col];

  if (p.mcp >= MCP_COUNT || p.pin >= 16) return;

  uint16_t mask = (uint16_t)1 << p.pin;

  if (on == LED_ACTIVE_HIGH) {
    outputCache[p.mcp] |= mask;
  } else {
    outputCache[p.mcp] &= ~mask;
  }

  if (mcpAvailable[p.mcp]) {
    mcp[p.mcp].writeGPIOAB(outputCache[p.mcp]);
  }

  ledOn[led] = on;

  if (on) {
    currentOnCount++;
  } else {
    currentOnCount--;
  }
}

void allLedsOff() {

  spiralActive = false;

  for (uint8_t i = 0; i < MCP_COUNT; i++) {

    if (mcpAvailable[i] && outputCache[i] != 0) {
      mcp[i].writeGPIOAB(0x0000);
    }

    outputCache[i] = 0;
  }

  for (uint16_t i = 0; i < LED_COUNT; i++) {
    ledOn[i] = false;
  }

  currentOnCount = 0;
  currentOffCount = LED_COUNT;
}

// ============================================================
// SPIRAL ORDER
// ============================================================

void createSpiralOrder() {

  int top = 0;
  int bottom = ROWS - 1;
  int left = 0;
  int right = COLS - 1;

  uint16_t index = 0;

  while (top <= bottom && left <= right) {

    // Top edge: left to right
    for (int col = left; col <= right; col++) {
      spiralOrder[index++] = top * COLS + col;
    }
    top++;

    // Right edge: top to bottom
    for (int row = top; row <= bottom; row++) {
      spiralOrder[index++] = row * COLS + right;
    }
    right--;

    // Bottom edge: right to left
    if (top <= bottom) {
      for (int col = right; col >= left; col--) {
        spiralOrder[index++] = bottom * COLS + col;
      }
      bottom--;
    }

    // Left edge: bottom to top
    if (left <= right) {
      for (int row = bottom; row >= top; row--) {
        spiralOrder[index++] = row * COLS + left;
      }
      left++;
    }
  }
}

void startSpiral() {

  allLedsOff();

  spiralStep = 0;
  lastSpiralStep = millis() - RELAY_ON_DELAY_MS;
  spiralActive = true;

  Serial.println("Starting spiral animation");
}

void updateSpiral() {

  if (!spiralActive) return;

  if (millis() - lastSpiralStep < RELAY_ON_DELAY_MS) {
    return;
  }

  lastSpiralStep = millis();

  if (spiralStep < LED_COUNT) {
    setSingleLed(spiralOrder[spiralStep], true);
    spiralStep++;
  }

  if (spiralStep >= LED_COUNT) {
    spiralActive = false;
    currentOffCount = 0;

    Serial.println("Spiral animation completed");
  }
}

// ============================================================
// DETERMINISTIC SHUFFLE
// ============================================================

uint32_t randomStep(uint32_t &state) {

  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;

  return state;
}

void createDailyShutdownOrder(int64_t cycleKey) {

  for (uint16_t i = 0; i < LED_COUNT; i++) {
    shutdownOrder[i] = (uint8_t)i;
  }

  uint32_t seed =
    (uint32_t)cycleKey ^ 0xA37F29B5UL;

  if (seed == 0) seed = 1;

  // Fisher-Yates shuffle
  for (int i = LED_COUNT - 1; i > 0; i--) {

    uint16_t j = randomStep(seed) % (i + 1);

    uint8_t temp = shutdownOrder[i];
    shutdownOrder[i] = shutdownOrder[j];
    shutdownOrder[j] = temp;
  }

  Serial.println("New daily shutdown order created");
}

// ============================================================
// RESTORE STATE FOR CURRENT TIME
// ============================================================

void applyScheduledState(uint16_t offCount) {

  if (offCount > LED_COUNT) {
    offCount = LED_COUNT;
  }

  // Determine desired logical state
  bool desiredOn[LED_COUNT];

  for (uint16_t i = 0; i < LED_COUNT; i++) {
    desiredOn[i] = true;
  }

  for (uint16_t i = 0; i < offCount; i++) {
    desiredOn[shutdownOrder[i]] = false;
  }

  // Change only outputs that differ
  for (uint16_t led = 0; led < LED_COUNT; led++) {

    if (ledOn[led] != desiredOn[led]) {
      setSingleLed((uint8_t)led, desiredOn[led]);
    }
  }

  currentOffCount = offCount;
}

// ============================================================
// CLOCK: CENTRAL EUROPEAN TIME
// ============================================================

// Gregorian calendar helper.
// Sunday = 0, Monday = 1, etc.
int dayOfWeek(int year, int month, int day) {

  static const int offsets[] =
    {0,3,2,5,0,3,5,1,4,6,2,4};

  if (month < 3) year--;

  return (year + year/4 - year/100 + year/400 +
          offsets[month-1] + day) % 7;
}

int lastSunday(int year, int month) {

  // March and October both have 31 days
  return 31 - dayOfWeek(year, month, 31);
}

bool isCest(time_t utc) {

  struct tm t;
  gmtime_r(&utc, &t);

  int year = t.tm_year + 1900;
  int month = t.tm_mon + 1;
  int day = t.tm_mday;
  int hour = t.tm_hour;

  if (month < 3 || month > 10) return false;
  if (month > 3 && month < 10) return true;

  int transitionDay = lastSunday(year, month);

  if (month == 3) {
    return (day > transitionDay) ||
           (day == transitionDay && hour >= 1);
  }

  return (day < transitionDay) ||
         (day == transitionDay && hour < 1);
}

time_t localEpoch(time_t utc) {
  return utc + (isCest(utc) ? 7200 : 3600);
}


bool synchronizeClock() {

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  lastNtpAttempt = millis();

  Serial.println("Requesting NTP time...");

  if (!timeClient.forceUpdate()) {
    Serial.println("NTP request failed");
    return false;
  }

  unsigned long epoch = timeClient.getEpochTime();

  Serial.print("NTP Unix timestamp: ");
  Serial.println(epoch);

  if (epoch < 1700000000UL) {
    Serial.println("Invalid NTP timestamp");
    return false;
  }

  RTCTime rtcTime((time_t)epoch);

  if (!RTC.setTime(rtcTime)) {
    Serial.println("RTC synchronization failed");
    return false;
  }

  clockValid = true;
  lastNtpSuccess = millis();

  Serial.println("RTC synchronized successfully");

  return true;
}


// ============================================================
// WIFI
// ============================================================

void maintainWiFi() {

  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {

    if (!webServerStarted) {
      server.begin();
      webServerStarted = true;

      Serial.print("Webserver: http://");
      Serial.println(WiFi.localIP());
    }

    unsigned long syncInterval =
      clockValid ? NTP_SYNC_MS : NTP_RETRY_MS;

    if (lastNtpAttempt == 0 ||
        now - lastNtpAttempt >= syncInterval) {
      synchronizeClock();
    }

    return;
  }

  webServerStarted = false;

  if (WIFI_SSID[0] == '\0') return;

  if (firstWiFiAttempt ||
      now - lastWiFiAttempt >= WIFI_RETRY_MS) {

    firstWiFiAttempt = false;
    lastWiFiAttempt = now;

    Serial.println("Connecting to WiFi...");

    WiFi.setTimeout(3000);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ============================================================
// DAILY SCHEDULER
// ============================================================

void updateSchedule() {

  uint64_t logicalSeconds;

  if (clockValid) {

    RTCTime rtcTime;

    if (!RTC.getTime(rtcTime)) return;

    time_t utc = rtcTime.getUnixTime();
    time_t local = localEpoch(utc);

    struct tm t;
    gmtime_r(&local, &t);

    snprintf(clockText, sizeof(clockText),
             "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900,
             t.tm_mon + 1,
             t.tm_mday,
             t.tm_hour,
             t.tm_min,
             t.tm_sec);

    logicalSeconds = (uint64_t)local;

  } else {

    // Free-running cycle when no valid clock exists.
    logicalSeconds = millis() / 1000ULL;

    uint32_t seconds = logicalSeconds % 86400ULL;

    snprintf(clockText, sizeof(clockText),
             "OFFLINE %02lu:%02lu:%02lu",
             (unsigned long)(seconds / 3600),
             (unsigned long)((seconds / 60) % 60),
             (unsigned long)(seconds % 60));
  }

  const uint32_t startSeconds =
    START_HOUR * 3600UL + START_MINUTE * 60UL;

  // Calendar-independent cycle calculation.
  // Works across month and year boundaries.
  const int64_t cycleSeconds =
    (int64_t)logicalSeconds - startSeconds;

  const int64_t secondsPerDay = 86400LL;

  int64_t cycleKey = cycleSeconds / secondsPerDay;
  int64_t elapsed = cycleSeconds % secondsPerDay;

  // Mathematical floor division for negative values
  if (elapsed < 0) {
    elapsed += secondsPerDay;
    cycleKey--;
  }

  currentElapsedSeconds = (uint32_t)elapsed;

  bool newCycle = cycleKey != currentCycleKey;
  bool firstCycle = currentCycleKey == INT64_MIN;

  if (newCycle) {

    createDailyShutdownOrder(cycleKey);
    currentCycleKey = cycleKey;

    // Animate only when entering the beginning of a cycle.
    // Also works when started exactly at midnight.
    if (elapsed < 20 && clockValid) {
      startSpiral();
    } else if (firstCycle && !clockValid) {
      startSpiral();
    } else {
      spiralActive = false;
      currentOffCount = 65535;
    }
  }

  if (spiralActive) {

    // If time jumped forward after synchronization,
    // stop the animation and restore scheduled state.
    if (elapsed > 30 && clockValid) {
      spiralActive = false;
    } else {
      updateSpiral();
      return;
    }
  }

  const uint32_t activeSeconds =
    86400UL - DARK_MINUTES * 60UL;

  uint16_t shouldBeOff;

  if (elapsed >= activeSeconds) {
    shouldBeOff = LED_COUNT;
  } else {
    shouldBeOff =
      ((uint64_t)elapsed * LED_COUNT) / activeSeconds;
  }

  if (shouldBeOff != currentOffCount) {
    applyScheduledState(shouldBeOff);
  }
}

// ============================================================
// WEB INTERFACE
// ============================================================

void handleWebClient() {

  if (!webServerStarted) return;

  WiFiClient client = server.available();
  if (!client) return;

  client.setTimeout(200);

  // Read request line
  client.readStringUntil('\n');

  // Read HTTP headers
  while (client.connected()) {

    String line = client.readStringUntil('\n');

    if (line == "\r" || line.length() == 0) {
      break;
    }
  }

  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html; charset=utf-8"));
  client.println(F("Cache-Control: no-store"));
  client.println(F("Connection: close"));
  client.println();

  client.println(F("<!DOCTYPE html>"));
  client.println(F("<html><head>"));
  client.println(F("<meta charset='utf-8'>"));
  client.println(F("<meta name='viewport' content='width=device-width,initial-scale=1'>"));
  client.println(F("<meta http-equiv='refresh' content='10'>"));

  client.println(F("<title>LED Matrix Controller</title>"));

  client.println(F("<style>"));
  client.println(F("body{background:#171a21;color:white;font-family:Arial;text-align:center;padding:15px}"));
  client.println(F(".panel{background:#252a35;border-radius:14px;padding:15px;margin:12px auto;max-width:500px}"));
  client.println(F(".grid{display:grid;grid-template-columns:repeat(12,1fr);gap:5px;max-width:440px;margin:auto}"));
  client.println(F(".led{aspect-ratio:1;border-radius:50%;background:#343944}"));
  client.println(F(".on{background:#ffcb35;box-shadow:0 0 10px #ffb700}"));
  client.println(F(".number{font-size:30px;font-weight:bold}"));
  client.println(F(".secondary{color:#a5adbc}"));
  client.println(F("</style></head><body>"));

  client.println(F("<h2>LED Matrix Controller</h2>"));

  client.println(F("<div class='panel'>"));

  client.print(F("<div class='number'>"));
  client.print(clockText);
  client.println(F("</div>"));

  client.print(F("<p class='secondary'>"));
  client.print(clockValid ? "RTC / Central Europe" :
                            "Offline simulation");
  client.println(F("</p>"));

  client.print(F("<p>LEDs ON: <b>"));
  client.print(currentOnCount);
  client.print(F(" / "));
  client.print(LED_COUNT);
  client.println(F("</b></p>"));

  client.print(F("<p>Mode: "));
  client.print(spiralActive ? "Spiral startup" :
                currentOnCount == 0 ? "Dark phase" :
                                      "Daily countdown");
  client.println(F("</p></div>"));

  client.println(F("<div class='panel'>"));
  client.println(F("<div class='grid'>"));

  for (uint16_t i = 0; i < LED_COUNT; i++) {

    client.print(F("<div class='led"));

    if (ledOn[i]) {
      client.print(F(" on"));
    }

    client.println(F("'></div>"));
  }

  client.println(F("</div></div>"));

  client.println(F("<div class='panel'>"));
  client.print(F("<p>MCP devices: "));

  uint8_t available = 0;

  for (uint8_t i = 0; i < MCP_COUNT; i++) {
    if (mcpAvailable[i]) available++;
  }

  client.print(available);
  client.print(F(" / "));
  client.print(MCP_COUNT);
  client.println(F("</p>"));

  client.println(F("<p>Start: 00:00 | Dark: 23:50</p>"));
  client.println(F("</div>"));

  client.println(F("</body></html>"));

  delay(1);
  client.stop();
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("===========================");
  Serial.println(" LED MATRIX CONTROLLER 1.0");
  Serial.println("===========================");

  initializeLedMap();

  if (!validateLedMap()) {
    Serial.println("FATAL: Invalid LED mapping");
    while (true) {
      delay(1000);
    }
  }

  createSpiralOrder();
  initializeMcps();

  RTC.begin();
  timeClient.begin();

  // Check whether the internal RTC already has a valid date.
  RTCTime rtcTime;

  if (RTC.getTime(rtcTime)) {

    if (rtcTime.getYear() >= 2025 &&
        rtcTime.getYear() <= 2099) {

      clockValid = true;
      Serial.println("RTC contains a plausible date");
    }
  }

  // Immediately establish a defined LED state.
  updateSchedule();

  // WiFi is optional.
  maintainWiFi();

  // Update again if time has synchronized.
  updateSchedule();

  Serial.println("Setup completed");
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  maintainWiFi();

  if (millis() - lastScheduleUpdate >= 1000UL) {
    lastScheduleUpdate = millis();
    updateSchedule();
  }

  // Animation must run more often than once a second.
  // Do not call it twice if the scheduler already did.
  if (spiralActive) {
    updateSpiral();
  }

  handleWebClient();
}
