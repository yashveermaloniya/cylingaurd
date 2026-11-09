
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Firebase_ESP_Client.h>
#include "HX711.h"
#include <ESP32Servo.h>

/********** PINS **********/
#define PIN_MQ2_AIN     34
#define PIN_BUZZER      27
#define PIN_HX_DT       4
#define PIN_HX_SCK      5
#define PIN_SERVO       14     // used if valveType == "servo"
#define PIN_RELAY       26     // used if valveType == "relay"

/********** CONFIG PORTAL **********/
#define AP_SSID         "Cylin-Guard-Setup"
#define AP_PASSWORD     "12345678"
#define CONFIG_TIMEOUT  300000  // 5 min in config mode, then retry

/********** GLOBALS **********/
Preferences prefs;
WebServer server(80);

FirebaseData    fbdo;
FirebaseAuth    auth;
FirebaseConfig  config;

HX711 scale;
Servo  valveServo;

// Configuration variables (loaded from Preferences)
String wifiSSID     = "";
String wifiPassword = "";
String firebaseAPIKey = "";
String firebaseDBURL  = "";
String deviceID       = "esp32-gas-01";

// Valve control mode: "servo" or "relay"
String valveType = "servo";

/********** SETTINGS (runtime, can be overridden via RTDB /settings) **********/
float SET_LEAK_PPM            = 1200.0;
float SET_LOW_GAS_KG          = 5.0;
bool  SET_AUTO_CLOSE_ENABLED  = true;
bool  SET_REQUIRE_MANUAL_OPEN = true;
float SET_FULL_CYL_KG         = 14.2;
int   SET_SERVO_OPEN_ANGLE    = 90;
int   SET_SERVO_CLOSE_ANGLE   = 0;
bool  SET_RELAY_OPEN_LEVEL    = HIGH;
float SET_SCALE_FACTOR        = 2280.0f;

/********** STATE **********/
bool  safetyArmed   = true;
bool  leakDetected  = false;
bool  valveClosed   = true;
bool  buzzerMuted   = false;
uint32_t lastLeakMs = 0;

bool configMode = false;
bool configured = false;

/********** TIMING **********/
const uint32_t TELEMETRY_MS     = 1000;
const uint32_t SETTINGS_PULL_MS = 5000;
const uint32_t COMMANDS_PULL_MS = 1500;

/********** LEAK FILTER **********/
const int LEAK_CONSEC_N = 3;
int leakConsec = 0;

/********** AVERAGING **********/
const int AVG_N_GAS = 5;
const int AVG_N_KG  = 5;

/********** FORWARD DECLS **********/
void valveOpen();
void valveClose();
void applyValveOutputs();
void loadSettings();
void processCommands();
void pushTelemetry(float gasRaw, float weightKg, bool leak);
void updateStateInRTDB();
void loadConfig();
void saveConfig();
void startConfigPortal();
void handleRoot();
void handleSave();
void handleReset();

/********** HELPERS **********/
String basePath() { return String("/devices/") + deviceID; }

/********** CONFIG MANAGEMENT **********/
void loadConfig() {
  prefs.begin("cylin-guard", true); // read-only
  wifiSSID       = prefs.getString("ssid", "");
  wifiPassword   = prefs.getString("pass", "");
  firebaseAPIKey = prefs.getString("apikey", "");
  firebaseDBURL  = prefs.getString("dburl", "");
  deviceID       = prefs.getString("devid", "esp32-gas-01");
  prefs.end();

  configured = (wifiSSID.length() > 0 && firebaseAPIKey.length() > 0);
  
  Serial.println("\n=== Configuration Loaded ===");
  Serial.println("WiFi SSID: " + (wifiSSID.length() > 0 ? wifiSSID : "[NOT SET]"));
  Serial.println("Firebase configured: " + String(firebaseAPIKey.length() > 0 ? "YES" : "NO"));
  Serial.println("Device ID: " + deviceID);
  Serial.println("===========================\n");
}

