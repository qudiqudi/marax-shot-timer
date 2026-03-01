// MaraX Shot Timer -- based on github.com/alexrus/marax_timer
// Added: WiFi (via WiFiManager) + InfluxDB telemetry

// D5, D6, D7 are already defined by the nodemcuv2 board variant
#define PUMP_PIN D7

#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <SoftwareSerial.h>

// ── Added for WiFi + InfluxDB ────────────────────────────────────────────────
#include <WiFiManager.h>
#include <InfluxDbClient.h>
#include <LittleFS.h>

// ── Original globals ─────────────────────────────────────────────────────────
Adafruit_SSD1306 display(128, 64, &Wire, -1);
SoftwareSerial mySerial(D5, D6);

// Replaced Timer library with millis()-based approach (not in PlatformIO registry)
unsigned long lastDisplayUpdate = 0;

// set to true/false when using another type of reed sensor
bool reedOpenSensor = true;
bool displayOn = true;
int timerCount = 0;
int prevTimerCount = 0;
bool timerStarted = false;
unsigned long timerStartMillis = 0;
unsigned long timerStopMillis = 0;
unsigned long timerDisplayOffMillis = 0;
unsigned long serialUpdateMillis = 0;
int pumpInValue = 0;

const byte numChars = 32;
char receivedChars[numChars];
static byte ndx = 0;
char endMarker = '\n';
char rc;

// ── WiFi / InfluxDB config ───────────────────────────────────────────────────
#define CONFIG_FILE "/config.json"

char influxUrl[128]    = "";
char influxOrg[64]     = "";
char influxBucket[64]  = "";
char influxToken[128]  = "";

InfluxDBClient* influxClient = nullptr;
Point sensorPoint("marax");
Point shotPoint("marax_shot");

bool wifiConnected = false;
bool shotEventSent = false;
bool currentShotValid = false;
unsigned long lastTelemetry = 0;
unsigned long lastShotMillis = 0;
const unsigned long TELEMETRY_INTERVAL = 5000;
const unsigned long SHOT_COOLDOWN_MS = 90UL * 1000;

// ── Forward declarations ─────────────────────────────────────────────────────
void updateDisplay();
void detectChanges();
void getMachineInput();
String getTimer();
void loadConfig();
void saveConfig();
void setupWiFi();
void setupInflux();
void sendTelemetry();
void sendShotEvent(int duration);
void drawWiFiIcon(int x, int y, bool connected);

// ── Config persistence (LittleFS) ────────────────────────────────────────────

void loadConfig() {
    if (!LittleFS.begin()) return;
    if (!LittleFS.exists(CONFIG_FILE)) return;
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) return;

    // Simple key=value parsing to avoid ArduinoJson dependency
    String json = f.readString();
    f.close();

    // Minimal JSON parsing for our 4 flat fields
    auto extractValue = [&](const char* key) -> String {
        String needle = String("\"") + key + "\":\"";
        int start = json.indexOf(needle);
        if (start < 0) return "";
        start += needle.length();
        int end = json.indexOf("\"", start);
        if (end < 0) return "";
        return json.substring(start, end);
    };

    extractValue("url").toCharArray(influxUrl, sizeof(influxUrl));
    extractValue("org").toCharArray(influxOrg, sizeof(influxOrg));
    extractValue("bucket").toCharArray(influxBucket, sizeof(influxBucket));
    extractValue("token").toCharArray(influxToken, sizeof(influxToken));

    Serial.println("Config loaded");
}

void saveConfig() {
    if (!LittleFS.begin()) return;
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) return;

    f.print("{\"url\":\"");    f.print(influxUrl);
    f.print("\",\"org\":\"");  f.print(influxOrg);
    f.print("\",\"bucket\":\""); f.print(influxBucket);
    f.print("\",\"token\":\"");  f.print(influxToken);
    f.print("\"}");
    f.close();
    Serial.println("Config saved");
}

// ── WiFiManager setup ────────────────────────────────────────────────────────

bool shouldSaveConfig = false;
void saveConfigCallback() { shouldSaveConfig = true; }

