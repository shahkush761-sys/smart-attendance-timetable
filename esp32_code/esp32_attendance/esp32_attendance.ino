/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║   ESP32-S3 + ILI9488 480×320 TFT — FaceGuard v1.4 (FINAL)  ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  ROOT CAUSE OF ALL OVERLAPS (found in v1.3 → fixed here):   ║
 * ║                                                              ║
 * ║  setTextDatum(MC_DATUM) was active, which means the Y        ║
 * ║  passed to drawString() is the TEXT CENTER, not the top.     ║
 * ║  But clearText() was treating Y as the TOP → fillRect()      ║
 * ║  cleared the WRONG area every time, always missing the       ║
 * ║  upper half of the previous glyph → ghost pixels remained.  ║
 * ║                                                              ║
 * ║  FIX: clearText() now treats Y as the CENTER (MC_DATUM)     ║
 * ║  and offsets fillRect up by h/2 so it covers the full glyph.║
 * ║  Same Y value is passed to BOTH clearText() and drawString() ║
 * ║  so they always clear and draw in the same spot.             ║
 * ║                                                              ║
 * ║  PIN CONFIG (goes in TFT_eSPI User_Setup.h):                 ║
 * ║    #define ILI9488_DRIVER                                    ║
 * ║    #define USE_FSPI_PORT                                     ║
 * ║    #define TFT_MOSI  11   #define TFT_MISO  13              ║
 * ║    #define TFT_SCLK  12   #define TFT_CS    10              ║
 * ║    #define TFT_DC     9   #define TFT_RST    8              ║
 * ║    #define TFT_BL    46   #define TFT_BACKLIGHT_ON HIGH     ║
 * ║    #define SPI_FREQUENCY  27000000                           ║
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

// Panel tones — MUST exactly match fillRoundRect() fill colour
#define C_PANEL_BLUE   0x0040
#define C_PANEL_RED    0x2000
#define C_PANEL_GREEN  0x0050

// ═══════════════════════════════════════════════════════════════════
//  FONT HALF-HEIGHTS (used by clearText to offset fillRect)
//  TFT_eSPI built-in fonts:  Font1=8px  Font2=16px  Font4=26px
//  Half-height = pixel height / 2, rounded up, + 2px safety margin
// ═══════════════════════════════════════════════════════════════════
#define FHH1   6    // half-height of font 1  (8/2 + 2)
#define FHH2  10    // half-height of font 2  (16/2 + 2)
#define FHH4  15    // half-height of font 4  (26/2 + 2)

// Full height for the fillRect (2× half + 2px bottom pad)
#define FH1   (FHH1*2 + 2)
#define FH2   (FHH2*2 + 2)
#define FH4   (FHH4*2 + 2)

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

/*
 * clearText() — THE CORE FIX
 * ─────────────────────────────────────────────────────────────────
 * Call BEFORE drawString() for any variable-length text.
 *
 * IMPORTANT: Y is the MC_DATUM centre coordinate — the same value
 * you will pass to drawString().  The fillRect is offset UPWARD by
 * half the font height so it covers the full glyph (top + bottom).
 *
 *   x, yCenter : same coordinates you pass to drawString()
 *   w          : width to erase (panel width or 480 for full-screen)
 *   font       : 1 / 2 / 4 — must match drawString() font arg
 *   bg         : background colour behind the text
 */
void clearText(int x, int yCenter, int w, int font, uint16_t bg) {
  int hh = (font == 4) ? FHH4 : (font == 2) ? FHH2 : FHH1;
  int h  = hh * 2 + 2;
  // fillRect from (yCenter - halfHeight - 1) so the full glyph is wiped
  tft.fillRect(x, yCenter - hh - 1, w, h, bg);
}

// Header bar — always redrawn from scratch; bg=C_SURF prevents bleed
void drawHeader(const char* title, uint16_t col) {
  tft.fillRect(0, 0, 480, 44, C_SURF);
  tft.drawFastHLine(0, 44, 480, col);
  tft.fillCircle(18, 22, 9, col);
  tft.fillCircle(18, 22, 4, C_SURF);
  tft.setTextColor(col, C_SURF);       // 2-arg — bg = C_SURF
  tft.drawString(title, 34, 13, 2);
  bool ok = (WiFi.status() == WL_CONNECTED);
  tft.fillCircle(466, 22, 7, ok ? C_GREEN : C_RED);
}

