/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║   ESP32-S3 + 16x2 I2C LCD + R307 — FaceGuard LCD v2.0      ║
 * ║   Face recognition  →  Fingerprint verify  →  Mark          ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  WIRING                                                      ║
 * ║    LCD GND    → GND          R307 GND  → GND                ║
 * ║    LCD VCC    → 3.3V         R307 VCC  → 5V (preferred)     ║
 * ║    LCD SDA    → GPIO 8       R307 TX   → GPIO 18            ║
 * ║    LCD SCL    → GPIO 9       R307 RX   → GPIO 17            ║
 * ║    LCD LED    → 3.3V         R307 WAKE → 3.3V (or float)    ║
 * ║                                                              ║
 * ║  FLOW                                                        ║
 * ║    Idle          → "Please stand in  / front of camera"     ║
 * ║    Face seen     → "Checking...     / <name>"               ║
 * ║    Face OK       → "Identity        / Verified!"            ║
 * ║    FP prompt     → "Place thumb on  / scanner..."           ║
 * ║    FP matched    → "Attendance      / Marked!"              ║
 * ║    FP not found  → "Finger not      / registered!"          ║
 * ║    FP timeout    → "Scan timeout    / Try again"            ║
 * ║    Not in DB     → "Not Registered  / Enroll on server"     ║
 * ║    Error         → "Server Error    / Try again"            ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  SERIAL MONITOR COMMANDS (115200 baud, newline ending)      ║
 * ║    ENROLL <id>   register finger at slot 1-127              ║
 * ║    DELETE <id>   remove a slot                              ║
 * ║    LIST          show all occupied slots                    ║
 * ║    CLEAR         wipe entire R307 database                  ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * Libraries (Arduino Library Manager):
 *   • LiquidCrystal_I2C          by Frank de Brabander
 *   • ArduinoJson                by Benoit Blanchon
 *   • Adafruit Fingerprint Sensor by Adafruit
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_Fingerprint.h>

// ═══════════════════════════════════════════════════════════════════
//  ▶▶  EDIT THESE  ◀◀
// ═══════════════════════════════════════════════════════════════════
#define WIFI_SSID       "Kush"
#define WIFI_PASSWORD   "fortyfour"
#define SERVER_IP       "10.148.145.187"
#define SERVER_PORT     5000

// I2C pins
#define I2C_SDA         8
#define I2C_SCL         9

// I2C address — try 0x3F if LCD shows nothing
#define LCD_I2C_ADDR    0x27

// R307 UART pins
#define FP_RX_PIN       18   // ESP32 RX  ←  R307 TX  (green wire)
#define FP_TX_PIN       17   // ESP32 TX  →  R307 RX  (white wire)

// ═══════════════════════════════════════════════════════════════════
//  TIMING (ms)
// ═══════════════════════════════════════════════════════════════════
#define POLL_MS           1000   // server poll interval
#define STABLE_POLLS         2   // consecutive detections before acting
#define MSG_HOLD_MS       2500   // how long result stays on screen
#define FP_TIMEOUT_MS    15000   // give up waiting for thumb after this

// ═══════════════════════════════════════════════════════════════════
//  STATE MACHINE
// ═══════════════════════════════════════════════════════════════════
enum State {
  S_WIFI,        // connecting to WiFi
  S_IDLE,        // polling server, waiting for face
  S_CHECKING,    // face detected, building stable count
  S_VERIFIED,    // face identity confirmed
  S_FP_SCAN,     // waiting for thumb on R307
  S_FP_OK,       // fingerprint matched in storage
  S_FP_FAIL,     // fingerprint not in storage
  S_FP_TIMEOUT,  // no thumb placed in time
  S_MARKED,      // attendance posted successfully
  S_NOTFOUND,    // face not registered on server
  S_FAILED,      // server / network error
  S_ENROLLING    // mid-enrolment (blocks normal flow)
};

State         state      = S_WIFI;
unsigned long stateTs    = 0;
void          setState(State s) { state = s; stateTs = millis(); }
unsigned long inState()         { return millis() - stateTs; }

// ═══════════════════════════════════════════════════════════════════
//  HARDWARE
// ═══════════════════════════════════════════════════════════════════
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);

