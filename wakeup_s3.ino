#include "USB.h"
#include "USBHID.h"
#include "tusb.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoMqttClient.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>

// --- Firmware Version ---
const String FIRMWARE_VERSION = "1.0";

// --- Hardware Pins ---
#define LED_PIN         21
#define BOOT_BUTTON_PIN 0 

Adafruit_NeoPixel strip(1, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences preferences;

// --- Network & MQTT Settings ---
String ssid = "";
String password = "";
String mqttServer = "";
int mqttPort = 1883;
String mqttUser = "";
String mqttPass = "";

String webUser = "admin";
String webPass = "wakeup123";
String sessionToken = "";

WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
WebServer server(80);

String deviceId = "";

// === 1. REPORT IDs ===
enum {
  REPORT_ID_KEYBOARD = 1,
  REPORT_ID_MOUSE    = 2,
  REPORT_ID_CONSUMER = 3,
  REPORT_ID_SYSTEM   = 4
};

// === 2. COMPOSITE DESCRIPTOR ===
static const uint8_t desc_hid_report[] = {
  TUD_HID_REPORT_DESC_KEYBOARD( HID_REPORT_ID(REPORT_ID_KEYBOARD) ),
  TUD_HID_REPORT_DESC_MOUSE( HID_REPORT_ID(REPORT_ID_MOUSE) ),
  TUD_HID_REPORT_DESC_CONSUMER( HID_REPORT_ID(REPORT_ID_CONSUMER) ),
  TUD_HID_REPORT_DESC_SYSTEM_CONTROL( HID_REPORT_ID(REPORT_ID_SYSTEM) )
};

USBHID HID;

typedef struct {
  uint8_t modifiers;
  uint8_t reserved;
  uint8_t keys[6];
} __attribute__((packed)) custom_kb_report_t;

typedef struct {
  uint8_t buttons;
  int8_t  x;
  int8_t  y;
  int8_t  wheel;
} __attribute__((packed)) custom_ms_report_t;

// === 3. USBHID WRAPPER CLASS ===
class TotalKeyboardDevice : public USBHIDDevice {
public:
  TotalKeyboardDevice() {}
  void begin() { HID.addDevice(this, sizeof(desc_hid_report)); }
  uint16_t _onGetDescriptor(uint8_t* buffer) override {
    memcpy(buffer, desc_hid_report, sizeof(desc_hid_report));
    return sizeof(desc_hid_report);
  }
  void _onOutput(uint8_t report_id, const uint8_t* buffer, uint16_t len) override {}
};

TotalKeyboardDevice TotalKB;

unsigned long lastActionTime = 0;
unsigned long buttonPressTime = 0;
unsigned long lastNetworkCheck = 0;
unsigned long lastWifiAttempt = 0;
bool buttonHeld = false;

void setStatusColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 20) {
  strip.setBrightness(brightness);
  strip.setPixelColor(0, strip.Color(r, g, b));
  strip.show();
}

bool isAuthenticated() {
  if (server.hasHeader("Cookie")) {
    return server.header("Cookie").indexOf("AUTH=" + sessionToken) != -1;
  }
  return false;
}

// === 4. INPUT FUNCTIONS WITH OVERFLOW PROTECTION ===
void sendMouseClick(uint8_t button = 0x01) {
  if (!tud_hid_ready()) return;
  custom_ms_report_t report = { button, 0, 0, 0 };
  HID.SendReport(REPORT_ID_MOUSE, &report, sizeof(report));
  delay(30);
  memset(&report, 0, sizeof(report));
  HID.SendReport(REPORT_ID_MOUSE, &report, sizeof(report));
}

void sendAcpiCommand(uint8_t acpi_cmd) {
  if (!tud_hid_ready()) return; 
  HID.SendReport(REPORT_ID_SYSTEM, &acpi_cmd, sizeof(acpi_cmd));
  delay(40);
  uint8_t release_cmd = 0;
  HID.SendReport(REPORT_ID_SYSTEM, &release_cmd, sizeof(release_cmd));
}