void saveConfig() {
  prefs.begin("cylin-guard", false); // read-write
  prefs.putString("ssid", wifiSSID);
  prefs.putString("pass", wifiPassword);
  prefs.putString("apikey", firebaseAPIKey);
  prefs.putString("dburl", firebaseDBURL);
  prefs.putString("devid", deviceID);
  prefs.end();
  Serial.println("Configuration saved to flash!");
}

void clearConfig() {
  prefs.begin("cylin-guard", false);
  prefs.clear();
  prefs.end();
  Serial.println("Configuration cleared!");
}

/********** WEB PORTAL **********/
void startConfigPortal() {
  Serial.println("\n=== Starting Configuration Portal ===");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  Serial.println("Connect to: " + String(AP_SSID));
  Serial.println("Password: " + String(AP_PASSWORD));
  Serial.println("Open browser: http://" + IP.toString());
  Serial.println("=====================================\n");

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", HTTP_POST, handleReset);
  server.begin();
  
  configMode = true;
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Cylin Guard - Configuration</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 20px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            max-width: 500px;
            width: 100%;
            overflow: hidden;
        }
        .header {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 30px;
            text-align: center;
        }
        .header h1 {
            font-size: 2em;
            margin-bottom: 5px;
            font-weight: 700;
        }
        .header p {
            opacity: 0.9;
            font-size: 0.95em;
        }
        .form-container {
            padding: 30px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            color: #333;
            font-weight: 600;
            margin-bottom: 8px;
            font-size: 0.9em;
        }
        input {
            width: 100%;
            padding: 12px 15px;
            border: 2px solid #e0e0e0;
            border-radius: 8px;
            font-size: 1em;
            transition: border 0.3s;
        }
        input:focus {
            outline: none;
            border-color: #667eea;
        }
        .btn {
            width: 100%;
            padding: 14px;
            border: none;
            border-radius: 8px;
            font-size: 1em;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s;
            margin-top: 10px;
        }
        .btn-primary {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
        }
        .btn-primary:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(102, 126, 234, 0.4);
        }
        .btn-danger {
            background: #e74c3c;
            color: white;
            margin-top: 15px;
        }
        .btn-danger:hover {
            background: #c0392b;
        }
        .info {
            background: #f8f9fa;
            padding: 15px;
            border-radius: 8px;
            margin-top: 20px;
            font-size: 0.85em;
            color: #666;
        }
        .shield-icon {
            font-size: 3em;
            margin-bottom: 10px;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <div class="shield-icon">🛡️</div>
            <h1>Cylin Guard</h1>
            <p>Smart LPG Safety System</p>
        </div>
        <div class="form-container">
            <form action="/save" method="POST">
                <div class="form-group">
                    <label for="ssid">WiFi Network Name (SSID)</label>
                    <input type="text" id="ssid" name="ssid" placeholder="Enter your WiFi name" required>
                </div>
                
                <div class="form-group">
                    <label for="password">WiFi Password</label>
                    <input type="password" id="password" name="password" placeholder="Enter WiFi password" required>
                </div>
                
                <div class="form-group">
                    <label for="apikey">Firebase API Key</label>
                    <input type="text" id="apikey" name="apikey" placeholder="AIzaSy..." required>
                </div>
                
                <div class="form-group">
                    <label for="dburl">Firebase Database URL</label>
                    <input type="text" id="dburl" name="dburl" placeholder="https://your-project.firebaseio.com" required>
                </div>
                
                <div class="form-group">
                    <label for="devid">Device ID</label>
                    <input type="text" id="devid" name="devid" placeholder="esp32-gas-01" value="esp32-gas-01" required>
                </div>
                
                <button type="submit" class="btn btn-primary">💾 Save Configuration</button>
            </form>
            
            <form action="/reset" method="POST" onsubmit="return confirm('Reset all settings? Device will restart in config mode.');">
                <button type="submit" class="btn btn-danger">🔄 Reset Configuration</button>
            </form>
            
            <div class="info">
                <strong>📝 Instructions:</strong><br>
                1. Enter your WiFi credentials<br>
                2. Add Firebase project details from console<br>
                3. Set a unique device ID<br>
                4. Click Save - device will restart and connect
            </div>
        </div>
    </div>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid")) wifiSSID = server.arg("ssid");
  if (server.hasArg("password")) wifiPassword = server.arg("password");
  if (server.hasArg("apikey")) firebaseAPIKey = server.arg("apikey");
  if (server.hasArg("dburl")) firebaseDBURL = server.arg("dburl");
  if (server.hasArg("devid")) deviceID = server.arg("devid");
  
  saveConfig();
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Saved - Cylin Guard</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 20px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            max-width: 500px;
            width: 100%;
            text-align: center;
            padding: 50px 30px;
        }
        .success-icon { font-size: 5em; margin-bottom: 20px; }
        h1 { color: #333; margin-bottom: 15px; }
        p { color: #666; line-height: 1.6; }
    </style>
</head>
<body>
    <div class="container">
        <div class="success-icon">✅</div>
        <h1>Configuration Saved!</h1>
        <p>Cylin Guard is restarting and will connect to your network.<br>
        This page will close automatically.</p>
    </div>
    <script>
        setTimeout(function(){ window.close(); }, 3000);
    </script>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
  delay(2000);
  ESP.restart();
}

void handleReset() {
  clearConfig();
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Reset - Cylin Guard</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #e74c3c 0%, #c0392b 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 20px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            max-width: 500px;
            width: 100%;
            text-align: center;
            padding: 50px 30px;
        }
        .icon { font-size: 5em; margin-bottom: 20px; }
        h1 { color: #333; margin-bottom: 15px; }
        p { color: #666; line-height: 1.6; }
    </style>
</head>
<body>
    <div class="container">
        <div class="icon">🔄</div>
        <h1>Configuration Reset</h1>
        <p>Device is restarting in configuration mode...<br>
        Reconnect to the setup WiFi network.</p>
    </div>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
  delay(2000);
  ESP.restart();
}

/********** WIFI CONNECTION **********/
void wifiConnect() {
  if (wifiSSID.length() == 0) {
    Serial.println("No WiFi credentials configured!");
    return;
  }
  
  Serial.print("Connecting to WiFi: ");
  Serial.println(wifiSSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    Serial.print(".");
    delay(500);
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi!");
    Serial.println("Restarting in config mode...");
    clearConfig();
    delay(1000);
    ESP.restart();
  }
}

void firebaseInit() {
  if (firebaseAPIKey.length() == 0 || firebaseDBURL.length() == 0) {
    Serial.println("Firebase not configured!");
    return;
  }
  
  Serial.print("Initializing Firebase...");
  config.api_key = firebaseAPIKey.c_str();
  config.database_url = firebaseDBURL.c_str();
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println(" Done!");
}

void hardwareInit() {
  pinMode(PIN_MQ2_AIN, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  valveServo.setPeriodHertz(50);
  valveServo.attach(PIN_SERVO, 500, 2400);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, !SET_RELAY_OPEN_LEVEL);
  scale.begin(PIN_HX_DT, PIN_HX_SCK);
  scale.set_scale(SET_SCALE_FACTOR);
  scale.tare();

  valveClose();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=================================");
  Serial.println("   CYLIN GUARD - SMART LPG");
  Serial.println("=================================\n");
  
  loadConfig();
  
  if (!configured) {
    Serial.println("⚠️  Device not configured - starting setup portal");
    startConfigPortal();
  } else {
    wifiConnect();
    if (WiFi.status() == WL_CONNECTED) {
      firebaseInit();
      hardwareInit();
      
      String p = basePath() + "/state/connected";
      if (Firebase.RTDB.setBool(&fbdo, p.c_str(), true)) {
        Serial.println("✓ Connected state set in Firebase");
      }
    }
  }
}

float readGasAvg() {
  static float buf[AVG_N_GAS] = {0};
  static int idx = 0;
  int raw = analogRead(PIN_MQ2_AIN);
  buf[idx] = raw;
  idx = (idx + 1) % AVG_N_GAS;
  float s = 0;
  for (int i=0;i<AVG_N_GAS;i++) s += buf[i];
  return s / AVG_N_GAS;
}

float readKgAvg() {
  static float buf[AVG_N_KG] = {0};
  static int idx = 0;
  float kg = scale.get_units(3);
  buf[idx] = kg;
  idx = (idx + 1) % AVG_N_KG;
  float s = 0;
  for (int i=0;i<AVG_N_KG;i++) s += buf[i];
  return s / AVG_N_KG;
}

uint32_t lastTele=0, lastSetPull=0, lastCmdPull=0;

void loop() {
  if (configMode) {
    server.handleClient();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) wifiConnect();

  uint32_t now = millis();

  if (now - lastSetPull > SETTINGS_PULL_MS) { loadSettings(); lastSetPull = now; }
  if (now - lastCmdPull > COMMANDS_PULL_MS) { processCommands(); lastCmdPull = now; }

  float gas = readGasAvg();
  float kg  = readKgAvg();

  bool over = gas >= SET_LEAK_PPM;
  if (over) leakConsec++;
  else if (leakConsec > 0) leakConsec--;

  bool confirmed = leakConsec >= LEAK_CONSEC_N;

  if (safetyArmed && confirmed) {
    leakDetected = true;
    lastLeakMs = now;
    if (SET_AUTO_CLOSE_ENABLED) valveClose();
  }

  digitalWrite(PIN_BUZZER, (leakDetected && !buzzerMuted) ? HIGH : LOW);

  if (now - lastTele > TELEMETRY_MS) {
    pushTelemetry(gas, kg, leakDetected);
    lastTele = now;
  }
}

/********** VALVE CONTROL **********/
void valveOpen() {
  if (valveType == "servo") {
    valveServo.write(SET_SERVO_OPEN_ANGLE);
  } else {
    digitalWrite(PIN_RELAY, SET_RELAY_OPEN_LEVEL);
  }
  valveClosed = false;
  updateStateInRTDB();
}

void valveClose() {
  if (valveType == "servo") {
    valveServo.write(SET_SERVO_CLOSE_ANGLE);
  } else {
    digitalWrite(PIN_RELAY, !SET_RELAY_OPEN_LEVEL);
  }
  valveClosed = true;
  updateStateInRTDB();
}

void applyValveOutputs() {
  if (valveClosed) valveClose();
  else valveOpen();
}

/********** FIREBASE IO **********/
void pushTelemetry(float gasRaw, float weightKg, bool leak) {
  String p = basePath() + "/telemetry";
  FirebaseJson j;
  j.set("gasPpm", gasRaw);
  j.set("weightKg", weightKg);
  j.set("leak", leak);
  j.set("ts", (double) millis());

  Serial.printf("Pushing telemetry: Gas=%f, Weight=%f, Leak=%d\n", gasRaw, weightKg, leak);
  if (!Firebase.RTDB.setJSON(&fbdo, p.c_str(), &j)) {
    Serial.println("Failed to push telemetry data.");
  }
}

void updateStateInRTDB() {
  String p = basePath() + "/state";
  FirebaseJson j;
  j.set("valveClosed", valveClosed);
  j.set("safetyArmed", safetyArmed);
  j.set("leakDetected", leakDetected);
  j.set("buzzerMuted", buzzerMuted);
  j.set("lastLeakMs", (double) lastLeakMs);
  j.set("connected", true);
  Firebase.RTDB.updateNode(&fbdo, p.c_str(), &j);
}

void loadSettings() {
  String p = basePath() + "/settings";
  if (Firebase.RTDB.getJSON(&fbdo, p.c_str())) {
    FirebaseJson& j = fbdo.jsonObject();
    FirebaseJsonData v;

    if (j.get(v, "leakPpm") && v.success) SET_LEAK_PPM = v.to<double>();
    if (j.get(v, "lowGasKg") && v.success) SET_LOW_GAS_KG = v.to<double>();
    if (j.get(v, "autoCloseEnabled") && v.success) SET_AUTO_CLOSE_ENABLED = v.to<bool>();
    if (j.get(v, "requireManualOpen") && v.success) SET_REQUIRE_MANUAL_OPEN = v.to<bool>();
    if (j.get(v, "fullCylinderKg") && v.success) SET_FULL_CYL_KG = v.to<double>();
    if (j.get(v, "valveType") && v.success) valveType = String(v.to<const char*>());
    if (j.get(v, "servoOpenAngle") && v.success) SET_SERVO_OPEN_ANGLE = v.to<int>();
    if (j.get(v, "servoCloseAngle") && v.success) SET_SERVO_CLOSE_ANGLE = v.to<int>();
    if (j.get(v, "relayOpenLevel") && v.success) SET_RELAY_OPEN_LEVEL = v.to<bool>();
    if (j.get(v, "scaleFactor") && v.success) {
      SET_SCALE_FACTOR = v.to<double>();
      scale.set_scale(SET_SCALE_FACTOR);
    }
    if (j.get(v, "tareNow") && v.success && v.to<bool>()) {
      scale.tare();
      FirebaseJson tareUpdate;
      tareUpdate.set("tareNow", false);
      Firebase.RTDB.updateNode(&fbdo, p.c_str(), &tareUpdate);
    }
  }
  applyValveOutputs();
}

void ackCommand(const String& key, const String& status) {
  String p = basePath() + "/commands/" + key + "/ack";
  Firebase.RTDB.setString(&fbdo, p.c_str(), status);
}

void processCommandObject(const String& key, FirebaseJson& cmd) {
  FirebaseJsonData v;
  if (!cmd.get(v, "type") || !v.success) return;
  String type = v.to<const char*>();

  if (type == "OPEN_VALVE") {
    if (SET_REQUIRE_MANUAL_OPEN && leakDetected) {
      ackCommand(key, "blocked_leak_latched");
      return;
    }
    valveOpen();
    ackCommand(key, "ok");
  } else if (type == "CLOSE_VALVE") {
    valveClose();
    ackCommand(key, "ok");
  } else if (type == "TARE") {
    scale.tare();
    ackCommand(key, "ok");
  } else if (type == "SILENCE_BUZZER") {
    buzzerMuted = true;
    updateStateInRTDB();
    ackCommand(key, "ok");
  } else if (type == "ARM") {
    safetyArmed = true;
    updateStateInRTDB();
    ackCommand(key, "ok");
  } else if (type == "DISARM") {
    safetyArmed = false;
    updateStateInRTDB();
    ackCommand(key, "ok");
  } else if (type == "RESET_LEAK") {
    leakDetected = false;
    leakConsec = 0;
    buzzerMuted = false;
    updateStateInRTDB();
    ackCommand(key, "ok");
  } else {
    ackCommand(key, "unknown_type");
  }
}

void processCommands() {
  String p = basePath() + "/commands";
  if (!Firebase.RTDB.getJSON(&fbdo, p.c_str())) return;

  FirebaseJson& root = fbdo.jsonObject();
  size_t n = root.iteratorBegin();
  FirebaseJson::IteratorValue it;
  for (size_t i=0;i<n;i++) {
    it = root.valueAt(i);
    String key = it.key;
    FirebaseJson cmd = it.value;
    FirebaseJsonData v;
    if (cmd.get(v, "ack") && v.success) continue;
    processCommandObject(key, cmd);
  }
  root.iteratorEnd();
}
