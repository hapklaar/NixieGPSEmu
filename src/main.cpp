#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <TFT_eSPI.h>
#include <sys/time.h>

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);  // Off-screen buffer

#define GPS_TX_PIN 26
#define RESET_BUTTON_PIN 0

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

unsigned long lastDisplayUpdate = 0;
const unsigned long displayInterval = 1000;

unsigned long buttonPressStart = 0;

void updateTimeStatus() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm *tm_struct = gmtime(&tv.tv_sec);
  timeSet = (tm_struct->tm_year + 1900) >= 2020;
}

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

void setupWebRoutes() {
  server.on("/", []() {
    String html = "<html><head><title>NixieGPS-Emulator</title>"
                  "<style>"
                  "body { font-family: Arial, sans-serif; background-color: #222; color: #eee; font-size: 24px; }"
                  "form { margin: auto; width: 640px; padding: 40px; background: #333; border-radius: 20px; }"
                  "input, select { width: 100%; margin: 16px 0; padding: 16px; border-radius: 8px; border: none; font-size: 24px; }"
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
      html += "<option value='" + ssidScan + "'>" + ssidScan + " (" + String(rssi) + "dBm)</option>";
    }
    html += "</select><br>";
    html += "Password: <input type='password' name='password'><br>";
    html += "Hostname: <input type='text' name='hostname' value='" + hostname + "'><br>";
    html += "NTP Server: <input type='text' name='ntpserver' value='" + ntpServer + "'><br>";
    html += "Baudrate: <input type='number' name='baudrate' value='" + String(baudrate) + "'><br>";
    html += "<input type='submit' value='Save'>";
    html += "</form></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/save", []() {
    if (server.hasArg("ssid")) ssid = server.arg("ssid");
    if (server.hasArg("password")) password = server.arg("password");
    if (server.hasArg("hostname")) hostname = server.arg("hostname");
    if (server.hasArg("ntpserver")) ntpServer = server.arg("ntpserver");
    if (server.hasArg("baudrate")) baudrate = server.arg("baudrate").toInt();

    saveConfig();

    String response = "<html><body><p>Settings saved. Rebooting...</p></body></html>";
    server.send(200, "text/html", response);

    delay(1000);
    ESP.restart();
  });
}

void startAPMode() {
  configMode = true;
  WiFi.softAP("NixieGPS");
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("Started AP mode: IP %s\n", ip.toString().c_str());
  setupWebRoutes();
  server.begin();
}

void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  Serial.printf("Connecting to WiFi %s ...\n", ssid.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, ntpServer.c_str());
    setupWebRoutes();
    server.begin();
  } else {
    Serial.println("Failed to connect to WiFi.");
    startAPMode();
  }
}

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

  String gpsLine = "$" + sentence + "*" + checksum;

  Serial.println("GPS output: " + gpsLine);
  Serial.flush();
  Serial2.print(gpsLine);
  Serial2.print("\r\n");
}

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
  } else {  
  // Labels in white
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
  
  updateTimeStatus();
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm *tm_struct = gmtime(&tv.tv_sec);

  if (timeSet) {
    sprite.setTextColor(TFT_GREEN);
    sprite.print("OK");
  } else {
    sprite.setTextColor(TFT_WHITE);
    sprite.print("Syncing...");
  }

  sprite.setCursor(5, 90);
  if (timeSet) {
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

void setup() {
  Serial.begin(115200);
  preferences.begin("config", false);
  loadConfig();

  Serial2.begin(baudrate, SERIAL_8N1, -1, GPS_TX_PIN);

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
//  delay(50);
//  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
//    Serial.println("Reset button pressed: clearing config...");
//    preferences.clear();
//    delay(500);
//    ESP.restart();
//  }

  tft.init();
  tft.setRotation(1);
  sprite.createSprite(tft.width(), tft.height());
  sprite.setRotation(1);

  if (ssid == "") {
    configMode = true;
    startAPMode();
  } else {
    configMode = false;
    startWiFi();
    }
  if (!configMode) {
    if (!MDNS.begin(hostname.c_str())) {
      Serial.println("Error starting mDNS");
    } else {
      Serial.printf("mDNS started: http://%s.local\n", hostname.c_str());
      
    }
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

  if (!configMode) {
    if (WiFi.status() == WL_CONNECTED) {
      unsigned long now = millis();
      if (now - lastDisplayUpdate > displayInterval) {
        lastDisplayUpdate = now;
        drawDisplay();
        if (timeSet) {
          outputGPS();
        }
        updateTimeStatus();
      }
    } else {
      Serial.println("WiFi lost, starting AP mode");
      startAPMode();
      configMode = true;
    }
  } else {
    unsigned long now = millis();
    if (now - lastDisplayUpdate > displayInterval) {
      lastDisplayUpdate = now;
      drawDisplay();
    }
  }
}