void sendConsumerCommand(uint16_t media_cmd) {
  if (!tud_hid_ready()) return;
  HID.SendReport(REPORT_ID_CONSUMER, &media_cmd, sizeof(media_cmd));
  delay(30);
  uint16_t empty = 0;
  HID.SendReport(REPORT_ID_CONSUMER, &empty, sizeof(empty));
}

void sendKeycode(uint8_t keycode, uint8_t modifiers = 0) {
  if (!tud_hid_ready()) return; 
  custom_kb_report_t report = { modifiers, 0, { keycode, 0, 0, 0, 0, 0 } };
  HID.SendReport(REPORT_ID_KEYBOARD, &report, sizeof(report));
  delay(30);
  memset(&report, 0, sizeof(report));
  HID.SendReport(REPORT_ID_KEYBOARD, &report, sizeof(report));
  delay(15);
}

void safeKeyboardPrint(const String& text) {
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    unsigned long start = millis();
    while (!tud_hid_ready() && (millis() - start < 50)) { yield(); }
    if (!tud_hid_ready()) continue;

    uint8_t keycode = 0;
    uint8_t mod = 0;

    if (c >= 'a' && c <= 'z') keycode = 0x04 + (c - 'a');
    else if (c >= 'A' && c <= 'Z') { keycode = 0x04 + (c - 'A'); mod = 0x02; }
    else if (c >= '1' && c <= '9') keycode = 0x1E + (c - '1');
    else if (c == '0') keycode = 0x27;
    else if (c == ' ') keycode = 0x2C;
    else if (c == '\n' || c == '\r') keycode = 0x28;
    else if (c == '.') keycode = 0x37;
    else if (c == ',') keycode = 0x36;
    else if (c == '-') keycode = 0x2D;
    else if (c == '_') { keycode = 0x2D; mod = 0x02; }
    else if (c == '!') { keycode = 0x1E; mod = 0x02; }

    if (keycode != 0) sendKeycode(keycode, mod);
  }
}

void publishStatus(const char* state) {
  if (mqttClient.connected()) {
    mqttClient.beginMessage("pc/status", false, 1, false);
    mqttClient.print(state);
    mqttClient.endMessage();
  }
}

void executeCommand(String cmd) {
  if (millis() - lastActionTime < 150) return;
  lastActionTime = millis();

  if (cmd == "wake") {
    if (tud_suspended()) {
      tud_remote_wakeup();
      unsigned long wait_start = millis();
      while (tud_suspended() && (millis() - wait_start < 3000)) {
        delay(50);
      }
      if (!tud_hid_ready()) {
        publishStatus("wake_failed");
        return; 
      }
    }
    sendMouseClick(0x01);
    delay(150);
    sendAcpiCommand(0x03);
    delay(150);
    sendKeycode(0x2C);
    delay(80);
    sendKeycode(0x28);
    publishStatus("woken");
  } 
  else if (cmd == "sleep") {
    sendAcpiCommand(0x02);
    publishStatus("sleeping");
  } 
  else if (cmd == "power") {
    sendAcpiCommand(0x01);
    publishStatus("power_down");
  } 
  else if (cmd == "enter") {
    sendKeycode(0x28); 
    publishStatus("entered");
  }
  else if (cmd == "vol_up") {
    sendConsumerCommand(HID_USAGE_CONSUMER_VOLUME_INCREMENT);
    publishStatus("vol_up");
  }
  else if (cmd == "vol_down") {
    sendConsumerCommand(HID_USAGE_CONSUMER_VOLUME_DECREMENT);
    publishStatus("vol_down");
  }
  else if (cmd == "mute") {
    sendConsumerCommand(HID_USAGE_CONSUMER_MUTE);
    publishStatus("muted");
  }
  else if (cmd == "play_pause") {
    sendConsumerCommand(HID_USAGE_CONSUMER_PLAY_PAUSE);
    publishStatus("play_paused");
  }
  else if (cmd == "next") {
    sendConsumerCommand(HID_USAGE_CONSUMER_SCAN_NEXT_TRACK);
    publishStatus("next_track");
  }
  else if (cmd == "prev") {
    sendConsumerCommand(HID_USAGE_CONSUMER_SCAN_PREVIOUS_TRACK);
    publishStatus("prev_track");
  }
}

