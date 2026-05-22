/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║   ESP32-S3 + ILI9488 480×320 TFT — FaceGuard v1.2 (final)  ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  ALL OVERLAP FIXES:                                          ║
 * ║   • Every screen function starts with tft.fillScreen(C_BG)  ║
 * ║   • Every setTextColor() uses two args (fg, bg)             ║
 * ║   • fillRect() clears variable-length text rows before draw ║
 * ║   • screenIdle() called on EVERY transition back to S_IDLE  ║
 * ║   • screenWifi() restored and called in setup               ║
 * ║   • Idle text moved to bottom corners — out of the way      ║
 * ╚══════════════════════════════════════════════════════════════╝
 */

// ── FS first — fixes WebServer/FS namespace on ESP32 core 3.x ────
#include <FS.h>
using fs::FS;
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>

// ═══════════════════════════════════════════════════════════════════
//  ▶▶  EDIT THESE  ◀◀
// ═══════════════════════════════════════════════════════════════════
#define WIFI_SSID      "Kush"
#define WIFI_PASSWORD  "fortyfour"
#define SERVER_IP      "10.148.145.187"
#define SERVER_PORT    5000

// ═══════════════════════════════════════════════════════════════════
//  TIMING
// ═══════════════════════════════════════════════════════════════════
#define POLL_MS        800
#define STABLE_POLLS   2
#define NOTFOUND_MS    4000
#define CONFIRMED_MS   3500
#define TIMETABLE_MS   25000
#define FAILED_MS      3000

// ═══════════════════════════════════════════════════════════════════
//  COLORS (RGB565)
// ═══════════════════════════════════════════════════════════════════
#define C_BG           0x0000
#define C_SURF         0x0829
#define C_SURF2        0x0C31
#define C_BLUE         0x2777
#define C_GREEN        0x15E3
#define C_RED          0xF248
#define C_YELLOW       0xFEA0
#define C_ORANGE       0xFC4A
#define C_WHITE        0xFFFF
#define C_GRAY         0x8C17
#define C_DGRAY        0x2104

// Panel tones — used as text-bg to exactly match the panel fill
#define C_PANEL_BLUE   0x0040   // face-detected panel
#define C_PANEL_RED    0x2000   // not-found panel
#define C_PANEL_GREEN  0x0050   // confirmed panel

// ═══════════════════════════════════════════════════════════════════
//  STATE MACHINE
// ═══════════════════════════════════════════════════════════════════
enum State {
  S_WIFI, S_IDLE, S_FACE, S_NOTFOUND,
  S_MARKING, S_CONFIRMED, S_TIMETABLE, S_FAILED
};
State         state     = S_WIFI;
State         prevState = S_WIFI;
unsigned long stateTs   = 0;

// ═══════════════════════════════════════════════════════════════════
//  HARDWARE
// ═══════════════════════════════════════════════════════════════════
TFT_eSPI  tft;
WebServer localWeb(80);

// ═══════════════════════════════════════════════════════════════════
//  RUNTIME DATA
// ═══════════════════════════════════════════════════════════════════
String        recognizedName = "";
float         recognizedConf = 0.0f;
int           stableCount    = 0;
String        lastStableName = "";
unsigned long lastPoll       = 0;

struct Period { String label, time, subject, teacher; };
Period tt[10];
int    ttCount   = 0;
String ttDay     = "";
String ttSection = "";

// ═══════════════════════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════════════════════
void          setState(State s) { state = s; stateTs = millis(); }
unsigned long inState()         { return millis() - stateTs; }

// Header bar — always drawn fresh; bg=C_SURF so title never bleeds
void drawHeader(const char* title, uint16_t col) {
  tft.fillRect(0, 0, 480, 44, C_SURF);
  tft.drawFastHLine(0, 44, 480, col);
  tft.fillCircle(18, 22, 9, col);
  tft.fillCircle(18, 22, 4, C_SURF);
  tft.setTextColor(col, C_SURF);           // 2-arg: bg=C_SURF
  tft.drawString(title, 34, 13, 2);
  bool ok = (WiFi.status() == WL_CONNECTED);
  tft.fillCircle(466, 22, 7, ok ? C_GREEN : C_RED);
}