HardwareSerial    fpSerial(1);
Adafruit_Fingerprint finger(&fpSerial);

// ═══════════════════════════════════════════════════════════════════
//  RUNTIME DATA
// ═══════════════════════════════════════════════════════════════════
String        recognizedName = "";
int           stableCount    = 0;
String        lastStableName = "";
unsigned long lastPoll       = 0;

// ═══════════════════════════════════════════════════════════════════
//  LCD HELPERS
// ═══════════════════════════════════════════════════════════════════
void lcdPrint(const char* line1, const char* line2 = "") {
  char l1[17], l2[17];
  snprintf(l1, sizeof(l1), "%-16s", line1);
  snprintf(l2, sizeof(l2), "%-16s", line2);
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
}

void lcdLine2(const char* line2) {
  char l2[17];
  snprintf(l2, sizeof(l2), "%-16s", line2);
  lcd.setCursor(0, 1);
  lcd.print(l2);
}

// ═══════════════════════════════════════════════════════════════════
//  NETWORK HELPERS
// ═══════════════════════════════════════════════════════════════════
String buildURL(const char* path) {
  return String("http://") + SERVER_IP + ":" + SERVER_PORT + path;
}

// ═══════════════════════════════════════════════════════════════════
//  API: GET /api/esp32/poll
// ═══════════════════════════════════════════════════════════════════
bool pollServer() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(buildURL("/api/esp32/poll"));
  http.setTimeout(2000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  String body = http.getString();
  http.end();

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) return false;

  const char* sv   = doc["state"] | "idle";
  const char* name = doc["name"]  | "";

  if (strcmp(sv, "face_detected") == 0
      && strlen(name) > 0
      && strcmp(name, "Unknown") != 0) {

    if (String(name) == lastStableName) {
      stableCount++;
    } else {
      lastStableName = String(name);
      stableCount    = 1;
    }
    recognizedName = String(name);

    if (stableCount >= STABLE_POLLS && state == S_IDLE) return true;

  } else if (strcmp(sv, "not_registered") == 0 && state == S_IDLE) {
    recognizedName = "";
    stableCount    = 0;
    lastStableName = "";
    setState(S_NOTFOUND);
    lcdPrint("Not Registered", "Enroll on server");

  } else {
    if (stableCount > 0) stableCount--;
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════════
//  API: POST /api/attendance/mark
// ═══════════════════════════════════════════════════════════════════
bool markAttendance() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(buildURL("/api/attendance/mark"));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(6000);

  StaticJsonDocument<128> req;
  req["name"]   = recognizedName;
  req["method"] = "face_and_fingerprint";
  String body;
  serializeJson(req, body);

  int    code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code != 200) {
    Serial.printf("[ATT] HTTP %d\n", code);
    return false;
  }

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, resp)) return false;
  return doc["success"] | false;
}

// ═══════════════════════════════════════════════════════════════════
//  FINGERPRINT — one-shot search
//  Returns slot ID (1-127) on match, -1 on no finger / no match
// ═══════════════════════════════════════════════════════════════════
int fingerprintSearch() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;          // no finger yet

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    Serial.printf("[FP] Match slot=%d  confidence=%d\n",
                  finger.fingerID, finger.confidence);
    return finger.fingerID;
  }
  Serial.println("[FP] No match in storage");
  return -2;    // -2 = finger present but no match (distinct from -1 = no finger)
}