// === 5. HOME ASSISTANT MQTT AUTO-DISCOVERY ===
void publishButtonDiscovery(const String& subTopic, const String& name, const String& cmd, const String& icon, const String& devInfo) {
  String topic = "homeassistant/button/" + deviceId + "/" + subTopic + "/config";
  String payload = "{\"name\":\"" + name + "\",\"cmd_t\":\"pc/command\",\"payload_press\":\"" + cmd + "\",\"ic\":\"" + icon + "\",\"uniq_id\":\"" + deviceId + "_" + subTopic + "\"" + devInfo + "}";
  mqttClient.beginMessage(topic, true, 1, false);
  mqttClient.print(payload);
  mqttClient.endMessage();
}

void publishHADiscovery() {
  if (!mqttClient.connected()) return;

  String devInfo = ",\"dev\":{\"ids\":[\"" + deviceId + "\"],\"name\":\"ESP32 PC Controller\",\"mf\":\"Logitech / ESP32\",\"mdl\":\"S3-HID-Media\",\"sw\":\"" + FIRMWARE_VERSION + "\"}";

  publishButtonDiscovery("wake", "Wake PC", "wake", "mdi:power-cycle", devInfo);
  publishButtonDiscovery("sleep", "Sleep PC", "sleep", "mdi:sleep", devInfo);
  publishButtonDiscovery("power", "Power Off PC", "power", "mdi:power", devInfo);
  publishButtonDiscovery("enter", "Send Enter", "enter", "mdi:keyboard-return", devInfo);

  publishButtonDiscovery("vol_up", "Volume Up", "vol_up", "mdi:volume-high", devInfo);
  publishButtonDiscovery("vol_down", "Volume Down", "vol_down", "mdi:volume-medium", devInfo);
  publishButtonDiscovery("mute", "Mute Audio", "mute", "mdi:volume-mute", devInfo);
  publishButtonDiscovery("play_pause", "Play / Pause", "play_pause", "mdi:play-pause", devInfo);
  publishButtonDiscovery("next", "Next Track", "next", "mdi:skip-next", devInfo);
  publishButtonDiscovery("prev", "Previous Track", "prev", "mdi:skip-previous", devInfo);

  String topicStatus = "homeassistant/sensor/" + deviceId + "/status/config";
  String payloadStatus = "{\"name\":\"PC Status\",\"stat_t\":\"pc/status\",\"ic\":\"mdi:information-outline\",\"uniq_id\":\"" + deviceId + "_status\"" + devInfo + "}";
  mqttClient.beginMessage(topicStatus, true, 1, false);
  mqttClient.print(payloadStatus);
  mqttClient.endMessage();

  String topicText = "homeassistant/text/" + deviceId + "/type/config";
  String payloadText = "{\"name\":\"Type Text\",\"cmd_t\":\"pc/type\",\"mode\":\"text\",\"ic\":\"mdi:keyboard-outline\",\"uniq_id\":\"" + deviceId + "_text\"" + devInfo + "}";
  mqttClient.beginMessage(topicText, true, 1, false);
  mqttClient.print(payloadText);
  mqttClient.endMessage();
}

