#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <U8g2lib.h>

// Forward declarations
void drawAt(uint8_t x, uint8_t y, const char *text);
void drawScreen();
String setupPage(const String &message);
void handleRoot();
void handleSave();
void handleReset();
void handleLoad();
void handleSender();
void startSetupMode();
void connectToSavedNetwork();

constexpr uint8_t OLED_SDA = 5;
constexpr uint8_t OLED_SCL = 6;
constexpr uint8_t STATUS_LED = 8;
constexpr uint8_t BOOT_BUTTON = 9;  // Built-in BOOT button, active low.
constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr byte DNS_PORT = 53;

const char *SETUP_AP_NAME = "PC-CPU-Setup";
const char *SETUP_AP_PASSWORD = "cpu-display-42";
const char *DEVICE_NAME = "pc-cpu-display";

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

// The 72x40 glass is a window at (30,12) within this SSD1306's 128x64 RAM.
// Rendering the complete controller buffer prevents old pixels outside the window from lingering.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(
  U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA
);

bool setupMode = false;
bool wasStale = false;
bool showNetworkInfo = false;
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
unsigned long lastButtonChangeMs = 0;

int cpuLoad = -1;
unsigned long lastUpdateMs = 0;
String networkName;
String localAddress;

// The actual OLED glass is the 72x40 region starting at (30,12).
constexpr uint8_t VIEW_X = 30;
constexpr uint8_t VIEW_Y = 12;
constexpr uint8_t VIEW_W = 72;

void drawAt(uint8_t x, uint8_t y, const char *text) {
  display.drawStr(VIEW_X + x, VIEW_Y + y, text);
}

void drawScreen() {
  display.clearBuffer();

  if (setupMode) {
    display.setFont(u8g2_font_4x6_tf);
    drawAt(0, 7, "WI-FI SETUP");
    drawAt(0, 16, "PC-CPU-Setup");
    drawAt(0, 25, "PASS: cpu-display-42");
    drawAt(0, 34, "192.168.4.1");

  } else if (showNetworkInfo) {
    display.setFont(u8g2_font_4x6_tf);
    drawAt(0, 7, "WI-FI / IP");

    String shortName = networkName.substring(0, 17);
    drawAt(0, 16, shortName.c_str());
    drawAt(0, 25, localAddress.c_str());
    drawAt(0, 35, "BOOT = CPU");

  } else if (cpuLoad < 0) {
    display.setFont(u8g2_font_5x8_tf);
    drawAt(0, 15, "WAITING");
    drawAt(0, 27, "FOR PC");

    display.setFont(u8g2_font_4x6_tf);
    drawAt(0, 37, "BOOT = WI-FI");

  } else {
    // Main screen: CPU percentage and bottom load bar only.
    char loadText[8];
    snprintf(loadText, sizeof(loadText), "%d%%", cpuLoad);

    display.setFont(u8g2_font_logisoso20_tf);
    int x = (VIEW_W - display.getStrWidth(loadText)) / 2;

    // Positioned above the bottom bar.
    display.drawStr(VIEW_X + x, VIEW_Y + 32, loadText);

    // Thick, margin-safe CPU bar along the bottom.
    constexpr uint8_t BAR_X = 2;
    constexpr uint8_t BAR_H = 6;
    constexpr uint8_t BAR_Y = 40 - BAR_H;
    constexpr uint8_t BAR_W = 68;

    display.drawFrame(
      VIEW_X + BAR_X,
      VIEW_Y + BAR_Y,
      BAR_W,
      BAR_H
    );

    uint8_t filledWidth = ((BAR_W - 2) * cpuLoad) / 100;

    if (filledWidth > 0) {
      display.drawBox(
        VIEW_X + BAR_X + 1,
        VIEW_Y + BAR_Y + 1,
        filledWidth,
        BAR_H - 2
      );
    }
  }

  display.sendBuffer();
}

String setupPage(const String &message = "") {
  return
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PC CPU Display setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:28rem;margin:2rem auto;padding:0 1rem}"
    "input{box-sizing:border-box;width:100%;padding:.7rem;margin:.35rem 0 1rem}"
    "button{padding:.7rem 1rem}"
    "</style></head><body>"
    "<h2>Connect the CPU display</h2>"
    "<p>Enter the same Wi-Fi your Windows PC uses. "
    "The password is saved only in this display.</p>" + message +
    "<form action='/save' method='post'>"
    "<label>Wi-Fi name</label>"
    "<input name='ssid' required maxlength='32' autofocus>"
    "<label>Wi-Fi password</label>"
    "<input name='password' type='password' maxlength='63'>"
    "<button type='submit'>Connect display</button>"
    "</form></body></html>";
}

