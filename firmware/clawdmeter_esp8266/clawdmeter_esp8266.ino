#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <WiFiManager.h>          // install "WiFiManager" by tzapu via Library Manager
#include <Arduino_GFX_Library.h>  // install "GFX Library for Arduino" by moononournation

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
  <div class="label"><span>Local session (5h)</span><span class="pct" id="sp">&ndash;</span></div>
  <div class="sub" id="st">&ndash;</div>
  <div class="track"><div class="fill" id="sf"></div></div>
</div>
<div class="meter">
  <div class="label"><span>Local weekly (7d)</span><span class="pct" id="wp">&ndash;</span></div>
  <div class="sub" id="wt">&ndash;</div>
  <div class="track"><div class="fill" id="wf"></div></div>
</div>
<script>
function color(p){return p>=90?'#d9534f':p>=60?'#e0a030':'#5fa463'}
function fmtTok(n){
  if(n>=1000000)return (n/1000000).toFixed(1)+'M tokens';
  if(n>=1000)return (n/1000).toFixed(1)+'K tokens';
  return n+' tokens';
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
    document.getElementById('st').textContent=fmtTok(d.st||0);
    document.getElementById('wt').textContent=fmtTok(d.wt||0);
    if(d.s<0){st.textContent='waiting for daemon…';st.className='sub';}
    else{st.textContent='updated '+d.age+'s ago';st.className=d.age>120?'sub stale':'sub';}
  }catch(e){st.textContent='device unreachable';}
}
tick();setInterval(tick,3000);
</script></body></html>
)HTML";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// Daemon pushes the numbers here: POST /usage?s=<int>&w=<int>&st=<tokens>&wt=<tokens>
void handleUsage() {
  if (server.hasArg("s")) sessionPct = server.arg("s").toInt();
  if (server.hasArg("w")) weeklyPct  = server.arg("w").toInt();
  if (server.hasArg("st")) sessionTokens = strtoul(server.arg("st").c_str(), NULL, 10);
  if (server.hasArg("wt")) weeklyTokens = strtoul(server.arg("wt").c_str(), NULL, 10);
  lastUpdateMs = millis();
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
             ",\"age\":" + String(age) + "}";
  server.send(200, "application/json", j);
}

// ---- ST7789 rendering ----
static uint16_t barColor(int p) {
  if (p >= 90) return 0xF800;   // red
  if (p >= 60) return 0xFFE0;   // yellow
  return 0x07E0;                // green
}

static String formatTokens(unsigned long tokens) {
  if (tokens >= 1000000UL) {
    return String(tokens / 1000000.0, 1) + "M tok";
  }
  if (tokens >= 1000UL) {
    return String(tokens / 1000.0, 1) + "K tok";
  }
  return String(tokens) + " tok";
}

static void drawBar(int y, const char *label, int pct, unsigned long tokens) {
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF, 0x0000);
  gfx->setCursor(12, y);
  gfx->print(label);
  gfx->setCursor(176, y);
  if (pct < 0) gfx->print("--");
  else { gfx->print(pct); gfx->print('%'); }

  gfx->setTextSize(1);
  gfx->setTextColor(0x8410, 0x0000);
  gfx->setCursor(12, y + 22);
  gfx->print(formatTokens(tokens));

  const int bx = 12, by = y + 36, bw = 216, bh = 24;
  gfx->fillRect(bx, by, bw, bh, 0x0000);
  gfx->drawRect(bx, by, bw, bh, 0xFFFF);
  if (pct > 0) {
    int p = pct > 100 ? 100 : pct;
    gfx->fillRect(bx + 2, by + 2, (bw - 4) * p / 100, bh - 4, barColor(pct));
  }
}

void drawMeter() {
  gfx->fillScreen(0x0000);
  gfx->setTextSize(2);
  gfx->setTextColor(0xFD20, 0x0000);   // orange title
  gfx->setCursor(12, 8);
  gfx->print("Clawdmeter");
  gfx->setTextSize(1);
  gfx->setTextColor(0x8410, 0x0000);
  gfx->setCursor(12, 30);
  gfx->print("local Claude Code logs");

  if (sessionPct < 0 && weeklyPct < 0) {
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF, 0x0000);
    gfx->setCursor(12, 110); gfx->print("waiting for");
    gfx->setCursor(12, 135); gfx->print("daemon...");
  } else {
    drawBar(52,  "Session 5h", sessionPct, sessionTokens);
    drawBar(132, "Weekly 7d",  weeklyPct, weeklyTokens);
  }

  gfx->setTextSize(1);
  gfx->setTextColor(0x8410, 0x0000);   // gray IP at the bottom
  gfx->setCursor(12, 226);
  gfx->print(WiFi.localIP());
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
}