const char* loginPage = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sign In</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;background:#121212;color:#eee;display:flex;justify-content:center;align-items:center;min-height:90vh;margin:0;padding:10px;}
.card{background:#1e1e1e;padding:24px;border-radius:12px;width:100%;max-width:320px;border:1px solid #333;box-shadow:0 4px 12px rgba(0,0,0,0.5);text-align:center;}
input{padding:11px;margin:6px 0;width:100%;font-size:14px;background:#282828;color:#fff;border:1px solid #333;border-radius:6px;box-sizing:border-box;}
button{padding:12px;font-size:14px;background:#0066cc;color:#fff;border:none;border-radius:6px;cursor:pointer;font-weight:600;width:100%;margin-top:10px;}
</style></head><body>
<div class="card">
  <h2 style="margin-top:0;">Sign In</h2>
  <form action="/login" method="POST">
    <input type="text" name="usr" placeholder="Username" required><br>
    <input type="password" name="pwd" placeholder="Password" required><br>
    <button type="submit">Log In</button>
  </form>
</div>
</body></html>)rawliteral";

const char* mainPage = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-S3 Controller</title>
<style>
:root{--bg:#121212;--card:#1e1e1e;--primary:#0066cc;--danger:#cc0000;--text:#eee;--border:#333;}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text);margin:0;padding:20px 10px;display:flex;flex-direction:column;align-items:center;}
.status-bar{display:flex;flex-wrap:wrap;justify-content:center;gap:8px;margin-bottom:18px;max-width:900px;}
.badge{background:#181818;border:1px solid var(--border);border-radius:20px;padding:4px 12px;font-size:12px;color:#aaa;display:inline-flex;align-items:center;gap:6px;}
.badge b{color:#fff;}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block;}
.dot-green{background:#00e676;box-shadow:0 0 6px #00e676;}
.dot-red{background:#ff5252;}
.nav{display:flex;gap:8px;margin-bottom:20px;background:#181818;padding:4px;border-radius:8px;border:1px solid var(--border);}
.nav-btn{background:transparent;border:none;color:#aaa;padding:8px 22px;border-radius:6px;cursor:pointer;font-size:14px;font-weight:600;transition:0.2s;}
.nav-btn.active{background:var(--primary);color:#fff;}
.card{background:var(--card);padding:20px;border-radius:12px;box-sizing:border-box;border:1px solid var(--border);box-shadow:0 4px 12px rgba(0,0,0,0.5);margin:0 auto 15px;}
.card h3{margin-top:0;margin-bottom:12px;font-size:15px;text-align:center;color:#fff;}
.btn-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px;}
.btn-grid-3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-bottom:12px;}
button{padding:11px;font-size:13px;background:var(--primary);color:#fff;border:none;border-radius:6px;cursor:pointer;font-weight:500;transition:0.15s;}
button:hover{opacity:0.9;}
button:active{transform:scale(0.98);}
.btn-red{background:var(--danger);width:100%;max-width:320px;margin:10px auto 0;display:block;}
input{padding:11px;margin:6px 0;width:100%;font-size:14px;background:#282828;color:#fff;border:1px solid var(--border);border-radius:6px;box-sizing:border-box;}
input:focus{border-color:var(--primary);outline:none;}
.tab-content{width:100%;display:flex;flex-direction:column;align-items:center;}
.hidden{display:none !important;}
hr{border:0;border-top:1px solid var(--border);margin:12px 0;}
#otastatus{font-size:13px;font-weight:600;margin-bottom:8px;text-align:center;}
.settings-grid{display:flex;flex-wrap:wrap;justify-content:center;gap:16px;max-width:1150px;width:100%;margin-bottom:15px;}
.settings-grid .card{flex:1 1 260px;max-width:280px;margin:0;}
.reboot-container{width:100%;max-width:1150px;display:flex;justify-content:center;}
</style></head><body>

<!-- TOP STATUS BAR WITH ACCENT BADGES -->
<div class="status-bar">
  <div class="badge">Version: <b>v%VER%</b></div>
  <div class="badge">IP: <b>%IP%</b></div>
  <div class="badge">Wi-Fi: <b>%SSID%</b></div>
  <div class="badge">MQTT: %MQTT_STATUS%</div>
</div>

<div class="nav">
  <button class="nav-btn active" onclick="switchTab('tab-control', this)">🎮 Control</button>
  <button class="nav-btn" onclick="switchTab('tab-settings', this)">⚙️ Settings</button>
</div>

<!-- TAB 1: CONTROL (CENTERED) -->
<div id="tab-control" class="tab-content">
  <div class="card" style="max-width:360px; width:100%;">
    <h3>PC Power & Input</h3>
    <div class="btn-grid">
      <button onclick="fetch('/wake')">Wake Up</button>
      <button onclick="fetch('/enter')">Enter</button>
      <button onclick="fetch('/sleep')">Sleep</button>
      <button onclick="fetch('/power')">Power Off</button>
    </div>
    <div style="display:flex; gap:6px;">
      <input type="text" id="str" placeholder="Text / Password (ENG)">
      <button onclick="sendText()" style="width:auto; padding:0 16px;">Type</button>
    </div>
  </div>

  <div class="card" style="max-width:360px; width:100%;">
    <h3>Media Control</h3>
    <div class="btn-grid-3">
      <button onclick="fetch('/vol_down')">Vol -</button>
      <button onclick="fetch('/mute')">Mute</button>
      <button onclick="fetch('/vol_up')">Vol +</button>
      <button onclick="fetch('/prev')">⏮ Prev</button>
      <button onclick="fetch('/play_pause')">⏯ Play</button>
      <button onclick="fetch('/next')">⏭ Next</button>
    </div>
  </div>
</div>

<!-- TAB 2: SETTINGS (CENTERED GRID) -->
<div id="tab-settings" class="tab-content hidden">
  <div class="settings-grid">
    <!-- 1. Network Settings -->
    <div class="card">
      <h3>Wi-Fi Network</h3>
      <form action="/save_wifi" method="POST" autocomplete="off" onsubmit="return checkPasswords(this.pass.value, this.pass2.value)">
        <input type="text" name="ssid" value="%SSID%" placeholder="Wi-Fi SSID" autocomplete="off"><br>
        <input type="password" name="pass" value="%WIFI_PASS%" placeholder="New Password" autocomplete="new-password"><br>
        <input type="password" name="pass2" placeholder="Repeat Password" autocomplete="new-password"><br>
        <button type="submit" style="width:100%; margin-top:8px;">Save Network</button>
      </form>
    </div>

    <!-- 2. MQTT Settings -->
    <div class="card">
      <h3>MQTT Broker</h3>
      <form action="/save_mqtt" method="POST" autocomplete="off">
        <input type="text" name="mqtt" value="%MQTT%" placeholder="Host / IP" autocomplete="off"><br>
        <input type="text" name="mqtt_port" value="%MQTT_PORT%" placeholder="Port (Default 1883)" autocomplete="off"><br>
        <input type="text" name="mqtt_user" value="%MQTT_USER%" placeholder="User (Optional)" autocomplete="off"><br>
        <input type="password" name="mqtt_pass" value="%MQTT_PASS%" placeholder="Password (Optional)" autocomplete="new-password"><br>
        <button type="submit" style="width:100%; margin-top:8px;">Save MQTT</button>
      </form>
    </div>

    <!-- 3. Web Auth -->
    <div class="card">
      <h3>Web Security</h3>
      <form action="/save_auth" method="POST" autocomplete="off" onsubmit="return checkPasswords(this.web_pass.value, this.web_pass2.value)">
        <input type="text" name="web_user" value="%USER%" placeholder="Username" autocomplete="off"><br>
        <input type="password" name="web_pass" value="%WEB_PASS%" placeholder="New Password" autocomplete="new-password"><br>
        <input type="password" name="web_pass2" placeholder="Repeat Password" autocomplete="new-password"><br>
        <button type="submit" style="width:100%; margin-top:8px;">Change Auth</button>
      </form>
    </div>

    <!-- 4. System & OTA -->
    <div class="card">
      <h3>System & OTA</h3>
      <div id="otastatus" style="color:#ffa500;"></div>
      <form id="otaForm" onsubmit="uploadOTA(event)">
        <input type="file" id="otafile" name="update" accept=".bin"><br>
        <button type="submit" style="width:100%; margin-top:8px;">Flash .bin</button>
      </form>
    </div>
  </div>

  <div class="reboot-container">
    <button class="btn-red" onclick="rebootESP()">Device Reboot</button>
  </div>
</div>

<script>
function switchTab(tabId, btn){
  document.querySelectorAll('.tab-content').forEach(el => el.classList.add('hidden'));
  document.querySelectorAll('.nav-btn').forEach(el => el.classList.remove('active'));
  document.getElementById(tabId).classList.remove('hidden');
  btn.classList.add('active');
}

function sendText(){
  let el = document.getElementById('str');
  if(!el.value) return;
  fetch('/type?text=' + encodeURIComponent(el.value));
  el.value = '';
}

function checkPasswords(p1, p2) {
  if (p1 === '********' && p2 === '') return true; 
  if (p1 !== p2) {
    alert('Error: Passwords do not match!');
    return false;
  }
  return true;
}

function uploadOTA(e){
  e.preventDefault();
  let fileInput = document.getElementById('otafile');
  if(!fileInput.files.length) return alert('Select a .bin file!');
  
  let st = document.getElementById('otastatus');
  st.style.color = '#ffa500';
  st.innerHTML = 'Uploading firmware...';
  
  let fd = new FormData();
  fd.append('update', fileInput.files[0]);
  
  fetch('/update', {method: 'POST', body: fd})
  .then(res => res.text())
  .then(text => {
    if(text.includes('SUCCESS')){
      st.style.color = '#00ff00';
      st.innerHTML = 'Success! Rebooting...';
      setTimeout(() => location.reload(), 10000);
    } else {
      st.style.color = '#ff0000';
      st.innerHTML = 'Error: ' + text;
    }
  }).catch(err => {
    st.style.color = '#ff0000';
    st.innerHTML = 'Connection error!';
  });
}

function rebootESP(){
  if(confirm('Reboot ESP32?')){
    fetch('/reboot');
    setTimeout(() => location.reload(), 5000);
  }
}
</script></body></html>)rawliteral";

void onMqttMessage(int messageSize) {
  String topic = mqttClient.messageTopic();
  String payload = "";
  while (mqttClient.available()) {
    payload += (char)mqttClient.read();
  }

  if (topic == "pc/command") {
    executeCommand(payload);
  } else if (topic == "pc/type") {
    safeKeyboardPrint(payload);
    publishStatus("typed");
  }
}

void setup() {
  Serial.begin(115200);
  sessionToken = String(esp_random(), HEX);
  Serial.println("\nBooting... Firmware Version: " + FIRMWARE_VERSION);

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char idBuf[16];
  snprintf(idBuf, sizeof(idBuf), "esp32_%02X%02X%02X", mac[3], mac[4], mac[5]);
  deviceId = String(idBuf);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  strip.begin();
  setStatusColor(255, 140, 0); 

  preferences.begin("cfg", false);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("pass", "");
  mqttServer = preferences.getString("mqtt", "");
  mqttPort = preferences.getInt("mqtt_port", 1883);
  mqttUser = preferences.getString("mqtt_u", "");
  mqttPass = preferences.getString("mqtt_p", "");
  
  String savedUser = preferences.getString("wUser", "");
  String savedPass = preferences.getString("wPass", "");
  if (savedUser.length() > 0) webUser = savedUser;
  if (savedPass.length() > 0) webPass = savedPass;
  preferences.end();

  // === TOTAL KEYBOARD + MEDIA INIT ===
  USB.VID(0x046D); 
  USB.PID(0xC322); 
  USB.productName("Logitech Total Keyboard V5.4");
  USB.manufacturerName("Logitech");

  USB.usbAttributes(0xA0); 

  TotalKB.begin();
  HID.begin();
  USB.begin();
  // ====================================

  if (ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(300);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("ESP32-Setup-AP", "12345678");
    setStatusColor(255, 0, 0); 
  } else {
    setStatusColor(0, 0, 255); 
    Serial.print("Connected to Wi-Fi. IP: ");
    Serial.println(WiFi.localIP());
  }

  server.on("/", []() {
    if (!isAuthenticated()) { 
      server.send(200, "text/html", loginPage); 
      return; 
    }
    
    String page = mainPage;
    String currentIP = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "192.168.4.1 (AP Mode)";
    String currentSSID = (ssid.length() > 0) ? ssid : "Not Set";
    String mqttStatusBadge = mqttClient.connected() 
      ? "<span class='dot dot-green'></span> <b>Connected</b>" 
      : "<span class='dot dot-red'></span> <b>Disconnected</b>";
    
    page.replace("%VER%", FIRMWARE_VERSION);
    page.replace("%IP%", currentIP);
    page.replace("%SSID%", currentSSID);
    page.replace("%MQTT_STATUS%", mqttStatusBadge);
    
    page.replace("%MQTT%", mqttServer);
    page.replace("%MQTT_PORT%", String(mqttPort));
    page.replace("%MQTT_USER%", mqttUser);
    page.replace("%MQTT_PASS%", mqttPass.length() > 0 ? "********" : "");
    page.replace("%USER%", webUser);
    page.replace("%WIFI_PASS%", password.length() > 0 ? "********" : "");
    page.replace("%WEB_PASS%", webPass.length() > 0 ? "********" : "");
    
    server.send(200, "text/html", page);
  });

  server.on("/login", HTTP_POST, []() {
    if (server.arg("usr") == webUser && server.arg("pwd") == webPass) {
      server.sendHeader("Set-Cookie", "AUTH=" + sessionToken + "; Path=/; HttpOnly");
      server.sendHeader("Location", "/");
      server.send(303);
    } else {
      server.send(401, "text/plain; charset=utf-8", "Invalid username or password");
    }
  });

  // Separate Wi-Fi Save Endpoint
  server.on("/save_wifi", HTTP_POST, []() {
    if (!isAuthenticated()) { server.send(401); return; }
    
    String p = server.arg("pass");
    String p2 = server.arg("pass2");
    
    if (p != "********" && p.length() > 0 && p != p2) {
      server.send(400, "text/plain; charset=utf-8", "Error: Wi-Fi passwords do not match");
      return;
    }

    preferences.begin("cfg", false);
    preferences.putString("ssid", server.arg("ssid"));
    if (p != "********" && p.length() > 0) {
      preferences.putString("pass", p); 
    }
    preferences.end();
    
    String html = "<meta charset=\"utf-8\"><h3>Wi-Fi Saved. Rebooting...</h3><script>setTimeout(()=>location.href='/', 6000);</script>";
    server.send(200, "text/html", html);
    delay(1000);
    ESP.restart();
  });

  // Separate MQTT Save Endpoint
  server.on("/save_mqtt", HTTP_POST, []() {
    if (!isAuthenticated()) { server.send(401); return; }

    preferences.begin("cfg", false);
    preferences.putString("mqtt", server.arg("mqtt"));
    int portVal = server.arg("mqtt_port").toInt();
    preferences.putInt("mqtt_port", portVal > 0 ? portVal : 1883);
    
    preferences.putString("mqtt_u", server.arg("mqtt_user"));
    
    String mp = server.arg("mqtt_pass");
    if (mp != "********") {
      preferences.putString("mqtt_p", mp);
    }
    preferences.end();
    
    String html = "<meta charset=\"utf-8\"><h3>MQTT Saved. Rebooting...</h3><script>setTimeout(()=>location.href='/', 6000);</script>";
    server.send(200, "text/html", html);
    delay(1000);
    ESP.restart();
  });

  // Separate Auth Save Endpoint
  server.on("/save_auth", HTTP_POST, []() {
    if (!isAuthenticated()) { server.send(401); return; }
    
    String wp = server.arg("web_pass");
    String wp2 = server.arg("web_pass2");
    
    if (wp != "********" && wp.length() > 0 && wp != wp2) {
      server.send(400, "text/plain; charset=utf-8", "Error: Passwords do not match");
      return;
    }

    preferences.begin("cfg", false);
    if (server.arg("web_user").length() > 0) preferences.putString("wUser", server.arg("web_user"));
    if (wp != "********" && wp.length() > 0) {
      preferences.putString("wPass", wp);
    }
    preferences.end();
    
    String html = "<meta charset=\"utf-8\"><h3>Auth Updated. Rebooting...</h3><script>setTimeout(()=>location.href='/', 6000);</script>";
    server.send(200, "text/html", html);
    delay(1000);
    ESP.restart();
  });

  server.on("/reboot", []() {
    if (!isAuthenticated()) { server.send(401); return; }
    server.send(200, "text/plain", "OK");
    delay(500);
    ESP.restart();
  });

  // Action Endpoints
  server.on("/wake",  []() { if(!isAuthenticated()){server.send(401);return;} executeCommand("wake"); server.send(200,"text/plain","OK"); });
  server.on("/sleep", []() { if(!isAuthenticated()){server.send(401);return;} executeCommand("sleep"); server.send(200,"text/plain","OK"); });
  server.on("/power", []() { if(!isAuthenticated()){server.send(401);return;} executeCommand("power"); server.send(200,"text/plain","OK"); });
  server.on("/enter", []() { if(!isAuthenticated()){server.send(401);return;} executeCommand("enter"); server.send(200,"text/plain","OK"); });

  server.on("/vol_up",     []() { if(!isAuthenticated()){server.send(401);return;} executeCommand("vol_up"); server.send(200,"text/plain","OK"); });
  server.on("/vol_down",   []() { if(!isAuthenticated()){server.send(401);return;} executeCommand("vol_down"); server.send(200,"text/plain","OK"); });
  server.on("/mute",       []() { if(!isAuthenticated()){server.send(401);return;} executeCommand("mute"); server.send(200,"text/plain","OK"); });
  server.on("/play_pause", []() { if(!isAuthenticated()){server.send(401);return;} executeCommand("play_pause"); server.send(200,"text/plain","OK"); });
  server.on("/next",       []() { if(!isAuthenticated()){server.send(401);return;} executeCommand("next"); server.send(200,"text/plain","OK"); });
  server.on("/prev",       []() { if(!isAuthenticated()){server.send(401);return;} executeCommand("prev"); server.send(200,"text/plain","OK"); });

  server.on("/type",  []() {
    if(!isAuthenticated()){server.send(401);return;}
    if(server.hasArg("text")) {
      safeKeyboardPrint(server.arg("text"));
      publishStatus("typed");
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/update", HTTP_POST, []() {
    if (!isAuthenticated()) { server.send(401, "text/plain", "Unauthorized"); return; }
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "UPDATE ERROR" : "SUCCESS");
    delay(500);
    ESP.restart();
  }, []() {
    if (!isAuthenticated()) {
      Update.abort();
      return;
    }
    
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update Start: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %u bytes\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  const char* headerkeys[] = {"Cookie"};
  server.collectHeaders(headerkeys, 1);
  server.begin();

  if (mqttServer.length() > 0) {
    mqttClient.onMessage(onMqttMessage);
  }
}

void loop() {
  server.handleClient();

  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    if (!buttonHeld) {
      buttonHeld = true;
      buttonPressTime = millis();
    } else if (millis() - buttonPressTime > 5000) {
      setStatusColor(255, 0, 0, 50);
      preferences.begin("cfg", false);
      preferences.clear();
      preferences.end();
      Serial.println("Factory Reset! Rebooting...");
      delay(1000);
      ESP.restart();
    }
  } else {
    buttonHeld = false;
  }

  if (millis() - lastNetworkCheck > 4000) {
    lastNetworkCheck = millis();

    if (ssid.length() > 0) {
      if (WiFi.status() != WL_CONNECTED) {
        setStatusColor(255, 140, 0); 
        if (millis() - lastWifiAttempt > 15000) {
          lastWifiAttempt = millis();
          Serial.println("Attempting Wi-Fi reconnection...");
          WiFi.begin(ssid.c_str(), password.c_str());
        }
      } else {
        if (mqttServer.length() > 0 && !mqttClient.connected()) {
          setStatusColor(0, 0, 255); 
          Serial.println("Connecting to MQTT...");
          
          if (mqttUser.length() > 0) {
            mqttClient.setUsernamePassword(mqttUser, mqttPass);
          }
          
          if (mqttClient.connect(mqttServer.c_str(), mqttPort)) {
            mqttClient.subscribe("pc/command");
            mqttClient.subscribe("pc/type");
            
            publishHADiscovery();
            
            publishStatus("online");
            setStatusColor(0, 255, 0); 
            Serial.println("MQTT Connected & HA Entities Discovered!");
          }
        }
      }
    }
  }

  if (mqttClient.connected()) {
    mqttClient.poll();
  }
}