void handleRoot() {
  if (setupMode) {
    server.send(200, "text/html", setupPage());
    return;
  }

  String page =
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PC CPU Display</title></head><body>"
    "<h2>PC CPU Display</h2>"
    "<p>Device address: <b>" + localAddress + "</b></p>"
    "<p>Current CPU load: <b>" + String(cpuLoad < 0 ? 0 : cpuLoad) + "%</b></p>"
    "<p>On your Windows PC, open <code>http://" + localAddress +
    "/sender</code> to install the automatic sender.</p>"
    "<p><a href='/reset'>Change Wi-Fi network</a></p>"
    "</body></html>";

  server.send(200, "text/html", page);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");

  if (ssid.length() == 0) {
    server.send(
      400,
      "text/html",
      setupPage("<p>Please enter a Wi-Fi name.</p>")
    );
    return;
  }

  preferences.begin("cpu-display", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();

  server.send(
    200,
    "text/html",
    "<h2>Saved</h2>"
    "<p>The display is joining your Wi-Fi now. "
    "Reconnect your phone to your normal Wi-Fi, then read the display address.</p>"
  );

  delay(1200);
  ESP.restart();
}

void handleReset() {
  preferences.begin("cpu-display", false);
  preferences.clear();
  preferences.end();

  server.send(
    200,
    "text/html",
    "<h2>Wi-Fi cleared</h2>"
    "<p>The setup network will return in a moment.</p>"
  );

  delay(800);
  ESP.restart();
}

void handleLoad() {
  if (!server.hasArg("value")) {
    server.send(
      400,
      "application/json",
      "{\"error\":\"Use /load?value=0..100\"}"
    );
    return;
  }

  String rawValue = server.arg("value");
  int requestedLoad = rawValue.toInt();

  if (rawValue.length() == 0 || requestedLoad < 0 || requestedLoad > 100) {
    server.send(
      400,
      "application/json",
      "{\"error\":\"value must be 0 through 100\"}"
    );
    return;
  }

  bool changed = requestedLoad != cpuLoad || wasStale;

  cpuLoad = requestedLoad;
  lastUpdateMs = millis();
  wasStale = false;

  if (changed) {
    drawScreen();
  }

  digitalWrite(STATUS_LED, LOW);
  delay(10);
  digitalWrite(STATUS_LED, HIGH);

  server.send(
    200,
    "application/json",
    "{\"ok\":true,\"cpuLoad\":" + String(cpuLoad) + "}"
  );
}

void handleSender() {
  String script =
    "$url = 'http://" + localAddress + "/load?value='\n"
    "$scriptPath = Join-Path $env:LOCALAPPDATA 'PC-Cpu-Display-Sender.ps1'\n"
    "$body = @'\n"
    "$counter = New-Object Diagnostics.PerformanceCounter('Processor Information','% Processor Time','_Total')\n"
    "while ($true) {\n"
    "  $value = [Math]::Round($counter.NextValue())\n"
    "  try { Invoke-WebRequest -Uri ('http://" + localAddress +
    "/load?value=' + $value) -UseBasicParsing -TimeoutSec 3 | Out-Null } catch {}\n"
    "  Start-Sleep -Seconds 3\n"
    "}\n"
    "'@\n"
    "Set-Content -Path $scriptPath -Value $body -Encoding UTF8\n"
    "$action = New-ScheduledTaskAction -Execute 'powershell.exe' "
    "-Argument ('-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"\"' + "
    "$scriptPath + '\"\"')\n"
    "$trigger = New-ScheduledTaskTrigger -AtLogOn\n"
    "Register-ScheduledTask -TaskName 'PC CPU Display Sender' "
    "-Action $action -Trigger $trigger "
    "-Description 'Sends PC CPU load to the ESP32 display' -Force | Out-Null\n"
    "Start-Process powershell.exe -ArgumentList "
    "('-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"\"' + "
    "$scriptPath + '\"\"')\n"
    "Write-Host 'Installed. It will start automatically when you sign in.'\n";

  server.sendHeader(
    "Content-Disposition",
    "attachment; filename=Install-PC-Cpu-Display.ps1"
  );

  server.send(200, "text/plain", script);
}

void startSetupMode() {
  setupMode = true;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_NAME, SETUP_AP_PASSWORD);

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  server.begin();

  drawScreen();
}

void connectToSavedNetwork() {
  preferences.begin("cpu-display", true);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  preferences.end();

  if (ssid.length() == 0) {
    startSetupMode();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long started = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - started < 20000
  ) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    startSetupMode();
    return;
  }

  networkName = ssid;
  localAddress = WiFi.localIP().toString();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/load", HTTP_GET, handleLoad);
  server.on("/load", HTTP_POST, handleLoad);
  server.on("/sender", HTTP_GET, handleSender);
  server.on("/reset", HTTP_GET, handleReset);
  server.begin();

  drawScreen();
}

void setup() {
  pinMode(STATUS_LED, OUTPUT);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  digitalWrite(STATUS_LED, HIGH);

  Wire.begin(OLED_SDA, OLED_SCL);

  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  connectToSavedNetwork();
}

void loop() {
  if (setupMode) {
    dnsServer.processNextRequest();
  }

  server.handleClient();

  // Debounced BOOT button: one press changes screen once.
  bool buttonReading = digitalRead(BOOT_BUTTON);

  if (buttonReading != lastButtonReading) {
    lastButtonChangeMs = millis();
  }

  if (
    millis() - lastButtonChangeMs > 35 &&
    buttonReading != stableButtonState
  ) {
    stableButtonState = buttonReading;

    if (stableButtonState == LOW && !setupMode) {
      showNetworkInfo = !showNetworkInfo;
      drawScreen();
    }
  }

  lastButtonReading = buttonReading;

  // Refresh once when sender updates have stopped for 15 seconds.
  bool staleNow = cpuLoad >= 0 && millis() - lastUpdateMs > 15000;

  if (staleNow != wasStale) {
    wasStale = staleNow;
    drawScreen();
  }
}
