/**
 * Nixie GPS Time Emulator v2.0
 *
 * Description:
 * This code runs on an ESP32 with a TFT display. Its primary function is to
 * connect to a WiFi network, get accurate time from an NTP server, and then
 * emulate a GPS module by outputting NMEA sentences ($GPRMC) over a serial
 * port. This is ideal for devices like Nixie clocks that require a GPS signal
 * for time synchronization but are used indoors where GPS reception is poor.
 *
 * v2.0 Changes:
 * - Robust WiFi Reconnection: Instead of reverting to AP mode on WiFi failure,
 * the device now periodically retries to connect to the saved network in the
 * background without blocking other functions.
 * - Improved Display States: The TFT display now shows a "Connecting..." status
 * when attempting to connect to WiFi, providing better user feedback.
 * - Enhanced Configuration UX: The web configuration portal now pre-selects the
 * currently saved WiFi network and provides a more informative confirmation
 * page after saving settings.
 * - Forced AP Mode on Boot: You can now force the device into AP (configuration)
 * mode by pressing and holding the reset button during startup.
 * - Code Refactoring: The connection logic has been moved into the main loop for
 * a non-blocking design, and redundant code has been cleaned up.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <TFT_eSPI.h>
#include <sys/time.h>

// Hardware Definitions
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft); // Off-screen buffer for flicker-free drawing

#define GPS_TX_PIN 26
#define RESET_BUTTON_PIN 0

// Configuration & State Variables
Preferences preferences;

String ssid = "";
String password = "";
String hostname = "NixieGPSEmu";
String ntpServer = "pool.ntp.org";
int baudrate = 9600;

bool configMode = false;
bool timeSet = false;
bool buttonPressed = false;

WebServer server(80);

// Timing variables for non-blocking operations
unsigned long lastDisplayUpdate = 0;
const unsigned long displayInterval = 1000;

unsigned long buttonPressStart = 0;

// New globals for WiFi retry logic
unsigned long lastWifiRetry = 0;
const unsigned long wifiRetryInterval = 30000; // Retry every 30 seconds
bool servicesStarted = false; // Flag to track if NTP and mDNS are running

// =================================================================
// TIME & STATUS FUNCTIONS
// =================================================================

void updateTimeStatus() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm *tm_struct = gmtime(&tv.tv_sec);
  // Consider time "set" if the year is plausible (post-2020)
  timeSet = (tm_struct->tm_year + 1900) >= 2020;
}

// =================================================================
// CONFIGURATION MANAGEMENT (SAVE/LOAD FROM FLASH)
// =================================================================

void saveConfig() {
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.putString("hostname", hostname);
  preferences.putString("ntpserver", ntpServer);
  preferences.putInt("baudrate", baudrate);
}

void loadConfig() {
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  hostname = preferences.getString("hostname", "NixieGPSEmu");
  ntpServer = preferences.getString("ntpserver", "pool.ntp.org");
  baudrate = preferences.getInt("baudrate", 9600);
}

// =================================================================
// WEB SERVER & CONFIGURATION PORTAL
// =================================================================

void setupWebRoutes() {
  server.on("/", []() {
    String html = "<html><head><title>NixieGPS-Emulator</title>"
                  "<style>"
                  "body { font-family: Arial, sans-serif; background-color: #222; color: #eee; font-size: 24px; }"
                  "form { margin: auto; width: 640px; padding: 40px; background: #333; border-radius: 20px; }"
                  "input, select { width: 100%; margin: 16px 0; padding: 16px; border-radius: 8px; border: none; font-size: 24px; box-sizing: border-box; }"
                  "input[type=submit] { background-color: #4CAF50; color: white; font-weight: bold; cursor: pointer; padding: 16px; }"
                  "h2 { text-align: center; font-size: 28px; }"
                  "</style>"
                  "</head><body><form method='POST' action='/save'>"
                  "<h2>NixieGPS-Emulator Configure WiFi and Settings</h2>";
    html += "SSID: <select name='ssid'>";
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; ++i) {
      String ssidScan = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      html += "<option value='" + ssidScan + "'";
      // Improvement: Pre-select the currently saved SSID
      if (ssidScan == ssid) {
        html += " selected";
      }
      html += ">" + ssidScan + " (" + String(rssi) + "dBm)</option>";
    }
    html += "</select><br>";
    html += "Password: <input type='password' name='password' placeholder='Enter new password'><br>";
    html += "Hostname: <input type='text' name='hostname' value='" + hostname + "'><br>";
    html += "NTP Server: <input type='text' name='ntpserver' value='" + ntpServer + "'><br>";
    html += "Baudrate: <input type='number' name='baudrate' value='" + String(baudrate) + "'><br>";
    html += "<input type='submit' value='Save'>";
    html += "</form></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/save", []() {
    String connectingToSSID = server.hasArg("ssid") ? server.arg("ssid") : ssid;
    if (server.hasArg("ssid")) ssid = server.arg("ssid");
    // Only update password if a new one is provided
    if (server.hasArg("password") && server.arg("password").length() > 0) {
        password = server.arg("password");
    }
    if (server.hasArg("hostname")) hostname = server.arg("hostname");
    if (server.hasArg("ntpserver")) ntpServer = server.arg("ntpserver");
    if (server.hasArg("baudrate")) baudrate = server.arg("baudrate").toInt();

    saveConfig();

    // Improvement: More informative save page
    String response = "<html><head><style>body{font-family: Arial, sans-serif; background-color: #222; color: #eee; font-size: 24px; text-align: center; padding-top: 50px;}</style></head>"
                      "<body><h2>Settings Saved!</h2>"
                      "<p>Rebooting and attempting to connect to:</p>"
                      "<p style='color: #4CAF50; font-weight: bold;'>" + connectingToSSID + "</p>"
                      "</body></html>";
    server.send(200, "text/html", response);

    delay(3000); // Give browser time to render the page
    ESP.restart();
  });
}

// =================================================================
// WIFI MANAGEMENT
// =================================================================

void startAPMode() {
  configMode = true;
  WiFi.softAP("NixieGPS");
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("Started AP mode: IP %s\n", ip.toString().c_str());
  setupWebRoutes();
  server.begin();
}


// =================================================================
// GPS EMULATION
// =================================================================

String calculateChecksum(const String &sentence) {
  uint8_t checksum = 0;
  for (size_t i = 1; i < sentence.length(); i++) {
    if (sentence[i] == '*') break;
    checksum ^= sentence[i];
  }
  char cs[3];
  sprintf(cs, "%02X", checksum);
  return String(cs);
}

void outputGPS() {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) != 0) return;

  time_t now = tv.tv_sec;
  struct tm *tm_struct = gmtime(&now);

  char buffer[80];
  sprintf(buffer, "GPRMC,%02d%02d%02d.000,A,0000.0000,N,00000.0000,E,0.0,0.0,%02d%02d%02d,,",
          tm_struct->tm_hour, tm_struct->tm_min, tm_struct->tm_sec,
          tm_struct->tm_mday, tm_struct->tm_mon + 1, (tm_struct->tm_year + 1900) % 100);

  String sentence = String(buffer);
  String checksum = calculateChecksum("$" + sentence);
  String gpsLine = "$" + sentence + "*" + checksum + "\r\n";

  Serial.print("GPS output: " + gpsLine);
  Serial2.print(gpsLine);
}

// =================================================================
// DISPLAY MANAGEMENT
// =================================================================

void drawDisplay() {
  sprite.fillSprite(TFT_BLACK);

  if (configMode) {
    // AP Mode display
    sprite.setTextSize(3);
    sprite.setTextColor(TFT_ORANGE);
    sprite.setCursor(40, 20);
    sprite.println("AP Mode");

    sprite.setTextSize(2);
    sprite.setCursor(5, 70);
    sprite.setTextColor(TFT_WHITE);
    sprite.print("IP: ");
    sprite.setTextColor(TFT_CYAN);
    sprite.println(WiFi.softAPIP().toString());
  } else if (WiFi.status() != WL_CONNECTED) {
    // Improvement: New state for Connecting / Reconnecting
    sprite.setTextSize(3);
    sprite.setTextColor(TFT_ORANGE);
    sprite.setCursor(10, 20);
    sprite.println("Connecting...");

    sprite.setTextSize(2);
    sprite.setCursor(5, 70);
    sprite.setTextColor(TFT_WHITE);
    sprite.print("SSID: ");
    sprite.setTextColor(TFT_CYAN);
    sprite.println(ssid);
  } else {
    // Normal connected display
    sprite.setTextColor(TFT_WHITE);
    sprite.setTextSize(2);
    sprite.setCursor(5, 0);
    sprite.print("Host: ");
    sprite.setTextColor(TFT_CYAN);
    sprite.print(hostname.c_str());

    sprite.setCursor(5, 30);
    sprite.setTextColor(TFT_WHITE);
    sprite.print("IP: ");
    sprite.setTextColor(TFT_CYAN);
    sprite.print(WiFi.localIP().toString());

    sprite.setCursor(5, 60);
    sprite.setTextColor(TFT_WHITE);
    sprite.print("NTP: ");
    
    if (timeSet) {
      sprite.setTextColor(TFT_GREEN);
      sprite.print("OK");
    } else {
      sprite.setTextColor(TFT_WHITE);
      sprite.print("Syncing...");
    }

    sprite.setCursor(5, 90);
    if (timeSet) {
      struct timeval tv;
      gettimeofday(&tv, NULL);
      struct tm *tm_struct = gmtime(&tv.tv_sec);
      sprite.setTextSize(3);
      char timebuf[20];
      sprintf(timebuf, "%02d:%02d:%02d UTC", tm_struct->tm_hour, tm_struct->tm_min, tm_struct->tm_sec);
      sprite.setTextColor(TFT_CYAN);
      sprite.print(timebuf);
    } else {
      sprite.setTextSize(2);
      sprite.setTextColor(TFT_CYAN);
      sprite.print("Waiting for NTP...");
    }
  }
  sprite.pushSprite(0, 0);
}


// =================================================================
// MAIN SETUP & LOOP
// =================================================================

void setup() {
  Serial.begin(115200);
  preferences.begin("config", false);
  loadConfig();

  Serial2.begin(baudrate, SERIAL_8N1, -1, GPS_TX_PIN);

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  delay(50); // Small delay to stabilize pin reading

  tft.init();
  tft.setRotation(1);
  sprite.createSprite(tft.width(), tft.height());
  sprite.setRotation(1);

  // Improvement: Force AP mode if no SSID saved OR reset button is held on boot
  if (ssid == "" || digitalRead(RESET_BUTTON_PIN) == LOW) {
    Serial.println("No WiFi config found or reset button held on boot. Starting AP mode.");
    startAPMode();
  } else {
    configMode = false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    Serial.printf("Attempting to connect to saved WiFi: %s\n", ssid.c_str());
    setupWebRoutes();
    server.begin();
  }
}

void checkResetButton() {
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (!buttonPressed) {
      buttonPressed = true;
      buttonPressStart = millis();
    } else if (millis() - buttonPressStart >= 2000) {
      Serial.println("Long press detected: clearing config...");
      preferences.clear();
      delay(500);
      ESP.restart();
    }
  } else {
    buttonPressed = false;
  }
}

void loop() {
  server.handleClient();
  checkResetButton();

  if (configMode) {
    // In AP mode, just update the display periodically
    unsigned long now = millis();
    if (now - lastDisplayUpdate > displayInterval) {
      lastDisplayUpdate = now;
      drawDisplay();
    }
  } else { // Normal (Station) Mode
    if (WiFi.status() != WL_CONNECTED) {
      // We are disconnected
      if (servicesStarted) {
        Serial.println("WiFi connection lost.");
        MDNS.end();
        servicesStarted = false; // Reset flag to re-init services on reconnect
        timeSet = false; // Time is no longer reliable
      }
      // Improvement: Non-blocking periodic retry
      unsigned long now = millis();
      if (now - lastWifiRetry > wifiRetryInterval) {
        lastWifiRetry = now;
        Serial.println("Retrying WiFi connection...");
        WiFi.begin(ssid.c_str(), password.c_str());
      }
    } else {
      // We are connected
      if (!servicesStarted) {
        // This block runs once upon successful connection
        Serial.printf("WiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        configTime(0, 0, ntpServer.c_str());
        
        if (MDNS.begin(hostname.c_str())) {
          Serial.printf("mDNS responder started: http://%s.local\n", hostname.c_str());
        } else {
          Serial.println("Error starting mDNS");
        }
        servicesStarted = true;
      }
    }

    // This section runs continuously in normal mode
    unsigned long now = millis();
    if (now - lastDisplayUpdate > displayInterval) {
      lastDisplayUpdate = now;
      drawDisplay(); // The display function is now connection-aware
      if (WiFi.status() == WL_CONNECTED) {
        updateTimeStatus();
        if (timeSet) {
          outputGPS();
        }
      }
    }
  }
}