String buildURL(const char* path) {
  return String("http://") + SERVER_IP + ":" + SERVER_PORT + path;
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: WiFi Connecting
//  fillScreen at top guarantees a clean slate every call.
// ═══════════════════════════════════════════════════════════════════
void screenWifi() {
  tft.fillScreen(C_BG);                    // ← clears everything
  tft.fillRect(0, 0, 480, 5, C_BLUE);

  int cx = 240, cy = 130;
  for (int r = 14; r <= 60; r += 14) {
    for (int a = 210; a <= 330; a += 2) {
      float rad = a * PI / 180.0;
      tft.drawPixel(cx + (int)(r * cos(rad)), cy + (int)(r * sin(rad)),     C_BLUE);
      tft.drawPixel(cx + (int)(r * cos(rad)), cy + (int)(r * sin(rad)) - 1, C_BLUE);
    }
  }
  tft.fillCircle(cx, cy, 7, C_BLUE);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_WHITE, C_BG);         // 2-arg
  tft.drawString("Connecting to WiFi", cx, 178, 2);
  tft.setTextColor(C_GRAY, C_BG);          // 2-arg
  tft.drawString(WIFI_SSID, cx, 200, 2);
  tft.drawString("Please wait...", cx, 224, 1);
  tft.setTextDatum(TL_DATUM);
}

// Dot row updater — clears full 480px width before drawing
void updateWifiDots(int dotCount) {
  tft.fillRect(0, 230, 480, 35, C_BG);    // full-width clear
  tft.setTextColor(C_BLUE, C_BG);          // 2-arg
  tft.setTextDatum(MC_DATUM);
  String dotStr = "";
  for (int i = 0; i <= dotCount % 6; i++) dotStr += ".";
  tft.drawString(dotStr, 240, 248, 2);
  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Idle
//  Informational text is placed at BOTTOM CORNERS so it cannot
//  visually clash with any face/confirmed panel text.
//  Called on EVERY transition back to S_IDLE — see loop().
// ═══════════════════════════════════════════════════════════════════
void screenIdle() {
  tft.fillScreen(C_BG);                    // ← wipes previous screen
  drawHeader("FaceGuard  ·  Smart Attendance", C_BLUE);

  int cx = 240, cy = 155;

  // Camera icon — centred
  tft.fillRoundRect(cx - 80, cy - 52, 160, 110, 14, C_SURF);
  tft.drawRoundRect(cx - 80, cy - 52, 160, 110, 14, C_BLUE);
  tft.drawCircle(cx, cy, 36, C_BLUE);
  tft.drawCircle(cx, cy, 26, C_SURF2);
  tft.fillCircle(cx, cy, 16, C_BLUE);
  tft.fillCircle(cx, cy,  6, C_BG);
  tft.fillCircle(cx + 62, cy - 32, 8, C_YELLOW);
  tft.fillRoundRect(cx - 24, cy - 63, 48, 16, 7, C_SURF);
  tft.drawRoundRect(cx - 24, cy - 63, 48, 16, 7, C_BLUE);

  // Main instruction — centred, below icon
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_WHITE, C_BG);         // 2-arg
  tft.drawString("Please stand in front of the camera", cx, 230, 2);

  // Status — centred, small
  tft.setTextColor(C_BLUE, C_BG);          // 2-arg
  tft.drawString("System Ready  |  Polling server", cx, 256, 2);

  // Server IP — bottom-LEFT corner (small, out of the way)
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_GREEN, C_BG);         // 2-arg
  tft.drawString(String(SERVER_IP) + ":" + SERVER_PORT, 6, 304, 1);

  // Version — bottom-RIGHT corner
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_DGRAY, C_BG);         // 2-arg
  tft.drawString("FaceGuard v1.2  |  DSCE", 474, 304, 1);
  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Face Detected