// ═══════════════════════════════════════════════════════════════════
//  FINGERPRINT — enrolment (called from Serial command)
// ═══════════════════════════════════════════════════════════════════
void enrollFinger(uint8_t id) {
  state = S_ENROLLING;
  Serial.printf("[ENROLL] Starting slot %d\n", id);
  lcdPrint("Enroll mode", ("Slot #" + String(id)).c_str());

  uint8_t p;
  unsigned long t;

  // ── Scan 1 ──────────────────────────────────────────────
  lcdPrint("Place finger", "on scanner");
  Serial.println("[ENROLL] Place finger on scanner...");
  t = millis();
  while (millis() - t < 15000) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    delay(100);
  }
  if (p != FINGERPRINT_OK) { goto enroll_timeout; }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) { goto enroll_fail; }

  // ── Wait for lift ────────────────────────────────────────
  lcdPrint("Lift finger", "then place again");
  Serial.println("[ENROLL] Lift finger...");
  delay(1200);
  t = millis();
  while (millis() - t < 8000) {
    if (finger.getImage() == FINGERPRINT_NOFINGER) break;
    delay(100);
  }

  // ── Scan 2 ──────────────────────────────────────────────
  lcdPrint("Place same", "finger again");
  Serial.println("[ENROLL] Place same finger again...");
  t = millis();
  p = FINGERPRINT_NOFINGER;
  while (millis() - t < 15000) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    delay(100);
  }
  if (p != FINGERPRINT_OK) { goto enroll_timeout; }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) { goto enroll_fail; }

  // ── Create model ─────────────────────────────────────────
  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    Serial.println("[ENROLL] Scans didn't match — try again");
    lcdPrint("Scans differ!", "Try again");
    delay(2500);
    goto enroll_done;
  }

  // ── Store ────────────────────────────────────────────────
  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.printf("[ENROLL] Stored at slot %d\n", id);
    lcdPrint("Enrolled OK!", ("Slot #" + String(id)).c_str());
  } else {
    Serial.println("[ENROLL] Store failed");
    lcdPrint("Store failed!", "");
  }
  delay(2500);
  goto enroll_done;

enroll_timeout:
  Serial.println("[ENROLL] Timeout");
  lcdPrint("Timeout!", "Try again");
  delay(2000);
  goto enroll_done;

enroll_fail:
  Serial.println("[ENROLL] Scan failed");
  lcdPrint("Scan failed!", "Try again");
  delay(2000);

enroll_done:
  goIdle();
}

// ═══════════════════════════════════════════════════════════════════
//  FINGERPRINT — management helpers
// ═══════════════════════════════════════════════════════════════════
void listFingerprints() {
  Serial.println("[LIST] Scanning R307...");
  int count = 0;
  for (uint8_t id = 1; id <= 127; id++) {
    if (finger.loadModel(id) == FINGERPRINT_OK) {
      Serial.printf("  Slot %d\n", id);
      count++;
    }
  }
  Serial.printf("  Total: %d\n", count ? count : 0);
  if (!count) Serial.println("  (no fingerprints stored)");
}

void deleteFinger(uint8_t id) {
  uint8_t p = finger.deleteModel(id);
  Serial.printf("[DELETE] Slot %d %s\n", id,
                p == FINGERPRINT_OK ? "deleted" : "FAILED");
}

void clearAll() {
  uint8_t p = finger.emptyDatabase();
  Serial.println(p == FINGERPRINT_OK
    ? "[CLEAR] All fingerprints wiped"
    : "[CLEAR] FAILED");
}

// ═══════════════════════════════════════════════════════════════════
//  SERIAL COMMAND PARSER
// ═══════════════════════════════════════════════════════════════════
void handleSerial(String cmd) {
  cmd.trim();
  String upper = cmd;
  upper.toUpperCase();

  if (upper.startsWith("ENROLL ")) {
    int id = upper.substring(7).toInt();
    if (id < 1 || id > 127) { Serial.println("[CMD] ID must be 1-127"); return; }
    enrollFinger((uint8_t)id);

  } else if (upper.startsWith("DELETE ")) {
    int id = upper.substring(7).toInt();
    if (id < 1 || id > 127) { Serial.println("[CMD] ID must be 1-127"); return; }
    deleteFinger((uint8_t)id);

  } else if (upper == "LIST") {
    listFingerprints();

  } else if (upper == "CLEAR") {
    Serial.println("[CMD] Wiping in 3 s — power-cycle to abort");
    delay(3000);
    clearAll();

  } else {
    Serial.println("[CMD] Commands: ENROLL <id> | DELETE <id> | LIST | CLEAR");
  }
}

// ═══════════════════════════════════════════════════════════════════
//  Reset to idle
// ═══════════════════════════════════════════════════════════════════
void goIdle() {
  stableCount    = 0;
  lastStableName = "";
  recognizedName = "";
  setState(S_IDLE);
  lcdPrint("Please stand in", "front of camera");
}

