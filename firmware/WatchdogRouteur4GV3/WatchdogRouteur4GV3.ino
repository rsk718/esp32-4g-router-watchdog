/*
  Watchdog Routeur 4G - V2 fiabilisée + anti-marquage LCD (défilement + alternance)
  ESP32 VROOM + LCD1602A (LiquidCrystal parallèle) + AsyncWebServer

  Objectifs:
  - Fonctionner des mois sans intervention
  - Reboot routeur 4G si Internet KO confirmé (hystérésis)
  - Timeout HTTP + fallback (Google 204 + Cloudflare 204)
  - Reconnect Wi-Fi + reboot ESP si Wi-Fi HS trop longtemps
  - Limite de reboots/jour + reset au changement de jour (NTP valide)
  - Reboot routeur non bloquant (state machine) => page web reste dispo
  - Logs persistants, écriture flash batchée (anti-usure)
  - LCD anti-marquage: alternance ligne 1 + scroll ligne 2

  Relais (comme ton code juillet):
    RELAY_PIN HIGH = COUPE alim routeur (OFF)
    RELAY_PIN LOW  = Routeur alimenté (ON)
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <LiquidCrystal.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <time.h>

// ------------------- PINS -------------------
#define RELAY_PIN        16
#define LED_PIN          23
#define TEST_BUTTON_PIN  25
#define RESET_WIFI_PIN   27

// LCD1602A : RS, E, D4, D5, D6, D7 (pins confirmées)
LiquidCrystal lcd(13, 12, 14, 32, 33, 26);

// ------------------- WIFI / NTP -------------------
Preferences preferences;
AsyncWebServer server(80);

String ssid = "";
String password = "";
bool portalMode = false;

const char* AP_SSID = "ESP32_Config";
const char* AP_PASS = "12345678"; // change si tu veux

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;      // Paris hiver
const int   daylightOffset_sec = 3600; // +1h été (ok pour FR)

// ------------------- SURVEILLANCE -------------------
unsigned long lastCheck = 0;
const unsigned long checkIntervalMs = 60UL * 1000UL; // 60s

const int FAILS_TO_REBOOT = 3;  // N échecs consécutifs avant reboot routeur
const int OKS_TO_CLEAR    = 2;  // M succès consécutifs pour considérer stable
int failStreak = 0;
int okStreak   = 0;

bool internetState = false; // état "stable" (après hystérésis)
String lastFailure = "";

// Limiteur de reboot/jour
int rebootCount = 0;
int maxRebootsPerDay = 48;
int lastYDay = -1; // jour de l'année (0..365)

// Logs RAM + flush NVS par lots
String logBuffer = "";
unsigned long lastLogFlush = 0;
const unsigned long LOG_FLUSH_MS = 60UL * 1000UL; // 1 min

// ------------------- REBOOT ROUTEUR NON BLOQUANT -------------------
enum RebootState { RB_IDLE, RB_POWER_OFF, RB_WAIT_BOOT };
RebootState rbState = RB_IDLE;
unsigned long rbTs = 0;

const unsigned long ROUTER_OFF_MS  = 10UL * 1000UL; // coupure 10s
const unsigned long ROUTER_BOOT_MS = 90UL * 1000UL; // attente 90s (ajuste si besoin)

// ------------------- WIFI ROBUSTE -------------------
unsigned long lastWiFiCheck = 0;
unsigned long wifiDownSince = 0;

// ------------------- LCD ANTI-MARQUAGE (défilement + alternance) -------------------
String scrollText = "";
int scrollIdx = 0;

unsigned long lastScroll = 0;
const unsigned long SCROLL_MS = 250;   // vitesse défilement

unsigned long lastPageSwitch = 0;
bool pageAlt = false;
const unsigned long PAGE_MS = 2500;    // alternance ligne 1

// ------------------- UI WEB -------------------
const char* CSS = R"CSS(
<style>
:root{--bg:#0b1220;--card:#0f1a2e;--muted:#90a4c3;--txt:#e9f0ff;--ok:#21c55d;--ko:#ef4444;--warn:#f59e0b;--btn:#2563eb;}
*{box-sizing:border-box} body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu;background:linear-gradient(180deg,#070b14,#0b1220 55%,#070b14);color:var(--txt);}
.wrap{max-width:980px;margin:0 auto;padding:20px;}
h1{font-size:22px;margin:0 0 12px 0;font-weight:800;letter-spacing:.2px}
.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:12px}
.card{grid-column:span 12;background:rgba(15,26,46,.85);border:1px solid rgba(255,255,255,.06);border-radius:16px;box-shadow:0 10px 30px rgba(0,0,0,.35);padding:14px}
@media(min-width:760px){.card.half{grid-column:span 6}.card.third{grid-column:span 4}}
.row{display:flex;align-items:center;justify-content:space-between;gap:10px}
.k{color:var(--muted);font-size:12px}
.v{font-size:15px;font-weight:700}
.badge{display:inline-flex;align-items:center;gap:8px;padding:6px 10px;border-radius:999px;font-weight:800;font-size:12px;border:1px solid rgba(255,255,255,.08)}
.badge.ok{background:rgba(33,197,93,.12);color:#9dffc3}
.badge.ko{background:rgba(239,68,68,.12);color:#ffb4b4}
.badge.warn{background:rgba(245,158,11,.12);color:#ffd59a}
.btns{display:flex;flex-wrap:wrap;gap:10px}
button,a.btn{appearance:none;border:0;border-radius:12px;padding:12px 14px;font-weight:900;cursor:pointer;color:#fff;background:var(--btn);text-decoration:none;display:inline-flex;align-items:center;gap:10px}
button.red,a.red{background:#b91c1c}
button.orange,a.orange{background:#d97706}
button.gray,a.gray{background:#334155}
small{color:var(--muted)}
pre{white-space:pre-wrap;word-break:break-word;background:#070b14;border:1px solid rgba(255,255,255,.06);border-radius:14px;padding:12px;max-height:260px;overflow:auto}
hr{border:0;border-top:1px solid rgba(255,255,255,.07);margin:12px 0}
input{width:100%;padding:10px 12px;border-radius:12px;border:1px solid rgba(255,255,255,.10);background:#070b14;color:var(--txt)}
label{font-size:12px;color:var(--muted);font-weight:700}
</style>
)CSS";

static inline String htmlHeader(const String& title){
  return "<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>"+title+"</title>"+String(CSS)+"</head><body><div class='wrap'>";
}
static inline String htmlFooter(){ return "</div></body></html>"; }

static inline String badgeOk(const String& t){ return "<span class='badge ok'>🟢 "+t+"</span>"; }
static inline String badgeKo(const String& t){ return "<span class='badge ko'>🔴 "+t+"</span>"; }
static inline String badgeWarn(const String& t){ return "<span class='badge warn'>🟠 "+t+"</span>"; }

// ------------------- OUTILS -------------------
bool ntpValid() { return time(nullptr) >= 1700000000; }

String nowString() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "Heure NTP ?";
  struct tm ti;
  localtime_r(&now, &ti);
  char buf[32];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &ti);
  return String(buf);
}

void addLog(const String& line) {
  logBuffer += nowString() + " → " + line + "\n";
}

void flushLogIfNeeded(bool force=false) {
  if (!force && (millis() - lastLogFlush < LOG_FLUSH_MS)) return;
  lastLogFlush = millis();
  preferences.begin("log", false);
  preferences.putString("txt", logBuffer);
  preferences.end();
}

void loadLog() {
  preferences.begin("log", true);
  logBuffer = preferences.getString("txt", "");
  preferences.end();
}

void clearLog() {
  logBuffer = "";
  flushLogIfNeeded(true);
}

// ------------------- CREDENTIALS -------------------
void loadCredentials() {
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("pass", "");
  preferences.end();
  portalMode = (ssid.length() == 0);
}

void saveCredentials(const String& s, const String& p) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", s);
  preferences.putString("pass", p);
  preferences.end();
}

void clearCredentials() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();
}

// ------------------- PERSIST COUNTERS -------------------
void loadCounters() {
  preferences.begin("counters", true);
  rebootCount = preferences.getInt("rebootCount", 0);
  maxRebootsPerDay = preferences.getInt("maxPerDay", 48);
  lastYDay = preferences.getInt("lastYDay", -1);
  preferences.end();
}

void saveCounters() {
  preferences.begin("counters", false);
  preferences.putInt("rebootCount", rebootCount);
  preferences.putInt("maxPerDay", maxRebootsPerDay);
  preferences.putInt("lastYDay", lastYDay);
  preferences.end();
}

void resetQuotaIfNewDay() {
  if (!ntpValid()) return; // IMPORTANT: sans NTP fiable, pas de reset
  time_t now = time(nullptr);
  struct tm ti;
  localtime_r(&now, &ti);

  if (lastYDay == -1) {
    lastYDay = ti.tm_yday;
    saveCounters();
    return;
  }

  if (ti.tm_yday != lastYDay) {
    lastYDay = ti.tm_yday;
    rebootCount = 0;
    saveCounters();
    addLog("Nouvelle journée : compteur reboots remis à 0");
    flushLogIfNeeded(true);
  }
}

// ------------------- WIFI -------------------
bool connectToWiFi(unsigned long timeoutMs=15000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(150);
  }
  return (WiFi.status() == WL_CONNECTED);
}

void ensureWiFi() {
  if (millis() - lastWiFiCheck < 5000) return;
  lastWiFiCheck = millis();

  if (WiFi.status() == WL_CONNECTED) {
    wifiDownSince = 0;
    return;
  }

  if (wifiDownSince == 0) wifiDownSince = millis();

  WiFi.disconnect();
  WiFi.reconnect();

  // Si Wi-Fi HS > 3 minutes : reboot ESP
  if (millis() - wifiDownSince > 180000UL) {
    addLog("Wi-Fi HS trop longtemps → reboot ESP32");
    flushLogIfNeeded(true);
    ESP.restart();
  }
}

// ------------------- INTERNET TEST -------------------
bool http204(const char* url) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(2500);
  if (!http.begin(url)) return false;
  int code = http.GET();
  http.end();
  return (code == 204);
}

bool internetNow() {
  if (http204("http://clients3.google.com/generate_204")) return true;
  if (http204("http://cp.cloudflare.com/generate_204"))   return true;
  return false;
}

// ------------------- RELAIS (comme ton code) -------------------
void routerPowerOn()  { digitalWrite(RELAY_PIN, LOW);  } // ON
void routerPowerOff() { digitalWrite(RELAY_PIN, HIGH); } // OFF
bool routerIsPowered(){ return digitalRead(RELAY_PIN) == LOW; }

// ------------------- REBOOT ROUTEUR (state machine) -------------------
void startRouterReboot(const String& reason) {
  if (rbState != RB_IDLE) return;

  resetQuotaIfNewDay();

  if (rebootCount >= maxRebootsPerDay) {
    lastFailure = "Limite reboots/jour atteinte";
    addLog("Reboot refusé (limite/jour) : " + reason);
    flushLogIfNeeded(true);
    return;
  }

  rebootCount++;
  saveCounters();

  lastFailure = reason;
  addLog("REBOOT ROUTEUR : " + reason + " (count=" + String(rebootCount) + "/" + String(maxRebootsPerDay) + ")");
  flushLogIfNeeded(true);

  routerPowerOff();
  rbState = RB_POWER_OFF;
  rbTs = millis();

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Reboot 4G...");
  lcd.setCursor(0,1); lcd.print("Coupure alim");
}

void tickRouterReboot() {
  if (rbState == RB_IDLE) return;

  if (rbState == RB_POWER_OFF) {
    if (millis() - rbTs >= ROUTER_OFF_MS) {
      routerPowerOn();
      rbState = RB_WAIT_BOOT;
      rbTs = millis();
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("Reboot 4G...");
      lcd.setCursor(0,1); lcd.print("Redemarrage");
    }
    return;
  }

  if (rbState == RB_WAIT_BOOT) {
    if (millis() - rbTs >= ROUTER_BOOT_MS) {
      rbState = RB_IDLE;
      failStreak = 0;
      okStreak = 0;
      addLog("Fin reboot routeur (attente boot OK)");
      flushLogIfNeeded(true);
    }
  }
}

// ------------------- LCD SCROLL LINE 2 -------------------
void lcdScrollLine2() {
  if (millis() - lastScroll < SCROLL_MS) return;
  lastScroll = millis();

  String t = scrollText;

  if (t.length() <= 16) {
    while (t.length() < 16) t += " ";
    lcd.setCursor(0, 1);
    lcd.print(t);
    return;
  }

  String padded = t + "   " + t;
  if (scrollIdx + 16 > (int)padded.length()) scrollIdx = 0;

  lcd.setCursor(0, 1);
  lcd.print(padded.substring(scrollIdx, scrollIdx + 16));

  scrollIdx++;
  if (scrollIdx >= (int)(t.length() + 3)) scrollIdx = 0;
}

// ------------------- LCD STATUS (anti-marquage) -------------------
void lcdStatus() {
  if (millis() - lastPageSwitch > PAGE_MS) {
    lastPageSwitch = millis();
    pageAlt = !pageAlt;
    scrollIdx = 0;
  }

  lcd.setCursor(0, 0);
  if (pageAlt) {
    if (internetState) lcd.print("Internet OK     ");
    else              lcd.print("Internet KO     ");
  } else {
    if (rbState == RB_IDLE) {
      lcd.print(routerIsPowered() ? "Routeur ON      " : "Routeur OFF     ");
    } else {
      lcd.print("Reboot en cours  ");
    }
  }

  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -999;
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "0.0.0.0";

  scrollText =
    "IP:" + ip +
    "  RSSI:" + String(rssi) + "dBm" +
    "  R:" + String(rebootCount) + "/" + String(maxRebootsPerDay) +
    (rbState == RB_IDLE ? "" : "  BOOT");

  lcdScrollLine2();
}

// ------------------- CHECK INTERNET + HYSTERESIS -------------------
void checkInternetStable() {
  ensureWiFi();
  resetQuotaIfNewDay();

  if (rbState != RB_IDLE) return; // pendant reboot routeur, pas de décision

  bool nowOk = internetNow();
  digitalWrite(LED_PIN, nowOk ? HIGH : LOW);

  if (nowOk) {
    okStreak++;
    failStreak = 0;

    if (!internetState && okStreak >= OKS_TO_CLEAR) {
      internetState = true;
      addLog("Internet redevenu OK (stable)");
      flushLogIfNeeded(true);
    }
    return;
  }

  failStreak++;
  okStreak = 0;

  if (internetState && failStreak >= FAILS_TO_REBOOT) {
    internetState = false;
    addLog("Internet KO confirmé (stable)");
    flushLogIfNeeded(true);
    startRouterReboot("Internet KO (" + String(FAILS_TO_REBOOT) + " echec(s) d'affilée)");
  }
}

// ------------------- WEB -------------------
void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    bool wifi = (WiFi.status()==WL_CONNECTED);
    bool inet = internetState;
    bool powered = routerIsPowered();
    int rssi = wifi ? WiFi.RSSI() : -999;

    String s = htmlHeader("Watchdog 4G");
    s += "<h1>Watchdog Routeur 4G</h1><div class='grid'>";

    s += "<div class='card third'><div class='row'><div><div class='k'>Internet</div><div class='v'>"
         + String(inet?"OK":"HS") + "</div></div>"
         + (inet?badgeOk("Connecté"):badgeKo("KO")) + "</div><small>Test 204 (fallback)</small></div>";

    s += "<div class='card third'><div class='row'><div><div class='k'>Wi-Fi</div><div class='v'>"
         + String(wifi?"Connecté":"Déconnecté") + "</div></div>"
         + (wifi?badgeOk("RSSI "+String(rssi)+" dBm"):badgeKo("OFF")) + "</div>"
         "<small>Reconnect auto</small></div>";

    s += "<div class='card third'><div class='row'><div><div class='k'>Routeur 4G</div><div class='v'>"
         + String(powered?"Alimenté":"Coupé") + "</div></div>"
         + (powered?badgeWarn("ON"):badgeKo("OFF")) + "</div><small>Relais GPIO16</small></div>";

    s += "<div class='card half'><div class='k'>Actions</div><div class='btns' style='margin-top:10px'>"
         "<form method='POST' action='/reboot'><button class='orange' type='submit'>🔄 Reboot 4G</button></form>"
         "<form method='POST' action='/on'><button type='submit'>🟠 4G ON</button></form>"
         "<form method='POST' action='/off'><button class='red' type='submit'>⛔ 4G OFF</button></form>"
         "<a class='btn gray' href='/wifi'>⚙️ Wi-Fi</a>"
         "<form method='POST' action='/clear'><button class='red' type='submit'>🧹 Effacer logs</button></form>"
         "</div></div>";

    s += "<div class='card half'><div class='k'>Sécurité</div>"
         "<div class='row' style='margin-top:8px'><div><div class='k'>Reboots aujourd’hui</div><div class='v'>"
         + String(rebootCount) + " / " + String(maxRebootsPerDay) + "</div></div>"
         + badgeWarn("Limiteur") + "</div>"
         "<hr><small>Reset du compteur au changement de jour (NTP). "
         "Sans NTP valide, le compteur est conservé (anti-boucle).</small>"
         "<div style='height:8px'></div><small>Dernière cause : " + lastFailure + "</small>"
         "</div>";

    s += "<div class='card'><div class='row'><div><div class='k'>Historique</div><div class='v'>Logs</div></div>"
         "<small>"+ nowString() + "</small></div>"
         "<pre>"+ logBuffer + "</pre></div>";

    s += "</div>" + htmlFooter();
    request->send(200, "text/html", s);
  });

  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
    startRouterReboot("Reboot manuel (Web)");
    request->redirect("/");
  });

  server.on("/on", HTTP_POST, [](AsyncWebServerRequest *request){
    routerPowerOn();
    addLog("Routeur ON (Web)");
    flushLogIfNeeded(true);
    request->redirect("/");
  });

  server.on("/off", HTTP_POST, [](AsyncWebServerRequest *request){
    routerPowerOff();
    addLog("Routeur OFF (Web)");
    flushLogIfNeeded(true);
    request->redirect("/");
  });

  server.on("/clear", HTTP_POST, [](AsyncWebServerRequest *request){
    clearLog();
    request->redirect("/");
  });

  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request){
    String s = htmlHeader("Config Wi-Fi");
    s += "<h1>Configuration Wi-Fi</h1><div class='grid'><div class='card half'>"
         "<form method='POST' action='/savewifi'>"
         "<label>SSID</label><input name='ssid' value='"+ssid+"'>"
         "<div style='height:10px'></div>"
         "<label>Mot de passe</label><input type='password' name='pass' value='"+password+"'>"
         "<div style='height:12px'></div>"
         "<button type='submit'>💾 Enregistrer & redémarrer</button>"
         "</form>"
         "<div style='height:10px'></div><a class='btn gray' href='/'>⬅️ Retour</a>"
         "</div></div>" + htmlFooter();
    request->send(200, "text/html", s);
  });

  server.on("/savewifi", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("ssid", true) && request->hasParam("pass", true)) {
      String ns = request->getParam("ssid", true)->value();
      String np = request->getParam("pass", true)->value();
      saveCredentials(ns, np);
    }
    request->send(200, "text/html", "Wi-Fi enregistré. Redémarrage…");
    delay(400);
    ESP.restart();
  });

  server.begin();
}

// ------------------- PORTAIL AP (si pas de WiFi) -------------------
void setupAccessPoint() {
  portalMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress IP = WiFi.softAPIP();

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("AP Config WiFi");
  lcd.setCursor(0,1); lcd.print(IP);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String s = htmlHeader("Config Wi-Fi (AP)");
    s += "<h1>Configuration Wi-Fi</h1><div class='grid'><div class='card half'>"
         "<small>Connecte-toi à l'AP <b>"+String(AP_SSID)+"</b> puis saisis tes identifiants.</small><hr>"
         "<form method='POST' action='/save'>"
         "<label>SSID</label><input name='s'>"
         "<div style='height:10px'></div>"
         "<label>Mot de passe</label><input type='password' name='p'>"
         "<div style='height:12px'></div>"
         "<button type='submit'>💾 Enregistrer & redémarrer</button>"
         "</form></div></div>" + htmlFooter();
    request->send(200, "text/html", s);
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("s", true) && request->hasParam("p", true)) {
      saveCredentials(request->getParam("s", true)->value(),
                      request->getParam("p", true)->value());
    }
    request->send(200, "text/html", "Enregistré. Redémarrage…");
    delay(400);
    ESP.restart();
  });

  server.begin();
}

// ------------------- SETUP / LOOP -------------------
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(TEST_BUTTON_PIN, INPUT_PULLUP);
  pinMode(RESET_WIFI_PIN, INPUT_PULLUP);

  // SAFE BOOT : routeur ON (LOW = ON)
  routerPowerOn();

  lcd.begin(16,2);
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Demarrage...");
  lcd.setCursor(0,1); lcd.print("Routeur ON");

  loadCredentials();
  loadCounters();
  loadLog();

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  if (portalMode) {
    setupAccessPoint();
    return;
  }

  bool ok = connectToWiFi();
  if (!ok) {
    addLog("WiFi STA KO → AP config");
    flushLogIfNeeded(true);
    setupAccessPoint();
    return;
  }

  addLog("WiFi connecté : " + WiFi.localIP().toString());
  flushLogIfNeeded(true);

  setupWebServer();
}

void loop() {
  // Reset WiFi : bouton dédié (maintenir ~3s pour reset)
  // Ici on garde simple: appui long => reset, sinon rien.
  static bool rstPressed = false;
  static unsigned long rstT0 = 0;

  if (digitalRead(RESET_WIFI_PIN) == LOW) {
    if (!rstPressed) { rstPressed = true; rstT0 = millis(); }
  } else {
    if (rstPressed) {
      unsigned long dt = millis() - rstT0;
      if (dt >= 3000) {
        addLog("Reset WiFi (bouton)");
        flushLogIfNeeded(true);
        clearCredentials();
        ESP.restart();
      }
    }
    rstPressed = false;
  }

  if (portalMode) {
    delay(10);
    return;
  }

  // Tick reboot routeur (non-bloquant)
  tickRouterReboot();

  // Bouton test reboot routeur (manuel)
  if (digitalRead(TEST_BUTTON_PIN) == LOW) {
    delay(60);
    if (digitalRead(TEST_BUTTON_PIN) == LOW) {
      startRouterReboot("Reboot manuel (bouton)");
      while (digitalRead(TEST_BUTTON_PIN) == LOW) delay(10);
    }
  }

  // Surveillance périodique
  if (millis() - lastCheck > checkIntervalMs) {
    lastCheck = millis();
    checkInternetStable();
  }

  // LCD anti-marquage
  lcdStatus();

  // Flush logs (batch)
  flushLogIfNeeded(false);

  delay(15);
}