// ═══════════════════════════════════════════════════════════════════
void screenFace() {
  tft.fillScreen(C_BG);                    // ← clears idle screen
  drawHeader("Face Detected", C_GREEN);
  int cx = 240;

  tft.fillRoundRect(40, 52, 400, 200, 14, C_PANEL_BLUE);
  tft.drawRoundRect(40, 52, 400, 200, 14, C_GREEN);

  for (int t = -2; t <= 2; t++) {
    tft.drawLine(cx - 52 + t, 148, cx - 12 + t, 196, C_GREEN);
    tft.drawLine(cx - 12 + t, 196, cx + 60 + t, 116, C_GREEN);
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_GREEN, C_PANEL_BLUE); // 2-arg: panel bg
  tft.drawString("Face Recognised!", cx, 208, 2);

  // Clear variable-length name row before drawing
  tft.fillRect(40, 210, 400, 36, C_PANEL_BLUE);
  tft.setTextColor(C_WHITE, C_PANEL_BLUE); // 2-arg: panel bg
  tft.drawString(recognizedName.c_str(), cx, 228, 4);

  // Clear confidence row (below panel, bg=C_BG)
  tft.fillRect(40, 250, 400, 22, C_BG);
  String confStr = "Confidence: " + String((int)(recognizedConf * 100)) + "%";
  tft.setTextColor(C_GRAY, C_BG);          // 2-arg
  tft.drawString(confStr.c_str(), cx, 262, 2);

  // Fingerprint prompt
  tft.setTextColor(C_YELLOW, C_BG);        // 2-arg
  tft.drawString("Place your finger on the", cx, 284, 2);
  tft.drawString("fingerprint scanner", cx, 304, 2);
  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Not Registered
// ═══════════════════════════════════════════════════════════════════
void screenNotFound() {
  tft.fillScreen(C_BG);                    // ← clears idle screen
  drawHeader("Not Registered", C_RED);
  int cx = 240, cy = 172;

  tft.fillRoundRect(40, 52, 400, 220, 14, C_PANEL_RED);
  tft.drawRoundRect(40, 52, 400, 220, 14, C_RED);
  for (int t = -2; t <= 2; t++) {
    tft.drawLine(cx - 52 + t, cy - 52, cx + 52 + t, cy + 52, C_RED);
    tft.drawLine(cx + 52 + t, cy - 52, cx - 52 + t, cy + 52, C_RED);
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_RED,  C_PANEL_RED);   // 2-arg: panel bg
  tft.drawString("Face Not Registered", cx, cy + 68, 4);
  tft.setTextColor(C_GRAY, C_BG);          // 2-arg: below panel
  tft.drawString("Please enroll at http://" + String(SERVER_IP) + ":5000", cx, cy + 102, 1);
  tft.drawString("Returning to scan...", cx, cy + 120, 1);
  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Marking Attendance
// ═══════════════════════════════════════════════════════════════════
void screenMarking() {
  tft.fillScreen(C_BG);                    // ← clears face screen
  drawHeader("Marking Attendance", C_YELLOW);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_WHITE, C_BG);         // 2-arg
  tft.drawString("Please wait...", 240, 150, 4);
  // Clear variable-length name row before drawing
  tft.fillRect(40, 184, 400, 24, C_BG);
  tft.setTextColor(C_YELLOW, C_BG);        // 2-arg
  tft.drawString(recognizedName.c_str(), 240, 196, 2);
  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Confirmed
// ═══════════════════════════════════════════════════════════════════
void screenConfirmed() {
  tft.fillScreen(C_BG);                    // ← clears marking screen
  drawHeader("Attendance Marked!", C_GREEN);
  int cx = 240;

  // Panel: y=52 to y=280
  tft.fillRoundRect(20, 52, 440, 228, 16, C_PANEL_GREEN);
  tft.drawRoundRect(20, 52, 440, 228, 16, C_GREEN);
  tft.drawRoundRect(22, 54, 436, 224, 16, 0x00A0);

  int sx = cx, sy = 70, sr = 42;
  tft.fillRoundRect(sx - sr, sy, sr * 2, (int)(sr * 1.4), 10, C_GREEN);
  for (int i = 0; i <= sr; i++)
    tft.drawFastHLine(sx - (sr - i + 1), sy + (int)(sr * 1.4) + i, (sr - i + 1) * 2, C_GREEN);
  for (int t = -2; t <= 2; t++) {
    tft.drawLine(sx - 26 + t, sy + 30, sx -  6 + t, sy + 52, C_PANEL_GREEN);
    tft.drawLine(sx -  6 + t, sy + 52, sx + 28 + t, sy + 22, C_PANEL_GREEN);
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_GREEN, C_PANEL_GREEN); // 2-arg: panel bg
  tft.drawString("Attendance Recorded", cx, 172, 2);
  tft.drawFastHLine(60, 194, 360, C_GREEN);

  // Clear variable-length name row before drawing
  tft.fillRect(20, 190, 440, 30, C_PANEL_GREEN);
  tft.setTextColor(C_WHITE, C_PANEL_GREEN); // 2-arg: panel bg
  tft.drawString(recognizedName.c_str(), cx, 203, 4);

  tft.setTextColor(C_GREEN, C_PANEL_GREEN); // 2-arg
  tft.drawString("Verified via Face Recognition", cx, 240, 2);

  if (ttCount > 0) {
    tft.setTextColor(C_BLUE, C_PANEL_GREEN);// 2-arg
    tft.drawString("Loading timetable...", cx, 262, 2);
  }

  // Below panel (y>280) — bg=C_BG
  tft.fillRect(20, 282, 440, 16, C_BG);
  tft.setTextColor(C_GRAY, C_BG);           // 2-arg
  tft.drawString("Section " + ttSection + "  |  " + ttDay, cx, 290, 1);
  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Timetable
// ═══════════════════════════════════════════════════════════════════
void screenTimetable() {
  tft.fillScreen(C_BG);                    // ← clears confirmed screen

  // Header bar
  tft.fillRect(0, 0, 480, 46, C_SURF);
  tft.drawFastHLine(0, 46, 480, C_BLUE);
  tft.setTextColor(C_BLUE, C_SURF);        // 2-arg: bar bg
  tft.drawString("Today's Timetable", 10, 8, 2);
  String sub = ttDay + "  |  Sec " + ttSection + "  |  " + recognizedName.substring(0, 18);
  tft.setTextColor(C_GRAY, C_SURF);        // 2-arg: bar bg
  tft.drawString(sub.c_str(), 10, 30, 1);
  tft.fillCircle(466, 22, 7, C_GREEN);

  if (ttCount == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_GRAY, C_BG);        // 2-arg
    tft.drawString("No classes today!", 240, 180, 4);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  // Column header bar
  tft.fillRect(0, 47, 480, 18, C_SURF2);
  tft.setTextColor(C_GRAY, C_SURF2);       // 2-arg: bar bg
  tft.drawString("Per  Time",  8,   51, 1);
  tft.drawString("Subject",    148, 51, 1);
  tft.drawString("Teacher",    310, 51, 1);
  tft.drawFastHLine(0, 65, 480, C_SURF);

  uint16_t stripeColors[] = {C_BLUE, C_GREEN, C_YELLOW, C_ORANGE, 0x07FF, C_RED};
  int maxRows = min(ttCount, 3);
  int rowH    = 72;
  int y       = 66;

  for (int i = 0; i < maxRows; i++) {
    uint16_t bg     = (i % 2 == 0) ? 0x080A : C_BG;
    uint16_t stripe = stripeColors[i % 6];
    tft.fillRect(0, y, 480, rowH - 1, bg); // fill row bg first
    tft.fillRect(0, y, 5, rowH - 1, stripe);
    tft.fillRoundRect(8, y + 6, 28, rowH - 14, 4, stripe);

    tft.setTextColor(C_BG, stripe);         // 2-arg: period pill bg
    tft.setTextDatum(MC_DATUM);
    tft.drawString(tt[i].label.c_str(), 22, y + rowH / 2, 1);
    tft.setTextDatum(TL_DATUM);

    tft.setTextColor(C_GRAY,  bg);          // 2-arg: row bg
    tft.drawString(tt[i].time.c_str(), 42, y + 6, 1);
    tft.setTextColor(C_WHITE, bg);          // 2-arg: row bg
    tft.drawString(tt[i].subject.substring(0, 18).c_str(), 148, y + 6, 2);
    tft.setTextColor(C_GRAY,  bg);          // 2-arg: row bg
    tft.drawString(tt[i].teacher.substring(0, 24).c_str(), 310, y + 6, 1);

    tft.drawFastHLine(0, y + rowH - 1, 480, C_SURF);
    y += rowH;
  }

  tft.drawFastHLine(0, y + 2, 480, C_SURF);
  tft.setTextColor(C_BLUE, C_BG);           // 2-arg
  tft.drawString(String(min(ttCount, 3)) + "/" + String(ttCount) + " periods shown", 8, y + 6, 1);

  // Countdown row — initial draw
  tft.fillRect(0, 298, 480, 22, C_BG);
  tft.setTextColor(C_GRAY, C_BG);           // 2-arg
  tft.setTextDatum(MC_DATUM);
  unsigned long secLeft = (TIMETABLE_MS - inState()) / 1000;
  tft.drawString("Auto-reset in " + String(secLeft) + "s", 240, 308, 1);
  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Network Error
// ═══════════════════════════════════════════════════════════════════
void screenFailed() {
  tft.fillScreen(C_BG);                    // ← clears whatever was shown
  drawHeader("Connection Error", C_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_RED,  C_BG);          // 2-arg
  tft.drawString("Cannot reach server", 240, 160, 4);
  tft.setTextColor(C_GRAY, C_BG);          // 2-arg
  tft.drawString(String(SERVER_IP) + ":" + SERVER_PORT, 240, 200, 2);
  tft.drawString("Check that run_all.py is running on your laptop", 240, 230, 1);
  tft.drawString("and that both devices are on the same WiFi",      240, 248, 1);
  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  API: Poll /api/esp32/poll
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

  const char* sv   = doc["state"]      | "idle";
  const char* name = doc["name"]       | "";
  float       conf = doc["confidence"] | 0.0f;

  if (strcmp(sv, "face_detected") == 0 && strlen(name) > 0 && strcmp(name, "Unknown") != 0) {
    if (String(name) == lastStableName) {
      stableCount++;
    } else {
      lastStableName = String(name);
      stableCount    = 1;
    }
    recognizedName = String(name);
    recognizedConf = conf;
    if (stableCount >= STABLE_POLLS && state == S_IDLE) return true;

  } else if (strcmp(sv, "not_registered") == 0 && state == S_IDLE) {
    recognizedName = "";
    stableCount    = 0;
    lastStableName = "";
    setState(S_NOTFOUND);
    screenNotFound();

  } else {
    if (stableCount > 0) stableCount--;
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════════
//  API: POST /api/attendance/mark
// ═══════════════════════════════════════════════════════════════════
void markAttendance() {
  if (WiFi.status() != WL_CONNECTED) {
    setState(S_FAILED);
    screenFailed();
    return;
  }
  setState(S_MARKING);
  screenMarking();

  HTTPClient http;
  http.begin(buildURL("/api/attendance/mark"));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(6000);

  StaticJsonDocument<128> req;
  req["name"]   = recognizedName;
  req["method"] = "face_recognition";
  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[ATT] HTTP error: %d\n", code);
    http.end();
    setState(S_FAILED);
    screenFailed();
    return;
  }

  String resp = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, resp)) {
    Serial.println("[ATT] JSON parse failed");
    setState(S_CONFIRMED);
    screenConfirmed();
    return;
  }

  ttCount = 0; ttDay = ""; ttSection = "";
  if (doc["success"]) {
    JsonObject ttObj = doc["timetable"];
    if (!ttObj.isNull()) {
      ttDay     = ttObj["day"]             | "";
      ttSection = doc["record"]["section"] | "";
      JsonArray classes = ttObj["classes"];
      for (JsonObject c : classes) {
        if (ttCount >= 10) break;
        tt[ttCount].label   = c["period"]  | "";
        tt[ttCount].time    = c["time"]    | "";
        tt[ttCount].subject = c["subject"] | "";
        tt[ttCount].teacher = c["teacher"] | "";
        ttCount++;
      }
    }
    Serial.printf("[ATT] Marked: %s | %d classes\n", recognizedName.c_str(), ttCount);
  }
  setState(S_CONFIRMED);
  screenConfirmed();
}

// ═══════════════════════════════════════════════════════════════════
//  LOCAL WEB DASHBOARD
// ═══════════════════════════════════════════════════════════════════
void webRoot() {
  const char* stateNames[] = {
    "WiFi connecting", "Idle — polling", "Face detected",
    "Not registered",  "Marking",        "Confirmed",
    "Timetable",       "Network error"
  };
  String html =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta http-equiv='refresh' content='3'>"
    "<title>FaceGuard ESP32</title>"
    "<style>body{font-family:monospace;background:#0a0f1e;color:#e2e8f0;padding:20px}"
    "h1{color:#4f8ef7}td{padding:6px 16px}th{text-align:left;color:#4a6fa5}"
    ".g{color:#22d3a0}.r{color:#f45454}</style></head><body>"
    "<h1>FaceGuard Kiosk</h1><table>"
    "<tr><th>State</th><td class='g'>"     + String(stateNames[(int)state])                    + "</td></tr>"
    "<tr><th>Detected</th><td>"            + (recognizedName.length() ? recognizedName : "—")  + "</td></tr>"
    "<tr><th>Confidence</th><td>"          + String((int)(recognizedConf * 100))               + "%</td></tr>"
    "<tr><th>Server</th><td>"              + SERVER_IP + ":" + SERVER_PORT                     + "</td></tr>"
    "<tr><th>WiFi IP</th><td>"             + WiFi.localIP().toString()                         + "</td></tr>"
    "<tr><th>Uptime</th><td>"              + String(millis() / 1000)                           + "s</td></tr>"
    "</table><p style='color:#4a6568;font-size:12px'>Auto-refreshes every 3s</p>"
    "</body></html>";
  localWeb.send(200, "text/html", html);
}

// ═══════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(200);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);
  Serial.println("[TFT] OK");

  setState(S_WIFI);
  screenWifi();                            // ← show WiFi screen

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && dots < 60) {  // 30 s timeout
    delay(500);
    updateWifiDots(dots);
    dots++;
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] FAILED");
    tft.fillRect(0, 230, 480, 90, C_BG);  // clear dot row first
    tft.setTextColor(C_RED, C_BG);         // 2-arg
    tft.setTextDatum(MC_DATUM);
    tft.drawString("WiFi FAILED — check credentials", 240, 260, 2);
    tft.setTextDatum(TL_DATUM);
    delay(5000);
    // Falls through to screenIdle() below — screen is never left on WiFi bg
  }

  localWeb.on("/", webRoot);
  localWeb.begin();
  Serial.printf("[Web]    http://%s\n",       WiFi.localIP().toString().c_str());
  Serial.printf("[Config] Flask: http://%s:%d\n", SERVER_IP, SERVER_PORT);

  setState(S_IDLE);
  screenIdle();                            // ← always clears WiFi screen
}