// ═══════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== FaceGuard v2.0 ===");
  Serial.println("Commands: ENROLL <id> | DELETE <id> | LIST | CLEAR");

  // LCD
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcdPrint("FaceGuard v2.0", "Starting...");
  Serial.println("[LCD] OK");
  delay(600);

  // Fingerprint sensor
  fpSerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  delay(200);
  if (finger.verifyPassword()) {
    finger.getTemplateCount();
    Serial.printf("[FP] R307 ready — %d template(s) stored\n",
                  finger.templateCount);
    lcdPrint("Fingerprint OK", ("Slots: " + String(finger.templateCount)).c_str());
  } else {
    Serial.println("[FP] R307 NOT FOUND — check wiring!");
    lcdPrint("FP sensor", "NOT FOUND!");
    delay(3000);
    // Not fatal — continue without FP (attendance won't complete)
  }
  delay(800);

  // WiFi
  lcdPrint("Connecting WiFi", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    attempts++;
    Serial.print(".");
    const char* dots = (attempts % 3 == 0) ? "."
                     : (attempts % 3 == 1) ? ".."
                     :                       "...";
    lcdLine2(dots);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] %s\n", WiFi.localIP().toString().c_str());
    lcdPrint("WiFi Connected!", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WiFi] FAILED");
    lcdPrint("WiFi FAILED", "Check password");
    delay(4000);
  }
  delay(1200);

  goIdle();
}

// ═══════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // ── Serial commands always get priority ──────────────────
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    handleSerial(cmd);
    return;
  }

  switch (state) {

    // ── IDLE: poll until face is stably recognised ───────────
    case S_IDLE:
      if (now - lastPoll >= POLL_MS) {
        lastPoll = now;
        if (pollServer()) {
          setState(S_CHECKING);
          lcdPrint("Checking...", recognizedName.substring(0, 16).c_str());
          delay(1000);

          // Face confirmed
          setState(S_VERIFIED);
          lcdPrint("Identity", "Verified!");
          Serial.printf("[FACE] Verified: %s\n", recognizedName.c_str());
          delay(MSG_HOLD_MS);

          // Move to fingerprint phase
          setState(S_FP_SCAN);
          lcdPrint("Place thumb on", "scanner...");
        }
      }
      break;

    // ── FP_SCAN: wait for thumb, search R307 storage ─────────
    case S_FP_SCAN: {
      // Timeout guard
      if (inState() >= FP_TIMEOUT_MS) {
        Serial.println("[FP] Scan timeout");
        setState(S_FP_TIMEOUT);
        lcdPrint("Scan timeout!", "Try again");
        delay(MSG_HOLD_MS);
        goIdle();
        break;
      }

      int result = fingerprintSearch();

      if (result > 0) {
        // ── Fingerprint found in storage ─────────────────────
        setState(S_FP_OK);
        lcdPrint("Identity", "Verified!");
        delay(MSG_HOLD_MS);

        // Post attendance to server
        lcdPrint("Marking...", recognizedName.substring(0, 16).c_str());
        if (markAttendance()) {
          setState(S_MARKED);
          lcdPrint("Attendance", "Marked!  :)");
          Serial.println("[ATT] Marked OK");
        } else {
          setState(S_FAILED);
          lcdPrint("Server Error", "Try again");
          Serial.println("[ATT] Mark failed");
        }
        delay(MSG_HOLD_MS);
        goIdle();

      } else if (result == -2) {
        // ── Finger present but not registered ────────────────
        setState(S_FP_FAIL);
        lcdPrint("Finger not", "registered!");
        Serial.println("[FP] Not in storage");
        delay(MSG_HOLD_MS);
        goIdle();
      }
      // result == -1 means no finger yet — keep looping

      delay(80);
      break;
    }

    // ── NOTFOUND: face not in server DB ──────────────────────
    case S_NOTFOUND:
      if (inState() >= 3000) goIdle();
      break;

    // ── FAILED: server error ──────────────────────────────────
    case S_FAILED:
      if (inState() >= MSG_HOLD_MS) goIdle();
      break;

    // Synchronous states — handled inline above
    case S_WIFI:
    case S_CHECKING:
    case S_VERIFIED:
    case S_FP_OK:
    case S_FP_FAIL:
    case S_FP_TIMEOUT:
    case S_MARKED:
    case S_ENROLLING:
    default:
      break;
  }

  delay(50);
}