String buildURL(const char* path) {
  return String("http://") + SERVER_IP + ":" + SERVER_PORT + path;
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: WiFi Connecting
// ═══════════════════════════════════════════════════════════════════
void screenWifi() {
  tft.fillScreen(C_BG);
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
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawString("Connecting to WiFi", cx, 178, 2);
  tft.setTextColor(C_GRAY, C_BG);
  tft.drawString(WIFI_SSID, cx, 200, 2);
  tft.drawString("Please wait...", cx, 224, 1);
  tft.setTextDatum(TL_DATUM);
}

// Clears the dot row and redraws — full 480 px width prevents ghosts
void updateWifiDots(int dotCount) {
  tft.fillRect(0, 230, 480, 35, C_BG);
  tft.setTextColor(C_BLUE, C_BG);
  tft.setTextDatum(MC_DATUM);
  String dotStr = "";
  for (int i = 0; i <= dotCount % 6; i++) dotStr += ".";
  tft.drawString(dotStr, 240, 248, 2);
  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Idle
//  Static text only — drawn once after fillScreen.
//  Info text in BOTTOM CORNERS so it never overlaps face panels.
// ═══════════════════════════════════════════════════════════════════
void screenIdle() {
  tft.fillScreen(C_BG);                      // wipes every previous screen
  drawHeader("FaceGuard  ·  Smart Attendance", C_BLUE);

  int cx = 240, cy = 155;

  // Camera icon
  tft.fillRoundRect(cx - 80, cy - 52, 160, 110, 14, C_SURF);
  tft.drawRoundRect(cx - 80, cy - 52, 160, 110, 14, C_BLUE);
  tft.drawCircle(cx, cy, 36, C_BLUE);
  tft.drawCircle(cx, cy, 26, C_SURF2);
  tft.fillCircle(cx, cy, 16, C_BLUE);
  tft.fillCircle(cx, cy,  6, C_BG);
  tft.fillCircle(cx + 62, cy - 32, 8, C_YELLOW);
  tft.fillRoundRect(cx - 24, cy - 63, 48, 16, 7, C_SURF);
  tft.drawRoundRect(cx - 24, cy - 63, 48, 16, 7, C_BLUE);

  // Instruction — centred below icon (static, drawn once)
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawString("Please stand in front of the camera", cx, 230, 2);
  tft.setTextColor(C_BLUE, C_BG);
  tft.drawString("System Ready  |  Polling server", cx, 256, 2);

  // Server IP — bottom-LEFT corner
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_GREEN, C_BG);
  tft.drawString(String(SERVER_IP) + ":" + SERVER_PORT, 6, 304, 1);

  // Version — bottom-RIGHT corner
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_DGRAY, C_BG);
  tft.drawString("FaceGuard v1.4  |  DSCE", 474, 304, 1);
  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Face Detected
// ═══════════════════════════════════════════════════════════════════
void screenFace() {
  tft.fillScreen(C_BG);
  drawHeader("Face Detected", C_GREEN);
  int cx = 240;

  // Panel: y=52 … y=252
  tft.fillRoundRect(40, 52, 400, 200, 14, C_PANEL_BLUE);
  tft.drawRoundRect(40, 52, 400, 200, 14, C_GREEN);

  // Checkmark
  for (int t = -2; t <= 2; t++) {
    tft.drawLine(cx - 52 + t, 148, cx - 12 + t, 196, C_GREEN);
    tft.drawLine(cx - 12 + t, 196, cx + 60 + t, 116, C_GREEN);
  }

  tft.setTextDatum(MC_DATUM);

  // "Face Recognised!" — font 2, y=208 centre, panel bg
  clearText(40, 208, 400, 2, C_PANEL_BLUE);
  tft.setTextColor(C_GREEN, C_PANEL_BLUE);
  tft.drawString("Face Recognised!", cx, 208, 2);

  // Name — font 4, y=230 centre, panel bg
  clearText(40, 230, 400, 4, C_PANEL_BLUE);
  tft.setTextColor(C_WHITE, C_PANEL_BLUE);
  tft.drawString(recognizedName.c_str(), cx, 230, 4);

  // Confidence — font 2, y=262 centre, below panel → bg=C_BG
  String confStr = "Confidence: " + String((int)(recognizedConf * 100)) + "%";
  clearText(40, 262, 400, 2, C_BG);
  tft.setTextColor(C_GRAY, C_BG);
  tft.drawString(confStr.c_str(), cx, 262, 2);

  // Fingerprint prompt — font 2, below panel → bg=C_BG
  clearText(40, 284, 400, 2, C_BG);
  tft.setTextColor(C_YELLOW, C_BG);
  tft.drawString("Place your finger on the", cx, 284, 2);

  clearText(40, 304, 400, 2, C_BG);
  tft.drawString("fingerprint scanner", cx, 304, 2);

  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Not Registered
// ═══════════════════════════════════════════════════════════════════
void screenNotFound() {
  tft.fillScreen(C_BG);
  drawHeader("Not Registered", C_RED);
  int cx = 240, cy = 172;

  // Panel: y=52 … y=272
  tft.fillRoundRect(40, 52, 400, 220, 14, C_PANEL_RED);
  tft.drawRoundRect(40, 52, 400, 220, 14, C_RED);

  // X mark
  for (int t = -2; t <= 2; t++) {
    tft.drawLine(cx - 52 + t, cy - 52, cx + 52 + t, cy + 52, C_RED);
    tft.drawLine(cx + 52 + t, cy - 52, cx - 52 + t, cy + 52, C_RED);
  }

  tft.setTextDatum(MC_DATUM);

  // "Face Not Registered" — font 4, inside panel
  clearText(40, cy + 68, 400, 4, C_PANEL_RED);
  tft.setTextColor(C_RED, C_PANEL_RED);
  tft.drawString("Face Not Registered", cx, cy + 68, 4);

  // Help text — font 1, below panel → bg=C_BG
  clearText(40, cy + 102, 400, 1, C_BG);
  tft.setTextColor(C_GRAY, C_BG);
  tft.drawString("Please enroll at http://" + String(SERVER_IP) + ":5000", cx, cy + 102, 1);

  clearText(40, cy + 118, 400, 1, C_BG);
  tft.drawString("Returning to scan...", cx, cy + 118, 1);

  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Marking Attendance
// ═══════════════════════════════════════════════════════════════════
void screenMarking() {
  tft.fillScreen(C_BG);
  drawHeader("Marking Attendance", C_YELLOW);
  tft.setTextDatum(MC_DATUM);

  // "Please wait..." — font 4
  clearText(0, 150, 480, 4, C_BG);
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawString("Please wait...", 240, 150, 4);

  // Name — font 2
  clearText(40, 196, 400, 2, C_BG);
  tft.setTextColor(C_YELLOW, C_BG);
  tft.drawString(recognizedName.c_str(), 240, 196, 2);

  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Confirmed
// ═══════════════════════════════════════════════════════════════════
void screenConfirmed() {
  tft.fillScreen(C_BG);
  drawHeader("Attendance Marked!", C_GREEN);
  int cx = 240;

  // Panel: y=52 … y=280
  tft.fillRoundRect(20, 52, 440, 228, 16, C_PANEL_GREEN);
  tft.drawRoundRect(20, 52, 440, 228, 16, C_GREEN);
  tft.drawRoundRect(22, 54, 436, 224, 16, 0x00A0);

  // Silhouette / tick graphic
  int sx = cx, sy = 70, sr = 42;
  tft.fillRoundRect(sx - sr, sy, sr * 2, (int)(sr * 1.4), 10, C_GREEN);
  for (int i = 0; i <= sr; i++)
    tft.drawFastHLine(sx - (sr - i + 1), sy + (int)(sr * 1.4) + i, (sr - i + 1) * 2, C_GREEN);
  for (int t = -2; t <= 2; t++) {
    tft.drawLine(sx - 26 + t, sy + 30, sx -  6 + t, sy + 52, C_PANEL_GREEN);
    tft.drawLine(sx -  6 + t, sy + 52, sx + 28 + t, sy + 22, C_PANEL_GREEN);
  }

  tft.setTextDatum(MC_DATUM);

  // "Attendance Recorded" — font 2, inside panel
  clearText(20, 172, 440, 2, C_PANEL_GREEN);
  tft.setTextColor(C_GREEN, C_PANEL_GREEN);
  tft.drawString("Attendance Recorded", cx, 172, 2);
  tft.drawFastHLine(60, 184, 360, C_GREEN);

  // Name — font 4, inside panel
  clearText(20, 205, 440, 4, C_PANEL_GREEN);
  tft.setTextColor(C_WHITE, C_PANEL_GREEN);
  tft.drawString(recognizedName.c_str(), cx, 205, 4);

  // "Verified" — font 2, inside panel
  clearText(20, 240, 440, 2, C_PANEL_GREEN);
  tft.setTextColor(C_GREEN, C_PANEL_GREEN);
  tft.drawString("Verified via Face Recognition", cx, 240, 2);

  if (ttCount > 0) {
    clearText(20, 262, 440, 2, C_PANEL_GREEN);
    tft.setTextColor(C_BLUE, C_PANEL_GREEN);
    tft.drawString("Loading timetable...", cx, 262, 2);
  }

  // Section/day — font 1, below panel → bg=C_BG
  clearText(20, 290, 440, 1, C_BG);
  tft.setTextColor(C_GRAY, C_BG);
  tft.drawString("Section " + ttSection + "  |  " + ttDay, cx, 290, 1);

  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Timetable
// ═══════════════════════════════════════════════════════════════════
void screenTimetable() {
  tft.fillScreen(C_BG);

  // Header bar (TL_DATUM — no clearText needed, drawn fresh each time)
  tft.fillRect(0, 0, 480, 46, C_SURF);
  tft.drawFastHLine(0, 46, 480, C_BLUE);
  tft.setTextColor(C_BLUE, C_SURF);
  tft.drawString("Today's Timetable", 10, 8, 2);
  String sub = ttDay + "  |  Sec " + ttSection + "  |  " + recognizedName.substring(0, 18);
  tft.setTextColor(C_GRAY, C_SURF);
  tft.drawString(sub.c_str(), 10, 30, 1);
  tft.fillCircle(466, 22, 7, C_GREEN);

  if (ttCount == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_GRAY, C_BG);
    tft.drawString("No classes today!", 240, 180, 4);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  // Column header bar
  tft.fillRect(0, 47, 480, 18, C_SURF2);
  tft.setTextColor(C_GRAY, C_SURF2);
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

    // Fill entire row background FIRST — erases all prior content
    tft.fillRect(0, y, 480, rowH - 1, bg);
    tft.fillRect(0, y, 5, rowH - 1, stripe);

    // Period pill — small label centred in pill
    tft.fillRoundRect(8, y + 6, 28, rowH - 14, 4, stripe);
    tft.setTextColor(C_BG, stripe);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(tt[i].label.c_str(), 22, y + rowH / 2, 1);
    tft.setTextDatum(TL_DATUM);

    // Time, Subject, Teacher drawn on already-filled row bg
    tft.setTextColor(C_GRAY,  bg);
    tft.drawString(tt[i].time.c_str(),                        42,  y + 6, 1);
    tft.setTextColor(C_WHITE, bg);
    tft.drawString(tt[i].subject.substring(0, 18).c_str(),   148, y + 6, 2);
    tft.setTextColor(C_GRAY,  bg);
    tft.drawString(tt[i].teacher.substring(0, 24).c_str(),   310, y + 6, 1);

    tft.drawFastHLine(0, y + rowH - 1, 480, C_SURF);
    y += rowH;
  }

  tft.drawFastHLine(0, y + 2, 480, C_SURF);
  tft.setTextColor(C_BLUE, C_BG);
  tft.drawString(String(min(ttCount, 3)) + "/" + String(ttCount) + " periods shown", 8, y + 6, 1);

  // Countdown — initial draw; loop updates it once per second
  tft.fillRect(0, 298, 480, 22, C_BG);
  tft.setTextColor(C_GRAY, C_BG);
  tft.setTextDatum(MC_DATUM);
  unsigned long secLeft = (TIMETABLE_MS - inState()) / 1000;
  tft.drawString("Auto-reset in " + String(secLeft) + "s", 240, 308, 1);
  tft.setTextDatum(TL_DATUM);
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: Network Error
// ═══════════════════════════════════════════════════════════════════
void screenFailed() {
  tft.fillScreen(C_BG);
  drawHeader("Connection Error", C_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_RED,  C_BG);
  tft.drawString("Cannot reach server", 240, 160, 4);
  tft.setTextColor(C_GRAY, C_BG);
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
    "<tr><th>State</th><td class='g'>"  + String(stateNames[(int)state])                   + "</td></tr>"
    "<tr><th>Detected</th><td>"         + (recognizedName.length() ? recognizedName : "—") + "</td></tr>"
    "<tr><th>Confidence</th><td>"       + String((int)(recognizedConf * 100))              + "%</td></tr>"
    "<tr><th>Server</th><td>"           + SERVER_IP + ":" + SERVER_PORT                    + "</td></tr>"
    "<tr><th>WiFi IP</th><td>"          + WiFi.localIP().toString()                        + "</td></tr>"
    "<tr><th>Uptime</th><td>"           + String(millis() / 1000)                          + "s</td></tr>"
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
  screenWifi();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && dots < 60) {  // 30s timeout
    delay(500);
    updateWifiDots(dots);
    dots++;
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] FAILED");
    tft.fillRect(0, 230, 480, 90, C_BG);
    tft.setTextColor(C_RED, C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("WiFi FAILED — check credentials", 240, 260, 2);
    tft.setTextDatum(TL_DATUM);
    delay(5000);
    // Falls through to screenIdle() — screen is never left on WiFi bg
  }

  localWeb.on("/", webRoot);
  localWeb.begin();
  Serial.printf("[Web]    http://%s\n",           WiFi.localIP().toString().c_str());
  Serial.printf("[Config] Flask: http://%s:%d\n", SERVER_IP, SERVER_PORT);

  setState(S_IDLE);
  screenIdle();                                  // always clears WiFi screen
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

    // ── Not found: auto-dismiss ───────────────────────────────────
    case S_NOTFOUND:
      if (inState() >= NOTFOUND_MS) {
        stableCount    = 0;
        lastStableName = "";
        recognizedName = "";
        setState(S_IDLE);
        screenIdle();                            // clears not-found screen
      }
      break;

    // ── Confirmed: wait, then timetable or idle ───────────────────
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
          screenIdle();                          // clears confirmed screen
        }
      }
      break;

    // ── Timetable: tick countdown once/second, then idle ─────────
    case S_TIMETABLE: {
      static unsigned long lastTTSec = 0;
      unsigned long curSec = millis() / 1000;
      if (curSec != lastTTSec) {               // fires exactly once per second
        lastTTSec = curSec;
        unsigned long secLeft = (TIMETABLE_MS - inState()) / 1000;
        tft.fillRect(0, 298, 480, 22, C_BG);   // full-width clear
        tft.setTextColor(C_GRAY, C_BG);
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
        screenIdle();                            // clears timetable screen
      }
      break;
    }

    // ── Failed: auto-dismiss ──────────────────────────────────────
    case S_FAILED:
      if (inState() >= FAILED_MS) {
        stableCount    = 0;
        lastStableName = "";
        recognizedName = "";
        setState(S_IDLE);
        screenIdle();                            // clears error screen
      }
      break;

    // ── Blocking states (managed by their own functions) ─────────
    case S_MARKING:
    case S_FACE:
    case S_WIFI:
    default:
      break;
  }

  delay(30);
}