void setupWiFi() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(120);

    WiFiManagerParameter pUrl("influx_url", "InfluxDB URL", influxUrl, sizeof(influxUrl));
    WiFiManagerParameter pOrg("influx_org", "InfluxDB Org", influxOrg, sizeof(influxOrg));
    WiFiManagerParameter pBucket("influx_bucket", "InfluxDB Bucket", influxBucket, sizeof(influxBucket));
    WiFiManagerParameter pToken("influx_token", "InfluxDB Token", influxToken, sizeof(influxToken));

    wm.addParameter(&pUrl);
    wm.addParameter(&pOrg);
    wm.addParameter(&pBucket);
    wm.addParameter(&pToken);
    wm.setSaveConfigCallback(saveConfigCallback);

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("Connecting WiFi...");
    display.println();
    display.println("If no network saved,");
    display.println("join WiFi:");
    display.println("  MaraX-Timer");
    display.display();

    if (wm.autoConnect("MaraX-Timer")) {
        Serial.print("WiFi connected: ");
        Serial.println(WiFi.localIP());
        wifiConnected = true;

        if (shouldSaveConfig) {
            strlcpy(influxUrl,    pUrl.getValue(),    sizeof(influxUrl));
            strlcpy(influxOrg,    pOrg.getValue(),    sizeof(influxOrg));
            strlcpy(influxBucket, pBucket.getValue(), sizeof(influxBucket));
            strlcpy(influxToken,  pToken.getValue(),  sizeof(influxToken));
            saveConfig();
        }
    } else {
        Serial.println("WiFi not connected, continuing offline");
        wifiConnected = false;
    }
}

// ── InfluxDB setup ───────────────────────────────────────────────────────────

void setupInflux() {
    if (strlen(influxUrl) == 0 || !wifiConnected) return;

    influxClient = new InfluxDBClient(influxUrl, influxOrg, influxBucket, influxToken);
    influxClient->setWriteOptions(
        WriteOptions()
            .writePrecision(WritePrecision::S)
            .batchSize(5)
            .bufferSize(20)
            .flushInterval(15)
    );

    if (influxClient->validateConnection()) {
        Serial.print("InfluxDB connected: ");
        Serial.println(influxClient->getServerUrl());
    } else {
        Serial.print("InfluxDB connection failed: ");
        Serial.println(influxClient->getLastErrorMessage());
    }
}

// ── Telemetry ────────────────────────────────────────────────────────────────

void sendTelemetry() {
    if (!influxClient || !wifiConnected) return;
    if (millis() - lastTelemetry < TELEMETRY_INTERVAL) return;
    if (!receivedChars[0]) return; // no data yet
    if (strlen(receivedChars) < 24) return; // incomplete serial frame
    lastTelemetry = millis();

    sensorPoint.clearFields();
    sensorPoint.clearTags();

    // Parse values from receivedChars (same indices the display uses)
    char modeTag[2] = {receivedChars[0] == 'V' ? 'S' : receivedChars[0], '\0'};
    sensorPoint.addTag("mode", modeTag);

    char fw[5] = {receivedChars[1], receivedChars[2], receivedChars[3], receivedChars[4], '\0'};
    sensorPoint.addTag("firmware", fw);

    int steam = (receivedChars[6]-'0')*100 + (receivedChars[7]-'0')*10 + (receivedChars[8]-'0');
    int targetSteam = (receivedChars[10]-'0')*100 + (receivedChars[11]-'0')*10 + (receivedChars[12]-'0');
    int hx = (receivedChars[14]-'0')*100 + (receivedChars[15]-'0')*10 + (receivedChars[16]-'0');
    int boost = (receivedChars[18]-'0')*1000 + (receivedChars[19]-'0')*100 + (receivedChars[20]-'0')*10 + (receivedChars[21]-'0');
    int heat = receivedChars[23] - '0';

    // Reject garbage values from corrupted serial frames
    if (steam < 0 || steam > 200) return;
    if (targetSteam < 0 || targetSteam > 200) return;
    if (hx < 0 || hx > 200) return;
    if (heat < 0 || heat > 1) return;

    sensorPoint.addField("steam_temp", steam);
    sensorPoint.addField("target_steam_temp", targetSteam);
    sensorPoint.addField("hx_temp", hx);
    sensorPoint.addField("heating", heat);
    sensorPoint.addField("boost_countdown", boost);

    if (!influxClient->writePoint(sensorPoint)) {
        Serial.print("InfluxDB write failed: ");
        Serial.println(influxClient->getLastErrorMessage());
    }
}

