#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <WiFiManager.h>          // install "WiFiManager" by tzapu via Library Manager
#include <Arduino_GFX_Library.h>  // install "GFX Library for Arduino" by moononournation
#include <time.h>                 // NTP clock (so the wait screen has time before any push)

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;   // serves an OTA upload form at /update

// --- GeekMagic HelloCubic Lite / SmallTV-Ultra: ESP8266 + ST7789 240x240 ---
// Pins/SPI mirror the GeekMagic open firmware. CS is tied to GND, the backlight
// is ACTIVE LOW (GPIO5 LOW = on), and the panel needs SPI mode 3.
#define LCD_DC   0
#define LCD_RST  2
#define LCD_BL   5
#define LCD_ROT  0    // change to 2/4/6 if the image is rotated or mirrored
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, GFX_NOT_DEFINED /* CS -> GND */);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, LCD_ROT, true /* IPS */, 240, 240);

// Latest usage, pushed by the daemon. -1 = no data yet.
int sessionPct = -1;            // 5-hour utilization %
int weeklyPct  = -1;            // 7-day utilization %
unsigned long sessionTokens = 0;
unsigned long weeklyTokens = 0;
unsigned long lastUpdateMs = 0;

// api-mode extras (see daemon push). 0 = not provided.
unsigned long sessReset = 0;    // unix epoch when the 5h window resets
unsigned long weekReset = 0;    // unix epoch when the 7d window resets
String unifiedStatus = "";      // "allowed" / "rejected" / ... from the API
int bindingLimit = 0;           // 1 = session is binding, 2 = weekly, 0 = unknown

// UTC clock, synced from the daemon's pushed server time (no RTC/NTP needed).
unsigned long timeBaseEpoch = 0;   // server epoch at the moment of the last push
unsigned long timeBaseMillis = 0;  // millis() at that same moment
unsigned long lastTickEpoch = 0;   // last second we redrew the clock/countdown

