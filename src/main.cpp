#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <TFT_eSPI.h>
#include <sys/time.h>  // for gettimeofday()

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);  // Create sprite for double buffering

#define GPS_TX_PIN 26       // Serial GPS output pin (changed per your request)
#define RESET_BUTTON_PIN 0  // Boot button on TTGO T-Display (GPIO0)

Preferences preferences;

String ssid = "";
String password = "";
String hostname = "NixieGPSEmu";
String ntpServer = "pool.ntp.org";
int baudrate = 9600;

bool configMode = false;

WebServer server(80);

unsigned long lastDisplayUpdate = 0;
const unsigned long displayInterval = 1000;

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

void startAPMode() {
  configMode = true;
  WiFi.softAP("ESP32GPSConfig");
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("Started AP mode: IP %s\n", ip.toString().c_str());

  server.on("/", []() {
    String html = "<html><head><title>Config Portal</title>"
                  "<style>body{font-family:Arial,sans-serif;text-align:center;margin-top:30px;}"
                  "form{display:inline-block;text-align:left;}</style></head><body>";
    html += "<h2>Configure WiFi and Settings</h2>";
    html += "<form method='POST' action='/save'>";
    html += "SSID: <select name='ssid'>";
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; ++i) {
      String ssidScan = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      html += "<option value='" + ssidScan + "'>" + ssidScan + " (" + String(rssi) + "dBm)</option>";
    }
    html += "</select><br><br>";
    html += "Password: <input type='password' name='password'><br><br>";
    html += "Hostname: <input type='text' name='hostname' value='" + hostname + "'><br><br>";
    html += "NTP Server: <input type='text' name='ntpserver' value='" + ntpServer + "'><br><br>";
    html += "Baudrate: <input type='number' name='baudrate' value='" + String(baudrate) + "'><br><br>";
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

    String response = "<html><body><h3>Settings saved. Rebooting...</h3></body></html>";
    server.send(200, "text/html", response);

    delay(1000);
    ESP.restart();
  });

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
  if (gettimeofday(&tv, NULL) != 0) {
    return; // no valid time yet
  }

  time_t now = tv.tv_sec;
  struct tm *tm_struct = gmtime(&now);

  // Format GPRMC time and date: hhmmss.sss, ddmmyy
  char buffer[80];
  sprintf(buffer, "GPRMC,%02d%02d%02d.00,A,0000.00,N,00000.00,E,0.0,0.0,%02d%02d%02d,,",
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
  spr.fillSprite(TFT_BLACK);

  // Labels in white
  spr.setTextColor(TFT_WHITE);
  spr.setTextSize(2);
  spr.setCursor(5, 0);
  spr.print("Host: ");
  spr.setTextColor(TFT_CYAN);
  spr.print(hostname.c_str());

  spr.setCursor(5, 30);
  spr.setTextColor(TFT_WHITE);
  spr.print("IP: ");
  spr.setTextColor(TFT_CYAN);
  spr.print(WiFi.localIP().toString());

  spr.setCursor(5, 60);
  spr.setTextColor(TFT_WHITE);
  spr.print("NTP: ");
  bool timeSet = false;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm *tm_struct = gmtime(&tv.tv_sec);
  timeSet = (tm_struct->tm_year + 1900) >= 2020;

  if (timeSet) {
    spr.setTextColor(TFT_GREEN);
    spr.print("OK");
  } else {
    spr.setTextColor(TFT_WHITE);
    spr.print("Syncing...");
  }

  // Time larger and lower
  spr.setTextSize(3);
  spr.setCursor(5, 90);
  if (timeSet) {
    char timebuf[20];
    sprintf(timebuf, "%02d:%02d:%02d UTC", tm_struct->tm_hour, tm_struct->tm_min, tm_struct->tm_sec);
    spr.setTextColor(TFT_CYAN);
    spr.print(timebuf);
  } else {
    spr.setTextColor(TFT_CYAN);
    spr.print("Waiting ...");
  }

  spr.pushSprite(0, 0);
}


void setup() {
  Serial.begin(115200);
  preferences.begin("config", false);
  loadConfig();

  Serial2.begin(baudrate, SERIAL_8N1, -1, GPS_TX_PIN);

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  delay(50);
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    Serial.println("Reset button pressed: clearing config...");
    preferences.clear();
    delay(500);
    ESP.restart();
  }

  tft.init();
  tft.setRotation(1);

  spr.createSprite(tft.width(), tft.height());  // Create sprite buffer matching display

  tft.fillScreen(TFT_BLACK);

  if (ssid == "") {
    startAPMode();
  } else {
    startWiFi();
  }

  server.begin();  // Always start server (AP or STA mode)

  if (!configMode) {
    if (!MDNS.begin(hostname.c_str())) {
      Serial.println("Error starting mDNS");
    } else {
      Serial.printf("mDNS started: http://%s.local\n", hostname.c_str());
    }
  }
}

void loop() {
  server.handleClient();

  if (!configMode) {
    if (WiFi.status() == WL_CONNECTED) {
      unsigned long now = millis();
      if (now - lastDisplayUpdate > displayInterval) {
        lastDisplayUpdate = now;
        drawDisplay();
        outputGPS();
      }
    } else {
      Serial.println("WiFi lost, starting AP mode");
      startAPMode();
    }
  }
}