// ═══════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════
void loop() {
  localWeb.handleClient();
  unsigned long now = millis();

  switch (state) {

    // ── Idle: poll server every POLL_MS ──────────────────────────
    case S_IDLE:
      if (now - lastPoll >= POLL_MS) {
        lastPoll = now;
        bool faceConfirmed = pollServer();
        if (faceConfirmed) {
          setState(S_FACE);
          screenFace();
          delay(1400);
          markAttendance();
        }
      }
      break;

    // ── Not found: auto-dismiss after NOTFOUND_MS ────────────────
    case S_NOTFOUND:
      if (inState() >= NOTFOUND_MS) {
        stableCount    = 0;
        lastStableName = "";
        recognizedName = "";
        setState(S_IDLE);
        screenIdle();                      // ← clears not-found screen
      }
      break;

    // ── Confirmed: wait then show timetable or return to idle ────
    case S_CONFIRMED:
      if (inState() >= CONFIRMED_MS) {
        if (ttCount > 0) {
          setState(S_TIMETABLE);
          screenTimetable();
        } else {
          stableCount    = 0;
          lastStableName = "";
          recognizedName = "";
          setState(S_IDLE);
          screenIdle();                    // ← clears confirmed screen
        }
      }
      break;

    // ── Timetable: update countdown once/second, then idle ───────
    case S_TIMETABLE: {
      static unsigned long lastTTSec = 0;
      unsigned long curSec = millis() / 1000;
      if (curSec != lastTTSec) {          // fires exactly once per second
        lastTTSec = curSec;
        unsigned long secLeft = (TIMETABLE_MS - inState()) / 1000;
        tft.fillRect(0, 298, 480, 22, C_BG); // full-width clear
        tft.setTextColor(C_GRAY, C_BG);    // 2-arg
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Auto-reset in " + String(secLeft) + "s", 240, 308, 1);
        tft.setTextDatum(TL_DATUM);
      }
      if (inState() >= TIMETABLE_MS) {
        stableCount    = 0;
        lastStableName = "";
        recognizedName = "";
        ttCount        = 0;
        setState(S_IDLE);
        screenIdle();                      // ← clears timetable screen
      }
      break;
    }

    // ── Failed: auto-dismiss after FAILED_MS ─────────────────────
    case S_FAILED:
      if (inState() >= FAILED_MS) {
        stableCount    = 0;
        lastStableName = "";
        recognizedName = "";
        setState(S_IDLE);
        screenIdle();                      // ← clears error screen
      }
      break;

    // ── These states are blocking (managed by their own functions) 
    case S_MARKING:
    case S_FACE:
    case S_WIFI:
    default:
      break;
  }

  delay(30);
}