// Current UTC epoch. Prefer the daemon-pushed server time (extrapolated via
// millis()); before the first push, fall back to NTP. 0 = no time source yet.
unsigned long nowEpoch() {
  if (timeBaseEpoch != 0)
    return timeBaseEpoch + (millis() - timeBaseMillis) / 1000UL;
  time_t t = time(nullptr);
  return (t > 1700000000) ? (unsigned long)t : 0;   // >2023 => NTP has synced
}

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Clawdmeter</title>
<style>
  :root{color-scheme:dark}
  body{font-family:system-ui,sans-serif;background:#14110f;color:#f4f1ea;margin:0;padding:24px;display:flex;flex-direction:column;gap:20px;max-width:520px;margin:0 auto}
  h1{font-size:20px;margin:0;font-weight:600}
  .sub{color:#9a9387;font-size:13px}
  .meter{background:#241f1b;border-radius:14px;padding:18px}
  .label{display:flex;justify-content:space-between;font-size:14px;margin-bottom:8px}
  .pct{font-variant-numeric:tabular-nums;font-weight:600}
  .track{background:#0f0d0b;border-radius:8px;height:18px;overflow:hidden}
  .fill{height:100%;width:0;border-radius:8px;transition:width .4s ease}
  .stale{opacity:.4}
</style></head><body>
<h1>&#128062; Clawdmeter</h1>
<div class="sub" id="status">connecting&hellip;</div>
<div class="meter">
  <div class="label"><span>Session (5h)</span><span class="pct" id="sp">&ndash;</span></div>
  <div class="sub" id="st">&ndash;</div>
  <div class="track"><div class="fill" id="sf"></div></div>
</div>
<div class="meter">
  <div class="label"><span>Weekly (7d)</span><span class="pct" id="wp">&ndash;</span></div>
  <div class="sub" id="wt">&ndash;</div>
  <div class="track"><div class="fill" id="wf"></div></div>
</div>
<script>
function color(p){return p>=90?'#d9534f':p>=60?'#e0a030':'#5fa463'}
var TZ=25200; // Asia/Bangkok UTC+7
function pad2(n){return (n<10?'0':'')+n}
function lt(e){return new Date((e+TZ)*1000)} // local (ICT) Date via UTC getters
function utcHHMM(e){var d=lt(e);return pad2(d.getUTCHours())+':'+pad2(d.getUTCMinutes())}
function utcClock(e){var d=lt(e);return utcHHMM(e)+':'+pad2(d.getUTCSeconds())+' ICT'}
var DOW=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
function countdown(secs){
  if(secs<=0)return 'T-0m';
  var h=Math.floor(secs/3600),m=Math.floor(secs%3600/60);
  return h>0?'T-'+h+'h'+pad2(m)+'m':'T-'+m+'m';
}
function set(pId,fId,v){
  var p=document.getElementById(pId),f=document.getElementById(fId);
  if(v<0){p.textContent='–';f.style.width='0';return;}
  p.textContent=v+'%';f.style.width=Math.min(v,100)+'%';f.style.background=color(v);
}
async function tick(){
  var st=document.getElementById('status');
  try{
    var d=await (await fetch('/usage.json',{cache:'no-store'})).json();
    set('sp','sf',d.s); set('wp','wf',d.w);
    var now=d.now||0;
    document.getElementById('st').textContent =
      d.sr ? 'resets '+utcHHMM(d.sr)+(now?' · '+countdown(d.sr-now):'')+' ICT' : '–';
    document.getElementById('wt').textContent =
      d.wr ? 'resets '+DOW[lt(d.wr).getUTCDay()]+' '+utcHHMM(d.wr)+' ICT' : '–';
    var clock=now?'  ·  '+utcClock(now):'';
    if(d.s<0){st.textContent='waiting for daemon…'+clock;st.className='sub';}
    else{
      var label=(d.stat||'').toUpperCase()||'updated '+d.age+'s ago';
      st.textContent=label+'  ·  updated '+d.age+'s ago'+clock;
      st.className=d.age>120?'sub stale':'sub';
    }
  }catch(e){st.textContent='device unreachable';}
}
tick();setInterval(tick,1000);
</script></body></html>
)HTML";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// Daemon pushes the numbers here:
// POST /usage?s=&w=&st=&wt=&sr=&wr=&stat=&bind=&t=
void handleUsage() {
  if (server.hasArg("s")) sessionPct = server.arg("s").toInt();
  if (server.hasArg("w")) weeklyPct  = server.arg("w").toInt();
  if (server.hasArg("st")) sessionTokens = strtoul(server.arg("st").c_str(), NULL, 10);
  if (server.hasArg("wt")) weeklyTokens = strtoul(server.arg("wt").c_str(), NULL, 10);
  if (server.hasArg("sr")) sessReset = strtoul(server.arg("sr").c_str(), NULL, 10);
  if (server.hasArg("wr")) weekReset = strtoul(server.arg("wr").c_str(), NULL, 10);
  if (server.hasArg("stat")) unifiedStatus = server.arg("stat");
  if (server.hasArg("bind")) bindingLimit = server.arg("bind").toInt();
  if (server.hasArg("t")) {
    unsigned long t = strtoul(server.arg("t").c_str(), NULL, 10);
    if (t > 0) { timeBaseEpoch = t; timeBaseMillis = millis(); }
  }
  lastUpdateMs = millis();
  lastTickEpoch = nowEpoch();
  drawMeter();
  server.send(200, "text/plain", "ok");
}

// The dashboard page polls this.
void handleUsageJson() {
  long age = (sessionPct < 0) ? -1 : (long)((millis() - lastUpdateMs) / 1000);
  String j = "{\"s\":" + String(sessionPct) +
             ",\"w\":" + String(weeklyPct) +
             ",\"st\":" + String(sessionTokens) +
             ",\"wt\":" + String(weeklyTokens) +
             ",\"sr\":" + String(sessReset) +
             ",\"wr\":" + String(weekReset) +
             ",\"stat\":\"" + unifiedStatus + "\"" +
             ",\"bind\":" + String(bindingLimit) +
             ",\"now\":" + String(nowEpoch()) +
             ",\"age\":" + String(age) + "}";
  server.send(200, "application/json", j);
}

// ---- ST7789 rendering ----
// Palette (hex literals; the GFX BLACK/WHITE macros don't resolve in lambdas).
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GRAY   0x8410
#define C_LINE   0x4208   // panel separators
#define C_GREEN  0x07E0
#define C_YELLOW 0xFFE0
#define C_RED    0xF800
#define C_AMBER  0xFD20
#define C_PANEL  0x2104   // IP panel background

// Display clock/reset times in Thailand time. Pushed epochs are UTC; add the
// offset only when formatting wall-clock text (durations/countdowns stay raw).
#define TZ_OFFSET 25200UL   // Asia/Bangkok, UTC+7 (no DST)
#define TZ_LABEL  "ICT"

static const char *const DOW[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *const MON[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec"};

static uint16_t barColor(int p) {
  if (p >= 90) return C_RED;
  if (p >= 60) return C_YELLOW;
  return C_GREEN;
}

static uint16_t statusColor() {
  if (unifiedStatus.length() == 0) return C_GRAY;
  if (unifiedStatus == "allowed") return C_GREEN;
  return C_RED;                         // rejected / blocked / queued ...
}

static String pad2(int v) {
  return (v < 10) ? "0" + String(v) : String(v);
}

// All times are UTC, derived from the daemon-pushed server epoch. Only seconds-
// of-day and day-of-week are needed (no calendar/leap math).
static String hhmmss(unsigned long e) {
  unsigned long s = e % 86400UL;
  return pad2(s / 3600) + ":" + pad2((s % 3600) / 60) + ":" + pad2(s % 60);
}
static String hhmm(unsigned long e) {
  unsigned long s = e % 86400UL;
  return pad2(s / 3600) + ":" + pad2((s % 3600) / 60);
}
static String dowName(unsigned long e) {
  return DOW[(int)((e / 86400UL + 4) % 7)];
}
static String countdown(long remaining) {
  if (remaining <= 0) return "T-0m";
  long h = remaining / 3600, m = (remaining % 3600) / 60;
  if (h > 0) return "T-" + String(h) + "h" + pad2(m) + "m";
  return "T-" + String(m) + "m";
}

// Right-align text ending at rightX (opaque, so it overprints cleanly).
static void printRight(int rightX, int y, uint8_t size, const String &s,
                       uint16_t fg, uint16_t bg) {
  gfx->setTextSize(size);
  gfx->setTextColor(fg, bg);
  gfx->setCursor(rightX - (int)s.length() * 6 * size, y);
  gfx->print(s);
}

// One usage block (session or weekly). y is the block's top edge.
static void drawBlock(int y, const char *label, int pct,
                      unsigned long reset, bool binding, bool isSession) {
  if (binding) gfx->fillRect(0, y + 2, 4, 80, C_AMBER);

  gfx->setTextSize(2);
  gfx->setTextColor(C_WHITE, C_BLACK);
  gfx->setCursor(12, y + 6);
  gfx->print(label);
  printRight(236, y + 6, 2, (pct < 0) ? "--" : String(pct) + "%", C_WHITE, C_BLACK);

  const int bx = 12, by = y + 30, bw = 216, bh = 18;
  gfx->fillRect(bx, by, bw, bh, C_BLACK);
  gfx->drawRect(bx, by, bw, bh, C_LINE);
  if (pct > 0) {
    int p = pct > 100 ? 100 : pct;
    gfx->fillRect(bx + 2, by + 2, (bw - 4) * p / 100, bh - 4, barColor(pct));
  }

  gfx->setTextSize(1);
  gfx->setTextColor(C_GRAY, C_BLACK);
  gfx->setCursor(12, y + 54);
  if (reset == 0) gfx->print("Reset --:--");
  else if (isSession) gfx->print("Reset " + hhmm(reset + TZ_OFFSET));
  else gfx->print("Reset " + dowName(reset + TZ_OFFSET) + " " + hhmm(reset + TZ_OFFSET));

  if (isSession) {
    String cd = (reset && nowEpoch()) ? countdown((long)reset - (long)nowEpoch()) : "T--";
    printRight(236, y + 54, 1, cd, C_WHITE, C_BLACK);
  }
}

static void drawIpPanel() {
  gfx->fillRect(0, 201, 240, 39, C_PANEL);
  gfx->setTextSize(1);
  gfx->setTextColor(C_GRAY, C_PANEL);
  gfx->setCursor(12, 214);
  gfx->print("IP");
  gfx->setTextSize(2);
  gfx->setTextColor(C_WHITE, C_PANEL);
  gfx->setCursor(36, 210);
  gfx->print(WiFi.localIP());
}

// Calendar date from a (TZ-adjusted) epoch — Howard Hinnant's civil_from_days.
static void civilFromEpoch(unsigned long e, int &year, int &month, int &day) {
  long z = (long)(e / 86400UL) + 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  long doe = z - era * 146097;                                // [0, 146096]
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long y = yoe + era * 400;
  long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);         // [0, 365]
  long mp = (5 * doy + 2) / 153;                              // [0, 11]
  day = (int)(doy - (153 * mp + 2) / 5 + 1);                 // [1, 31]
  month = (int)(mp < 10 ? mp + 3 : mp - 9);                  // [1, 12]
  year = (int)(y + (month <= 2));
}

// "sat 20 jun" from a TZ-adjusted epoch.
static String dateLine(unsigned long e) {
  int yr, mo, da;
  civilFromEpoch(e, yr, mo, da);
  String s = String(DOW[(int)((e / 86400UL + 4) % 7)]) + " " + String(da) + " " + MON[mo - 1];
  s.toLowerCase();
  return s;
}

// Big HH:MM + date on the wait screen (the only parts that change each minute).
static void drawWaitingTime() {
  unsigned long e = nowEpoch();
  unsigned long le = e ? e + TZ_OFFSET : 0;        // Thailand time
  String t = e ? hhmm(le) : String("--:--");
  gfx->fillRect(0, 40, 240, 40, C_BLACK);
  gfx->setTextSize(4);
  gfx->setTextColor(C_WHITE, C_BLACK);
  gfx->setCursor((240 - (int)t.length() * 24) / 2, 44);
  gfx->print(t);

  String d = e ? dateLine(le) : String("--");
  gfx->fillRect(0, 90, 240, 18, C_BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(C_GRAY, C_BLACK);
  gfx->setCursor((240 - (int)d.length() * 12) / 2, 92);
  gfx->print(d);
}

static void drawWaiting() {
  drawWaitingTime();
  gfx->drawFastHLine(30, 124, 180, C_LINE);
  gfx->setTextSize(2);
  gfx->setTextColor(C_WHITE, C_BLACK);
  gfx->setCursor(12, 140);
  gfx->print("waiting for daemon");
  drawIpPanel();
}

// Redraw only the once-per-second fields (clock + session countdown) so the
// rest of the screen doesn't flicker on every tick.
void tickDynamic() {
  unsigned long e = nowEpoch();
  if (e == 0) return;
  gfx->fillRect(150, 8, 86, 10, C_BLACK);
  printRight(236, 9, 1, hhmmss(e + TZ_OFFSET) + " " TZ_LABEL, C_WHITE, C_BLACK);
  if (sessReset) {
    gfx->fillRect(150, 82, 86, 10, C_BLACK);
    printRight(236, 82, 1, countdown((long)sessReset - (long)e), C_WHITE, C_BLACK);
  }
}

void drawMeter() {
  gfx->fillScreen(C_BLACK);

  if (sessionPct < 0 && weeklyPct < 0) { drawWaiting(); return; }

  // Status band: "CLAUDE USAGE" title + status dot/word + Thailand-time clock.
  gfx->setTextSize(1);
  gfx->setTextColor(C_WHITE, C_BLACK);
  gfx->setCursor(4, 9);
  gfx->print("CLAUDE USAGE");
  if (unifiedStatus.length()) {
    uint16_t c = statusColor();
    gfx->fillCircle(86, 13, 4, c);
    String label = unifiedStatus;
    label.toUpperCase();
    gfx->setTextColor(c, C_BLACK);
    gfx->setCursor(94, 9);
    gfx->print(label);
  }
  unsigned long e = nowEpoch();
  printRight(236, 9, 1, e ? hhmmss(e + TZ_OFFSET) + " " TZ_LABEL
                          : String("--:--:-- " TZ_LABEL), C_WHITE, C_BLACK);
  gfx->drawFastHLine(0, 27, 240, C_LINE);

  drawBlock(28, "SESSION 5h", sessionPct, sessReset, bindingLimit == 1, true);
  gfx->drawFastHLine(0, 113, 240, C_LINE);
  drawBlock(114, "WEEKLY 7d", weeklyPct, weekReset, bindingLimit == 2, false);
  gfx->drawFastHLine(0, 198, 240, C_LINE);   // double rule under weekly
  gfx->drawFastHLine(0, 200, 240, C_LINE);

  drawIpPanel();
}

void setup() {
  Serial.begin(115200);

  // --- Display init (mirrors the GeekMagic open firmware) ---
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LOW);          // backlight is ACTIVE LOW -> LOW = on
  // Arduino_GFX defaults ST7789 on ESP8266 to SPI_MODE2; this panel needs mode 3.
  // Start the bus ourselves and tell gfx->begin() not to reconfigure it.
  bus->begin(40000000, SPI_MODE3);
  gfx->begin(GFX_SKIP_DATABUS_BEGIN);  // hardware reset (RST) + ST7789 init
  gfx->fillScreen(0x0000);
  gfx->setTextColor(0xFFFF, 0x0000);
  gfx->setTextSize(2);
  gfx->setCursor(12, 100);
  gfx->print("starting...");

  // Tries the saved network; if it can't connect it opens a "Clawdmeter-setup"
  // hotspot — join it from your phone to pick a new WiFi. No re-flashing needed.
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);          // give up after 3 min and reboot/retry
  wm.setAPCallback([](WiFiManager *m) {    // show setup hint on the screen
    gfx->fillScreen(0x0000);
    gfx->setTextColor(0xFFFF, 0x0000);
    gfx->setTextSize(2);
    gfx->setCursor(12, 70);  gfx->print("Setup WiFi:");
    gfx->setCursor(12, 100); gfx->print("join hotspot");
    gfx->setTextColor(0xFD20, 0x0000);
    gfx->setCursor(12, 130); gfx->print("Clawdmeter-setup");
  });
  if (!wm.autoConnect("Clawdmeter-setup")) {
    Serial.println("WiFi setup timed out, restarting...");
    ESP.restart();
  }
  Serial.print("Connected: http://");
  Serial.println(WiFi.localIP());

  // NTP in UTC (we apply the ICT offset at display time). This gives the wait
  // screen a clock before the daemon ever pushes; daemon time takes over later.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Stable hostname so the daemon/browser don't chase IPs: http://clawdmeter.local/
  if (MDNS.begin("clawdmeter")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("Also at: http://clawdmeter.local/");
  }

  drawMeter();   // shows "waiting for daemon..." + the device IP until data arrives

  server.on("/", handleRoot);
  server.on("/usage", HTTP_POST, handleUsage);
  server.on("/usage", HTTP_GET, handleUsage);   // GET allowed too, handy for testing
  server.on("/usage.json", handleUsageJson);
  httpUpdater.setup(&server);   // OTA: upload a .bin at http://clawdmeter.local/update
  Serial.println("OTA update: http://clawdmeter.local/update");
  server.begin();
}

void loop() {
  MDNS.update();
  server.handleClient();

  unsigned long e = nowEpoch();
  if (e == 0) return;
  if (sessionPct < 0 && weeklyPct < 0) {
    // Wait screen: refresh the big clock/date once a minute.
    unsigned long minute = e / 60UL;
    if (minute != lastTickEpoch) { lastTickEpoch = minute; drawWaitingTime(); }
  } else if (e != lastTickEpoch) {
    // With data: tick the clock + countdown once a second, no full redraw.
    lastTickEpoch = e;
    tickDynamic();
  }
}