void sendShotEvent(int duration) {
    if (!influxClient || !wifiConnected) return;

    shotPoint.clearFields();
    shotPoint.clearTags();

    char modeTag[2] = {receivedChars[0] == 'V' ? 'S' : receivedChars[0], '\0'};
    shotPoint.addTag("mode", modeTag);
    shotPoint.addField("duration", duration);

    if (!influxClient->writePoint(shotPoint)) {
        Serial.print("InfluxDB shot write failed: ");
        Serial.println(influxClient->getLastErrorMessage());
    }
}

// ── WiFi status icon ─────────────────────────────────────────────────────────

void drawWiFiIcon(int x, int y, bool connected) {
    if (connected) {
        display.drawPixel(x + 3, y + 6, SSD1306_WHITE);
        display.drawCircle(x + 3, y + 6, 2, SSD1306_WHITE);
        display.drawCircle(x + 3, y + 6, 4, SSD1306_WHITE);
        display.drawCircle(x + 3, y + 6, 6, SSD1306_WHITE);
        display.fillRect(x, y + 7, 8, 7, SSD1306_BLACK);
    } else {
        display.drawLine(x + 1, y + 1, x + 5, y + 5, SSD1306_WHITE);
        display.drawLine(x + 5, y + 1, x + 1, y + 5, SSD1306_WHITE);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Original timer.ino code below -- unchanged except:
//   1. setup(): WiFi.mode(WIFI_OFF) replaced with setupWiFi()/setupInflux()
//   2. setup(): Timer t replaced with millis()-based approach
//   3. loop(): added sendTelemetry(), WiFi status check, InfluxDB flush
//   4. detectChanges(): added sendShotEvent() when shot ends
//   5. updateDisplay(): added WiFi icon on idle screen
// ═══════════════════════════════════════════════════════════════════════════════

void setup() {
  // WiFi.mode(WIFI_OFF);  // removed -- we want WiFi now

  Serial.begin(9600);
  mySerial.begin(9600);

  pinMode(PUMP_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  memset(receivedChars, 0, numChars);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.display();

  // ── Added: WiFi + InfluxDB ──
  loadConfig();
  setupWiFi();
  setupInflux();

  mySerial.write(0x11);
}

void loop() {
  // Timer t.update() replaced with millis check
  if (millis() - lastDisplayUpdate >= 100) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }

  detectChanges();
  getMachineInput();

  // ── Added: telemetry + WiFi housekeeping ──
  sendTelemetry();
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  // flushBuffer() removed — the library handles flushing per writeOptions
  yield();
}

void getMachineInput() {
  while (mySerial.available() ) {
    serialUpdateMillis = millis();
    rc = mySerial.read();

    if (rc != endMarker) {
      receivedChars[ndx] = rc;
      ndx++;
      if (ndx >= numChars) {
        ndx = numChars - 1;
      }
    } else {
      receivedChars[ndx] = '\0';
      ndx = 0;
      Serial.println(receivedChars);
    }
  }

  if (millis() - serialUpdateMillis > 5000) {
    serialUpdateMillis = millis();
    memset(receivedChars, 0, numChars);
    Serial.println("Request serial update");
    mySerial.write(0x11);
  }
}

void detectChanges() {
  digitalWrite(LED_BUILTIN, digitalRead(PUMP_PIN));
  if(reedOpenSensor) {
    pumpInValue = digitalRead(PUMP_PIN);
  } else {
    pumpInValue = !digitalRead(PUMP_PIN);
  }
  if (!timerStarted && !pumpInValue) {
    timerStartMillis = millis();
    timerStarted = true;
    displayOn = true;
    shotEventSent = false;
    currentShotValid = false;
    Serial.println("Start pump");
  }
  if (timerStarted && pumpInValue) {
    if (timerStopMillis == 0) {
      timerStopMillis = millis();
    }
    if (millis() - timerStopMillis > 500) {
      timerStarted = false;
      timerStopMillis = 0;
      timerDisplayOffMillis = millis();
      display.invertDisplay(false);
      Serial.println("Stop pump");
      // ── Added: send shot event if timer exceeded threshold ──
      if (currentShotValid && prevTimerCount <= 60 && !shotEventSent
          && (lastShotMillis == 0 || millis() - lastShotMillis >= SHOT_COOLDOWN_MS)) {
        sendShotEvent(prevTimerCount);
        shotEventSent = true;
        lastShotMillis = millis();
      }
    }
  } else {
    timerStopMillis = 0;
  }
  if (!timerStarted && displayOn && timerDisplayOffMillis > 0 && (millis() - timerDisplayOffMillis > 1000UL * 60 * 60)) {
    timerDisplayOffMillis = 0;
    timerCount = 0;
    prevTimerCount = 0;
    displayOn = false;
    Serial.println("Sleep");
  }
}

String getTimer() {
  char outMin[4];
  if (timerStarted) {
    timerCount = (millis() - timerStartMillis ) / 1000;
    if (timerCount > 25) {
      prevTimerCount = timerCount;
      currentShotValid = true;
    }
  } else {
    timerCount = prevTimerCount;
  }
  if (timerCount > 99) {
    return "99";
  }
  sprintf( outMin, "%02u", timerCount);
  return outMin;
}

void updateDisplay() {
  display.clearDisplay();
  if (displayOn) {
    if (timerStarted) {
      display.setTextSize(7);
      display.setCursor(25, 8);
      display.print(getTimer());
    } else {
      // draw line
      display.drawLine(74, 0, 74, 63, SSD1306_WHITE);
      // draw time seconds
      display.setTextSize(4);
      display.setCursor(display.width() / 2 - 1 + 17, 20);
      display.print(getTimer());
      // draw machine state C/S
      if (receivedChars[0] ) {
        display.setTextSize(2);
        display.setCursor(1, 1);
        if (String(receivedChars[0]) == "C") {
          display.print("C");
        } else if (String(receivedChars[0]) == "V") {
          display.print("S");
        } else {
          display.print("X");
        }
      }
      if (String(receivedChars).substring(18, 22) == "0000") {
        // not in boost heating mode
        // draw fill circle if heating on
        if (String(receivedChars[23]) == "1") {
          display.fillCircle(45, 7, 6, SSD1306_WHITE);
        }
        // draw empty circle if heating off
        if (String(receivedChars[23]) == "0") {
          display.drawCircle(45, 7, 6, SSD1306_WHITE);
        }
      } else {
        // in boost heating mode
        // draw fill rectangle if heating on
        if (String(receivedChars[23]) == "1") {
          display.fillRect(39, 1, 12, 12, SSD1306_WHITE);
        }
        // draw empty rectangle if heating off
        if (String(receivedChars[23]) == "0") {
          display.drawRect(39, 1, 12, 12, SSD1306_WHITE);
        }
      }
      // draw temperature
      if (receivedChars[14] && receivedChars[15] && receivedChars[16]) {
        display.setTextSize(3);
        display.setCursor(1, 20);
        if (String(receivedChars[14]) != "0") {
          display.print(String(receivedChars[14]));
        }
        display.print(String(receivedChars[15]));
        display.print(String(receivedChars[16]));
        display.print((char)247);
        if (String(receivedChars[14]) == "0") {
          display.print("C");
        }
      }
      // draw steam temperature
      if (receivedChars[6] && receivedChars[7] && receivedChars[8]) {
        display.setTextSize(2);
        display.setCursor(1, 48);
        if (String(receivedChars[6]) != "0") {
          display.print(String(receivedChars[6]));
        }
        display.print(String(receivedChars[7]));
        display.print(String(receivedChars[8]));
        display.print((char)247);
        display.print("C");
      }
      // ── Added: WiFi status icon ──
      drawWiFiIcon(62, 0, wifiConnected);
    }
  }
  display.display();
}
