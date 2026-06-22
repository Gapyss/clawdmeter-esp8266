#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Updater.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <WiFiManager.h>          // install "WiFiManager" by tzapu via Library Manager
#include <Arduino_GFX_Library.h>  // install "GFX Library for Arduino" by moononournation
#include <time.h>                 // NTP clock (so the wait screen has time before any push)

ESP8266WebServer server(80);

// --- GeekMagic HelloCubic Lite / SmallTV-Ultra: ESP8266 + ST7789 240x240 ---
// Pins/SPI mirror the GeekMagic open firmware. CS is tied to GND, the backlight
// is ACTIVE LOW (GPIO5 LOW = on), and the panel needs SPI mode 3.
#define LCD_DC   0
#define LCD_RST  2
#define LCD_BL   5
#define LCD_ROT  0    // change to 2/4/6 if the image is rotated or mirrored
// Backlight brightness 0..255. Was hardwired full-on (255), which runs the panel
// hot; the LED string behind the glass is the main "screen is hot" heat source.
// ~90 is plenty indoors. Lower = cooler + less current (also eases the regulator).
#define LCD_BRIGHTNESS 90
#define LCD_MAX_BRIGHTNESS 120
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, GFX_NOT_DEFINED /* CS -> GND */);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, LCD_ROT, true /* IPS */, 240, 240);

// Companion-face mood descriptor (the "Claude mascot" screen). Defined up here
// because Arduino auto-generates function prototypes at the top of the file, so a
// struct used as a parameter/field type must already be visible at that point.
struct FaceExpr {
  uint16_t body;     // mascot body color
  uint16_t spark;    // mood "antenna" spark + status-text color
  uint8_t  eyeH;     // open-eye height (px)
  uint8_t  mouth;    // 0 smile, 1 flat, 2 open, 3 sleepy
  uint8_t  id;       // mood id, for cheap change detection
  const char *mood;  // status label
};

// Face-animation frame types. Also defined up here (same auto-prototype reason as
// FaceExpr): the scene-selector helpers return/take these, so they must be visible
// where Arduino injects their prototypes at the top of the file.
// A sparse cell patch {row, col, value} applied on top of the (shifted) base.
struct FacePatch { uint8_t r, c, v; };
// One animation step: hold (ms), base shift (dr,dc), and an optional cell patch.
struct FaceFrame { uint16_t hold; int8_t dr, dc; const FacePatch *ops; uint8_t nops; };
#define FACE_PATCH(a) (a), (uint8_t)(sizeof(a) / sizeof((a)[0]))

static const uint8_t EEPROM_MARKER_ADDR = 0;
static const uint8_t EEPROM_BRIGHTNESS_ADDR = 1;
static const uint8_t EEPROM_MARKER = 0xC1;
uint8_t lcdBrightness = LCD_BRIGHTNESS;

// Backlight is ACTIVE LOW and the pin supports software PWM. analogWrite sets the
// HIGH duty, and HIGH = off here, so invert: brightness 255 -> duty 0 (full on),
// brightness 0 -> duty 255 (off). The PWM is a software waveform on an IRAM timer
// ISR (~2 edges/period), so its CPU cost scales with frequency. Keep it LOW: at
// 20 kHz the ISR starved the WiFi/TCP stack and crash-rebooted the device under any
// HTTP load. 1 kHz (1/20th the interrupt rate) is well above flicker fusion and
// leaves the radio/server responsive. Extremes (brightness 0/255 -> duty 255/0)
// stop the waveform entirely, which is what makes a flash write safe (see OTA).
#define LCD_PWM_FREQ 1000
static void setBacklight(uint8_t brightness) {
  analogWriteRange(255);
  analogWriteFreq(LCD_PWM_FREQ);
  analogWrite(LCD_BL, 255 - brightness);
}

// Silence the PWM waveform ISR before any SPI-flash erase/write (OTA, EEPROM): a
// timer interrupt firing mid-flash resets the ESP8266 (deterministic crash). duty 0
// detaches the pin from the waveform generator and leaves the backlight full-on.
static void backlightStopForFlash() {
  analogWrite(LCD_BL, 0);
}

static uint8_t loadBrightness() {
  if (EEPROM.read(EEPROM_MARKER_ADDR) != EEPROM_MARKER) return LCD_BRIGHTNESS;
  return min((uint8_t)EEPROM.read(EEPROM_BRIGHTNESS_ADDR), (uint8_t)LCD_MAX_BRIGHTNESS);
}

static void saveBrightness(uint8_t brightness) {
  if (EEPROM.read(EEPROM_MARKER_ADDR) == EEPROM_MARKER &&
      EEPROM.read(EEPROM_BRIGHTNESS_ADDR) == brightness) {
    return;
  }
  EEPROM.write(EEPROM_MARKER_ADDR, EEPROM_MARKER);
  EEPROM.write(EEPROM_BRIGHTNESS_ADDR, brightness);
  backlightStopForFlash();   // no PWM ISR during the flash commit
  EEPROM.commit();
  setBacklight(lcdBrightness);  // restore the live level
}

static void applyBrightness(uint8_t brightness, bool persist) {
  lcdBrightness = min(brightness, (uint8_t)LCD_MAX_BRIGHTNESS);
  setBacklight(lcdBrightness);
  if (persist) saveBrightness(lcdBrightness);
}

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
int macCpuPct = -1;             // macOS CPU use, pushed by daemon
int macMemPct = -1;             // macOS memory use
int macDiskPct = -1;            // macOS home volume disk use
int macBatteryPct = -1;         // Mac battery level, -1 if unavailable
static const uint8_t SCREEN_CLAUDE = 0;
static const uint8_t SCREEN_MAC = 1;
static const uint8_t SCREEN_DESK = 2;
static const uint8_t SCREEN_FACE = 3;
uint8_t lcdScreen = SCREEN_CLAUDE;
static const uint8_t FACE_MODE_IDLE = 0;
static const uint8_t FACE_MODE_WORKING = 1;
static const uint8_t FACE_MODE_SLEEP = 2;
static const uint8_t FACE_MODE_MONK = 3;
static const uint8_t FACE_MODE_COUNT = 4;   // for the /face?state=toggle wrap
uint8_t faceMode = FACE_MODE_IDLE; // runtime-only face sub-state
String deskStatus = "coding";   // coding / meeting / busy / break / claude
String deskText = "CODING";
String deskColorName = "green";
unsigned long deskTypingStartMs = 0;
unsigned long deskAnimLastMs = 0;
String deskLastShown = "";       // last drawn typing frame; skip redraw when unchanged
bool otaInProgress = false;     // true while /update is writing firmware
bool otaUpdateOk = false;
bool meterRedrawPending = false;
bool macChromeReady = false;    // MAC screen chrome is static; dynamic pushes repaint only values
bool claudeChromeReady = false; // CLAUDE screen chrome (cream card) is static; pushes repaint only values
String otaError = "";
String bootReason = "";         // why the chip last reset (captured once at boot)
String bootInfo = "";           // detailed reset info (exception cause/stack on crash)

// UTC clock, synced from the daemon's pushed server time (no RTC/NTP needed).
unsigned long timeBaseEpoch = 0;   // server epoch at the moment of the last push
unsigned long timeBaseMillis = 0;  // millis() at that same moment
unsigned long lastTickEpoch = 0;   // last second we redrew the clock/countdown
uint8_t claudeStatusPulsePhase = 255; // last drawn Claude status pulse frame

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
  :root{color-scheme:dark;--bg:#0d1117;--panel:#161b22;--panel2:#10151d;--line:#30363d;--text:#f0f6fc;--muted:#8b949e;--green:#3fb950;--yellow:#d29922;--red:#f85149;--blue:#58a6ff;--claude:#d97757}
  *{box-sizing:border-box}
  body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,sans-serif;background:var(--bg);color:var(--text);margin:0;min-height:100vh}
  main{width:min(980px,100%);margin:0 auto;padding:22px;display:grid;gap:16px}
  header{display:flex;align-items:flex-start;justify-content:space-between;gap:16px;border-bottom:1px solid var(--line);padding-bottom:14px}
  h1{font-size:24px;line-height:1.1;margin:0;font-weight:700;letter-spacing:0}
  .sub,.muted{color:var(--muted);font-size:13px}
  .status{display:flex;align-items:center;gap:8px;justify-content:flex-end;flex-wrap:wrap;text-align:right}
  .dot{width:10px;height:10px;border-radius:50%;background:var(--muted);box-shadow:0 0 0 3px rgba(139,148,158,.15)}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}
  .panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:16px}
  .meter{position:relative;overflow:hidden}
  .meter.bind{border-color:var(--yellow)}
  .meter.bind:before{content:"";position:absolute;left:0;top:0;bottom:0;width:4px;background:var(--yellow)}
  .label{display:flex;align-items:baseline;justify-content:space-between;gap:14px;margin-bottom:12px}
  .name{font-size:15px;font-weight:650}
  .pct{font-size:34px;line-height:1;font-weight:750;font-variant-numeric:tabular-nums}
  .track{height:18px;background:#06080c;border:1px solid var(--line);border-radius:999px;overflow:hidden}
  .fill{height:100%;width:0;background:var(--green);transition:width .35s ease,background .35s ease}
  .row{display:grid;grid-template-columns:1fr auto;gap:12px;margin-top:12px;font-size:13px;color:var(--muted)}
  .stats{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}
  .stat{background:var(--panel2);border:1px solid var(--line);border-radius:8px;padding:14px}
  .k{color:var(--muted);font-size:12px;margin-bottom:6px}
  .v{font-size:20px;font-weight:650;font-variant-numeric:tabular-nums;white-space:nowrap}
  .actions{display:flex;gap:10px;flex-wrap:wrap}
  .btn{color:var(--text);text-decoration:none;border:1px solid var(--line);background:var(--panel2);border-radius:8px;padding:9px 12px;font-size:14px;cursor:pointer}
  .btn:hover{border-color:var(--blue)}
  .btn.active{border-color:var(--blue);background:#0f2236}
  .btn.danger{border-color:#8e2b2b}
  .btn.danger:hover{border-color:var(--red)}
  .control{display:grid;grid-template-columns:auto 1fr auto;align-items:center;gap:12px;margin-top:14px}
  .deskCustom{display:grid;grid-template-columns:1fr auto auto;gap:10px}
  input[type=text],select{width:100%;color:var(--text);background:var(--panel2);border:1px solid var(--line);border-radius:8px;padding:9px 10px}
  input[type=range]{width:100%;accent-color:var(--blue)}
  .stale{opacity:.55}
  @media(max-width:760px){main{padding:16px}.grid,.stats{grid-template-columns:1fr}header{display:grid}.status{text-align:left;justify-content:flex-start}.pct{font-size:30px}.control{grid-template-columns:1fr}.control .v{text-align:left}}
</style></head><body>
<main>
<header>
  <div><h1 id="title">Clawdmeter</h1><div class="sub" id="subtitle">Claude usage monitor</div></div>
  <div class="status"><span class="dot" id="dot"></span><span id="status">connecting...</span><a class="btn" id="switchMode" href="?view=mac">Mac</a></div>
</header>
<section class="grid">
  <div class="panel meter" id="sessionCard">
    <div class="label"><span class="name" id="sessionName">Session window</span><span class="pct" id="sp">--</span></div>
    <div class="track"><div class="fill" id="sf"></div></div>
    <div class="row"><span id="st">Reset --</span><span id="sc">--</span></div>
  </div>
  <div class="panel meter" id="weeklyCard">
    <div class="label"><span class="name" id="weeklyName">Weekly window</span><span class="pct" id="wp">--</span></div>
    <div class="track"><div class="fill" id="wf"></div></div>
    <div class="row"><span id="wt">Reset --</span><span id="wc">7 days</span></div>
  </div>
</section>
<section class="stats">
  <div class="stat"><div class="k" id="stat1Name">Clock</div><div class="v" id="clock">--:--:--</div></div>
  <div class="stat"><div class="k" id="stat2Name">Last push</div><div class="v" id="age">--</div></div>
  <div class="stat"><div class="k" id="stat3Name">Session tokens</div><div class="v" id="stok">--</div></div>
  <div class="stat"><div class="k" id="stat4Name">Weekly tokens</div><div class="v" id="wtok">--</div></div>
</section>
<section class="panel">
  <div class="label"><span class="name">Device</span><span class="muted" id="mode">waiting</span></div>
  <div class="actions"><a class="btn" href="/usage.json">Usage JSON</a><a class="btn" href="/update">OTA Update</a><a class="btn" href="/restart" id="restartDevice">Restart</a><a class="btn danger" href="/factory-reset" id="factoryReset">Reset Settings</a></div>
  <div class="control"><span class="muted">Companion</span><div class="actions"><button class="btn faceBtn" data-state="idle">Idle</button><button class="btn faceBtn" data-state="working">Working</button><button class="btn faceBtn" data-state="sleep">Sleep</button><button class="btn faceBtn" data-state="monk">Monk</button></div><span class="v" id="faceValue">--</span></div>
  <div class="control"><span class="muted">Desk</span><div class="actions"><button class="btn deskBtn" data-status="coding">Coding</button><button class="btn deskBtn" data-status="meeting">Meeting</button><button class="btn deskBtn" data-status="claude">Claude</button><button class="btn deskBtn" data-status="busy">Busy</button><button class="btn deskBtn" data-status="break">Break</button></div><span class="v" id="deskValue">--</span></div>
  <div class="control"><span class="muted">Custom</span><div class="deskCustom"><input id="deskText" type="text" maxlength="12" value="CODING"><select id="deskColor"><option value="green">Green</option><option value="claude">Claude</option><option value="red">Red</option><option value="amber">Amber</option><option value="blue">Blue</option><option value="white">White</option></select><button class="btn" id="deskApply">Apply</button></div><span></span></div>
  <div class="control"><span class="muted">Brightness</span><input id="brightness" type="range" min="0" max="120" value="90"><span class="v" id="brightnessValue">90</span></div>
</section>
</main>
<script>
function color(p){return p>=90?'#f85149':p>=60?'#d29922':'#3fb950'}
var TZ=25200; // Asia/Bangkok UTC+7
function pad2(n){return (n<10?'0':'')+n}
function lt(e){return new Date((e+TZ)*1000)} // local UTC+7 Date via UTC getters
function utcHHMM(e){var d=lt(e);return pad2(d.getUTCHours())+':'+pad2(d.getUTCMinutes())}
function utcClock(e){var d=lt(e);return utcHHMM(e)+':'+pad2(d.getUTCSeconds())}
var DOW=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
function countdown(secs){
  if(secs<=0)return 'T-0m';
  var h=Math.floor(secs/3600),m=Math.floor(secs%3600/60);
  return h>0?'T-'+h+'h'+pad2(m)+'m':'T-'+m+'m';
}
function commas(n){return n>0?String(n).replace(/\B(?=(\d{3})+(?!\d))/g,','):'--'}
function ageText(a){return a<0?'--':a<60?a+'s':Math.floor(a/60)+'m '+pad2(a%60)+'s'}
function pctText(v){return v>=0?v+'%':'--'}
function set(pId,fId,v){
  var p=document.getElementById(pId),f=document.getElementById(fId);
  if(v<0){p.textContent='--';f.style.width='0';return;}
  p.textContent=v+'%';f.style.width=Math.min(v,100)+'%';f.style.background=color(v);
}
function storedView(){
  var q=location.search;
  if(q.indexOf('view=mac')>=0)return 'mac';
  if(q.indexOf('view=desk')>=0)return 'desk';
  if(q.indexOf('view=face')>=0)return 'face';
  if(q.indexOf('view=claude')>=0)return 'claude';
  try{return localStorage.getItem('view')||'claude';}catch(e){return 'claude';}
}
var view=storedView();
var brightnessBusy=false,brightnessTimer=0;
function saveView(v){
  try{localStorage.setItem('view',v);}catch(e){}
  if(history.replaceState)history.replaceState(null,'','?view='+v);
  fetch('/mode?screen='+v,{cache:'no-store'}).catch(function(e){});
}
document.getElementById('switchMode').onclick=function(e){
  if(e&&e.preventDefault)e.preventDefault();
  view=view=='claude'?'mac':view=='mac'?'desk':view=='desk'?'face':'claude';
  saveView(view);
  tick();
};
function setDeskUi(status){
  document.getElementById('deskValue').textContent=status?status.toUpperCase():'--';
  var bs=document.querySelectorAll('.deskBtn');
  for(var i=0;i<bs.length;i++)bs[i].className='btn deskBtn'+(bs[i].getAttribute('data-status')==status?' active':'');
}
function setDeskCustomUi(text,color){
  var active=document.activeElement&&document.activeElement.id;
  if(text&&active!='deskText')document.getElementById('deskText').value=text;
  if(color&&active!='deskColor')document.getElementById('deskColor').value=color;
}
function setFaceUi(state){
  document.getElementById('faceValue').textContent=state?state.toUpperCase():'--';
  var bs=document.querySelectorAll('.faceBtn');
  for(var i=0;i<bs.length;i++)bs[i].className='btn faceBtn'+(bs[i].getAttribute('data-state')==state?' active':'');
}
var faceBtns=document.querySelectorAll('.faceBtn');
for(var fi=0;fi<faceBtns.length;fi++){
  faceBtns[fi].onclick=function(e){
    if(e&&e.preventDefault)e.preventDefault();
    var s=this.getAttribute('data-state');
    setFaceUi(s);
    view='face';
    saveView(view);
    fetch('/face?state='+s,{cache:'no-store',method:'POST'}).then(tick).catch(function(e){});
  };
}
var deskBtns=document.querySelectorAll('.deskBtn');
for(var di=0;di<deskBtns.length;di++){
  deskBtns[di].onclick=function(e){
    if(e&&e.preventDefault)e.preventDefault();
    var s=this.getAttribute('data-status');
    setDeskUi(s);
    view='desk';
    saveView(view);
    fetch('/desk?status='+s,{cache:'no-store',method:'POST'}).then(tick).catch(function(e){});
  };
}
document.getElementById('deskApply').onclick=function(e){
  if(e&&e.preventDefault)e.preventDefault();
  var text=document.getElementById('deskText').value||'CODING';
  var color=document.getElementById('deskColor').value||'green';
  view='desk';
  saveView(view);
  fetch('/desk?text='+encodeURIComponent(text)+'&color='+encodeURIComponent(color),{cache:'no-store',method:'POST'}).then(tick).catch(function(e){});
};
document.getElementById('factoryReset').onclick=function(e){
  if(!confirm('Reset WiFi and brightness settings, then reboot?'))e.preventDefault();
};
document.getElementById('restartDevice').onclick=function(e){
  if(!confirm('Restart Clawdmeter device?'))e.preventDefault();
};
var brightness=document.getElementById('brightness');
var brightnessValue=document.getElementById('brightnessValue');
var ticking=false;
function setBrightnessUi(v){
  brightness.value=v;
  brightnessValue.textContent=v;
}
brightness.oninput=function(){
  var v=parseInt(brightness.value,10)||0;
  setBrightnessUi(v);
  brightnessBusy=true;
  clearTimeout(brightnessTimer);
  brightnessTimer=setTimeout(function(){
    fetch('/brightness?value='+v,{cache:'no-store',method:'POST'}).catch(function(e){});
    brightnessBusy=false;
  },120);
};
async function tick(){
  if(ticking)return;
  ticking=true;
  var st=document.getElementById('status'),dot=document.getElementById('dot');
  try{
    var d=await (await fetch('/usage.json',{cache:'no-store'})).json();
    if(!brightnessBusy&&d.bl>=0)setBrightnessUi(d.bl);
    setDeskUi(d.desk||'coding');
    setDeskCustomUi(d.deskText||'CODING',d.deskColor||'green');
    setFaceUi(d.face||'idle');
    var now=d.now||0;
    document.getElementById('clock').textContent=now?utcClock(now):'--:--:--';
    document.getElementById('age').textContent=ageText(d.age);
    var live=d.age>=0&&d.age<=120;
    document.body.className=live?'':'stale';
    if(view=='desk'){
      document.getElementById('title').textContent='Desk Status';
      document.getElementById('subtitle').textContent='Physical status sign';
      document.getElementById('switchMode').textContent='Face';
      document.getElementById('switchMode').href='?view=face';
      document.getElementById('sessionName').textContent='Current status';
      document.getElementById('weeklyName').textContent='Screen mode';
      document.getElementById('sessionCard').className='panel meter';
      document.getElementById('weeklyCard').className='panel meter';
      set('sp','sf',-1); set('wp','wf',-1);
      document.getElementById('sp').textContent=(d.deskText||d.desk||'coding').toUpperCase();
      document.getElementById('wp').textContent='DESK';
      document.getElementById('st').textContent='Pushed from dashboard';
      document.getElementById('sc').textContent='';
      document.getElementById('wt').textContent='LCD status sign';
      document.getElementById('wc').textContent='';
      document.getElementById('stat3Name').textContent='Clock';
      document.getElementById('stat4Name').textContent='Last push';
      document.getElementById('stok').textContent=now?utcClock(now):'--:--:--';
      document.getElementById('wtok').textContent=ageText(d.age);
      st.textContent='DESK - '+(d.deskText||d.desk||'coding').toUpperCase();
      dot.style.background=d.deskColor=='red'?'var(--red)':d.deskColor=='amber'?'var(--yellow)':d.deskColor=='blue'?'var(--blue)':d.deskColor=='white'?'var(--text)':d.deskColor=='claude'?'var(--claude)':'var(--green)';
      document.getElementById('mode').textContent='desk status sign';
    }else if(view=='mac'){
      document.getElementById('title').textContent='Mac Monitor';
      document.getElementById('subtitle').textContent='System status from daemon';
      document.getElementById('switchMode').textContent='Desk';
      document.getElementById('switchMode').href='?view=desk';
      document.getElementById('sessionName').textContent='CPU';
      document.getElementById('weeklyName').textContent='Memory';
      document.getElementById('sessionCard').className='panel meter';
      document.getElementById('weeklyCard').className='panel meter';
      set('sp','sf',d.cpu); set('wp','wf',d.mem);
      document.getElementById('st').textContent='CPU usage';
      document.getElementById('sc').textContent='';
      document.getElementById('wt').textContent='Memory used';
      document.getElementById('wc').textContent='';
      document.getElementById('stat3Name').textContent='Disk used';
      document.getElementById('stat4Name').textContent='Battery';
      document.getElementById('stok').textContent=pctText(d.disk);
      document.getElementById('wtok').textContent=pctText(d.bat);
      st.textContent='MAC - updated '+ageText(d.age)+' ago';
      dot.style.background=live?'var(--green)':'var(--yellow)';
      document.getElementById('mode').textContent='mac system metrics';
    }else if(view=='face'){
      document.getElementById('title').textContent='Companion';
      document.getElementById('subtitle').textContent=d.face=='working'?'Coding at the desk':d.face=='sleep'?'Claude sleeping on the cube':d.face=='monk'?'Claude meditating on the cube':'Claude mascot on the cube';
      document.getElementById('switchMode').textContent='Claude';
      document.getElementById('switchMode').href='?view=claude';
      document.getElementById('sessionName').textContent='Session window';
      document.getElementById('weeklyName').textContent='Weekly window';
      document.getElementById('sessionCard').className='panel meter';
      document.getElementById('weeklyCard').className='panel meter';
      set('sp','sf',d.s); set('wp','wf',d.w);
      document.getElementById('stok').textContent=commas(d.st);
      document.getElementById('wtok').textContent=commas(d.wt);
      document.getElementById('stat3Name').textContent='Session tokens';
      document.getElementById('stat4Name').textContent='Weekly tokens';
      document.getElementById('st').textContent='Mood from usage';
      document.getElementById('sc').textContent='';
      document.getElementById('wt').textContent='Live on the LCD';
      document.getElementById('wc').textContent='';
      var mood=d.face=='working'?'CODING':d.face=='sleep'?'SLEEP':d.face=='monk'?'ZEN':(d.s>=80||d.w>=90||(d.stat&&d.stat!='allowed')?'ALERT':d.s>=40?'FOCUS':'HAPPY');
      st.textContent='COMPANION - '+mood;
      dot.style.background=mood=='ALERT'?'var(--red)':mood=='FOCUS'?'var(--yellow)':'var(--claude)';
      document.getElementById('mode').textContent='companion face';
    }else{
      document.getElementById('title').textContent='Clawdmeter';
      document.getElementById('subtitle').textContent='Claude usage monitor';
      document.getElementById('switchMode').textContent='Mac';
      document.getElementById('switchMode').href='?view=mac';
      document.getElementById('sessionName').textContent='Session window';
      document.getElementById('weeklyName').textContent='Weekly window';
      set('sp','sf',d.s); set('wp','wf',d.w);
      document.getElementById('stok').textContent=commas(d.st);
      document.getElementById('wtok').textContent=commas(d.wt);
      document.getElementById('stat3Name').textContent='Session tokens';
      document.getElementById('stat4Name').textContent='Weekly tokens';
      document.getElementById('sessionCard').className='panel meter'+(d.bind==1?' bind':'');
      document.getElementById('weeklyCard').className='panel meter'+(d.bind==2?' bind':'');
      document.getElementById('st').textContent =
        d.sr ? 'Reset '+utcHHMM(d.sr) : 'Reset --';
      document.getElementById('sc').textContent = d.sr&&now ? countdown(d.sr-now) : '--';
      document.getElementById('wt').textContent =
        d.wr ? 'Reset '+DOW[lt(d.wr).getUTCDay()]+' '+utcHHMM(d.wr) : 'Reset --';
      if(d.s<0){st.textContent='waiting for daemon';dot.style.background='var(--muted)';}
      else{
        var label=(d.stat||'local').toUpperCase();
        st.textContent=label+' - updated '+ageText(d.age)+' ago';
        dot.style.background=d.stat&&d.stat!='allowed'?'var(--red)':live?'var(--green)':'var(--yellow)';
      }
      document.getElementById('mode').textContent=d.stat?'api headers':'local or waiting';
    }
  }catch(e){st.textContent='device unreachable';dot.style.background='var(--red)';}
  ticking=false;
}
tick();setInterval(tick,3000);
</script></body></html>
)HTML";

// Gzipped form of INDEX_HTML above, served by handleRoot(). Regenerate with
// firmware/tools/gen_index_gz.py whenever INDEX_HTML changes.
#include "index_html_gz.h"

const char UPDATE_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Clawdmeter OTA</title>
<style>
  :root{color-scheme:dark;--bg:#0d1117;--panel:#161b22;--line:#30363d;--text:#f0f6fc;--muted:#8b949e;--blue:#58a6ff}
  *{box-sizing:border-box}
  body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,sans-serif;background:var(--bg);color:var(--text);margin:0;min-height:100vh;display:grid;place-items:center;padding:20px}
  main{width:min(520px,100%);background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:18px;display:grid;gap:14px}
  h1{font-size:22px;margin:0}
  p{color:var(--muted);font-size:14px;line-height:1.45;margin:0}
  form{display:grid;gap:12px}
  input,button{font:inherit}
  input[type=file]{border:1px solid var(--line);border-radius:8px;padding:10px;width:100%}
  button{color:var(--text);background:#10151d;border:1px solid var(--line);border-radius:8px;padding:10px 12px;cursor:pointer}
  button:hover{border-color:var(--blue)}
  code{color:var(--text)}
</style></head><body><main>
<h1>Firmware Update</h1>
<p>Upload only <code>clawdmeter_esp8266.ino.bin</code>. Close dashboard tabs and stop the daemon while updating.</p>
<form method="POST" action="/update" enctype="multipart/form-data">
  <input type="file" name="firmware" accept=".bin,.bin.gz" required>
  <button type="submit">Update Firmware</button>
</form>
<p>The device will reboot after a successful upload. If the browser disconnects during reboot, wait 20 seconds and reopen the dashboard.</p>
</main></body></html>
)HTML";

void handleRoot() {
  // Serve the dashboard gzipped (~15 KB -> ~4.4 KB, see index_html_gz.h). The
  // uncompressed page took multiple TCP segments and a single blocking send_P
  // stalled mid-write ~25% of the time when the WiFi link couldn't drain the
  // send buffer fast enough — Chrome then showed a failed/partial load. The
  // gzipped body fits in one send-buffer fill so the write completes in one go.
  // Keep the radio awake for the brief send as cheap insurance against a lossy
  // link; it's invisible (unlike parking the backlight PWM, which flashed the
  // screen). INDEX_HTML stays the editable source; regenerate the header with
  // firmware/tools/gen_index_gz.py after editing it.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Connection", "close");
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html", (PGM_P)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  // Drain the TX buffer before "Connection: close" tears the socket down. On a
  // lossy link the final segment was racing the close and arriving truncated at
  // the client (page came through as 2920/4424); flush() blocks until the
  // outgoing data is actually sent.
  server.client().flush();
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);
}

// Daemon pushes the numbers here:
// POST /usage?s=&w=&st=&wt=&sr=&wr=&stat=&bind=&t=&cpu=&mem=&disk=&bat=
void handleUsage() {
  if (server.hasArg("s")) sessionPct = server.arg("s").toInt();
  if (server.hasArg("w")) weeklyPct  = server.arg("w").toInt();
  if (server.hasArg("st")) sessionTokens = strtoul(server.arg("st").c_str(), NULL, 10);
  if (server.hasArg("wt")) weeklyTokens = strtoul(server.arg("wt").c_str(), NULL, 10);
  if (server.hasArg("sr")) sessReset = strtoul(server.arg("sr").c_str(), NULL, 10);
  if (server.hasArg("wr")) weekReset = strtoul(server.arg("wr").c_str(), NULL, 10);
  if (server.hasArg("stat")) unifiedStatus = server.arg("stat");
  if (server.hasArg("bind")) bindingLimit = server.arg("bind").toInt();
  if (server.hasArg("cpu")) macCpuPct = server.arg("cpu").toInt();
  if (server.hasArg("mem")) macMemPct = server.arg("mem").toInt();
  if (server.hasArg("disk")) macDiskPct = server.arg("disk").toInt();
  if (server.hasArg("bat")) macBatteryPct = server.arg("bat").toInt();
  if (server.hasArg("t")) {
    unsigned long t = strtoul(server.arg("t").c_str(), NULL, 10);
    if (t > 0) { timeBaseEpoch = t; timeBaseMillis = millis(); }
  }
  lastUpdateMs = millis();
  lastTickEpoch = nowEpoch();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok");
  meterRedrawPending = true;
}

// The dashboard page polls this.
static String screenName() {
  if (lcdScreen == SCREEN_DESK) return "desk";
  if (lcdScreen == SCREEN_MAC) return "mac";
  if (lcdScreen == SCREEN_FACE) return "face";
  return "claude";
}

static const char *faceModeName() {
  if (faceMode == FACE_MODE_WORKING) return "working";
  if (faceMode == FACE_MODE_SLEEP) return "sleep";
  if (faceMode == FACE_MODE_MONK) return "monk";
  return "idle";
}

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
             ",\"cpu\":" + String(macCpuPct) +
             ",\"mem\":" + String(macMemPct) +
             ",\"disk\":" + String(macDiskPct) +
             ",\"bat\":" + String(macBatteryPct) +
             ",\"screen\":\"" + screenName() + "\"" +
             ",\"face\":\"" + String(faceModeName()) + "\"" +
             ",\"desk\":\"" + deskStatus + "\"" +
             ",\"deskText\":\"" + deskText + "\"" +
             ",\"deskColor\":\"" + deskColorName + "\"" +
             ",\"bl\":" + String(lcdBrightness) +
             ",\"now\":" + String(nowEpoch()) +
             ",\"rst\":\"" + bootReason + "\"" +
             ",\"up\":" + String(millis() / 1000) +
             ",\"age\":" + String(age) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", j);
}

void handleBrightness() {
  if (server.hasArg("value")) {
    String raw = server.arg("value");
    bool valid = raw.length() > 0;
    for (unsigned int i = 0; i < raw.length(); i++) {
      if (!isDigit(raw[i])) valid = false;
    }
    if (valid) {
      int value = raw.toInt();
      value = constrain(value, 0, 255);
      applyBrightness((uint8_t)value, true);
    }
  }
  String j = "{\"brightness\":" + String(lcdBrightness) + "}";
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", j);
}

void handleMode() {
  if (server.hasArg("screen")) {
    String screen = server.arg("screen");
    screen.toLowerCase();
    uint8_t nextScreen = lcdScreen;
    if (screen == "mac") nextScreen = SCREEN_MAC;
    else if (screen == "desk") nextScreen = SCREEN_DESK;
    else if (screen == "face") nextScreen = SCREEN_FACE;
    else if (screen == "claude") nextScreen = SCREEN_CLAUDE;
    if (nextScreen != lcdScreen) {
      lcdScreen = nextScreen;
      if (lcdScreen == SCREEN_MAC) macChromeReady = false;
      if (lcdScreen == SCREEN_CLAUDE) claudeChromeReady = false;
      lastTickEpoch = 0;
      drawMeter();
    }
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", screenName());
}

static String sanitizedDeskText(String text) {
  text.trim();
  String out = "";
  for (unsigned int i = 0; i < text.length() && out.length() < 12; i++) {
    char ch = text[i];
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-' || ch == '_') {
      out += ch;
    }
  }
  if (out.length() == 0) out = "STATUS";
  out.toUpperCase();
  return out;
}

static bool validDeskColor(const String &color) {
  return color == "green" || color == "red" || color == "amber" ||
         color == "blue" || color == "white" || color == "claude";
}

void handleDesk() {
  bool changed = false;
  if (server.hasArg("status")) {
    String status = server.arg("status");
    status.toLowerCase();
    if (status == "coding" || status == "meeting" || status == "busy" ||
        status == "break" || status == "claude") {
      deskStatus = status;
      if (status == "busy") {
        deskText = "BUSY";
        deskColorName = "red";
      } else if (status == "meeting") {
        deskText = "MEETING";
        deskColorName = "blue";
      } else if (status == "break") {
        deskText = "BREAK";
        deskColorName = "amber";
      } else if (status == "claude") {
        deskText = "CLAUDE";
        deskColorName = "claude";
      } else {
        deskText = "CODING";
        deskColorName = "green";
      }
      lcdScreen = SCREEN_DESK;
      changed = true;
    }
  }
  if (server.hasArg("text")) {
    deskText = sanitizedDeskText(server.arg("text"));
    deskStatus = "custom";
    lcdScreen = SCREEN_DESK;
    changed = true;
  }
  if (server.hasArg("color")) {
    String color = server.arg("color");
    color.toLowerCase();
    if (validDeskColor(color)) {
      deskColorName = color;
      deskStatus = "custom";
      lcdScreen = SCREEN_DESK;
      changed = true;
    }
  }
  if (changed) drawMeter();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", deskStatus);
}

static void setOtaError() {
  StreamString message;
  Update.printError(message);
  otaError = message.c_str();
  Serial.print("OTA error: ");
  Serial.println(otaError);
}

void handleUpdatePage() {
  server.sendHeader("Connection", "close");
  server.send_P(200, "text/html", UPDATE_HTML);
}

void handleRestart() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "Restarting Clawdmeter...");
  delay(500);
  ESP.restart();
}

void handleFactoryReset() {
  WiFiManager wm;
  wm.resetSettings();
  EEPROM.write(EEPROM_MARKER_ADDR, 0x00);
  EEPROM.write(EEPROM_BRIGHTNESS_ADDR, LCD_BRIGHTNESS);
  EEPROM.commit();

  server.sendHeader("Connection", "close");
  server.send(200, "text/plain",
              "Factory reset OK. Rebooting to Clawdmeter-setup...");
  delay(500);
  ESP.restart();
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
#define C_BLUE   0x041F
#define C_CYAN   0x07FF
#define C_AMBER  0xFD20
#define C_CLAUDE 0xDBAA   // warm Claude-style orange accent (#D97757-ish)
#define C_CLAY   0xCBED   // muted clay (#CD7F6A) — the pixel-creature body color
// claude.ai brand palette for the SCREEN_CLAUDE redesign (warm paper aesthetic).
#define C_CREAM  0xF7BD   // ivory paper background (#F5F4EE)
#define C_INK    0x18E3   // warm near-black text / headline number (#1F1E1D-ish)
#define C_TAN    0xEF5C   // bar track / soft fills (#EDE9E0)
#define C_MUTE   0x8C92   // warm taupe for secondary labels on cream (#8A8578-ish)

// Display clock/reset times in Thailand time. Pushed epochs are UTC; add the
// offset only when formatting wall-clock text (durations/countdowns stay raw).
#define TZ_OFFSET 25200UL   // Asia/Bangkok, UTC+7 (no DST)

static const char *const DOW[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint16_t barColor(int p) {
  if (p >= 90) return C_RED;
  if (p >= 60) return C_YELLOW;
  return C_GREEN;
}

static uint16_t statusColor() {
  if (unifiedStatus.length() == 0) return C_GRAY;
  if (unifiedStatus == "allowed") return C_GREEN;
  if (sessionPct >= 100 || weeklyPct >= 100) return C_CLAUDE;
  return C_RED;                         // rejected / blocked / queued ...
}

static void drawClaudeStatusDot(bool force) {
  if (!unifiedStatus.length()) return;

  const unsigned long cycleMs = 5000UL;
  const unsigned long pulseMs = 700UL;
  unsigned long elapsed = millis() % cycleMs;
  uint8_t phase = elapsed < pulseMs ? (elapsed / 175UL) + 1 : 0;
  if (!force && phase == claudeStatusPulsePhase) return;
  claudeStatusPulsePhase = phase;

  uint16_t c = statusColor();
  gfx->fillRect(104, 7, 17, 18, C_CREAM);
  if (phase) {
    int r = 4 + phase;
    gfx->drawCircle(112, 16, r, c);
    if (phase < 3) gfx->drawCircle(112, 16, r + 1, c);
  }
  gfx->fillCircle(112, 16, 4, c);
}

static int textWidth(const String &s, uint8_t size) {
  return (int)s.length() * 6 * size;
}

static void printCentered(int y, uint8_t size, const String &s,
                          uint16_t fg, uint16_t bg) {
  gfx->setTextSize(size);
  gfx->setTextColor(fg, bg);
  gfx->setCursor((240 - textWidth(s, size)) / 2, y);
  gfx->print(s);
}

static String pctText(int pct) {
  return (pct < 0) ? String("--") : String(pct) + "%";
}

static void drawProgressBar(int x, int y, int w, int h, int pct) {
  gfx->fillRect(x, y, w, h, C_BLACK);
  gfx->drawRect(x, y, w, h, C_LINE);
  if (pct > 0) {
    int p = pct > 100 ? 100 : pct;
    gfx->fillRect(x + 2, y + 2, (w - 4) * p / 100, h - 4, barColor(pct));
  }
}

// Rounded "pill" usage bar: tan track with a coral fill (red in the danger zone).
// Repaints the whole track each call so a shrinking % leaves no leftover fill.
static void drawClaudeBar(int x, int y, int w, int h, int pct) {
  int rad = h / 2;
  gfx->fillRoundRect(x, y, w, h, rad, C_TAN);     // track (also clears prior fill)
  gfx->drawRoundRect(x, y, w, h, rad, C_MUTE);    // soft rim for definition
  if (pct > 0) {
    int p = pct > 100 ? 100 : pct;
    int fw = (w * p) / 100;
    if (fw < h) fw = h;                            // keep the pill renderable at low %
    uint16_t c = (pct >= 85) ? C_RED : C_CLAUDE;
    gfx->fillRoundRect(x, y, fw, h, rad, c);
  }
}

// The Claude "burst" mark: eight coral spokes (long cardinals, shorter diagonals)
// radiating from a small filled hub. Cardinals are double-struck for weight.
static void drawClaudeIcon(int cx, int cy, uint16_t c) {
  int r = 9;
  int d = (r * 7) / 10;                            // diagonal reach
  gfx->drawLine(cx - r, cy, cx + r, cy, c);
  gfx->drawLine(cx, cy - r, cx, cy + r, c);
  gfx->drawLine(cx - r, cy + 1, cx + r, cy + 1, c);
  gfx->drawLine(cx + 1, cy - r, cx + 1, cy + r, c);
  gfx->drawLine(cx - d, cy - d, cx + d, cy + d, c);
  gfx->drawLine(cx - d, cy + d, cx + d, cy - d, c);
  gfx->fillCircle(cx, cy, 2, c);
}

static void drawOtaStatus(const String &line) {
  gfx->fillScreen(C_BLACK);
  gfx->fillRect(0, 0, 240, 34, C_BLUE);
  gfx->setTextColor(C_BLACK, C_BLUE);
  gfx->setTextSize(2);
  gfx->setCursor(14, 10);
  gfx->print("OTA UPDATE");

  gfx->setTextColor(C_WHITE, C_BLACK);
  gfx->setTextSize(2);
  gfx->setCursor(16, 84);
  gfx->print("firmware");
  gfx->setTextColor(C_GRAY, C_BLACK);
  gfx->setTextSize(1);
  gfx->setCursor(16, 116);
  gfx->print(line);
  String ip = "IP " + WiFi.localIP().toString();
  gfx->setCursor(236 - textWidth(ip, 1), 230);
  gfx->print(ip);
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

// Compact IP readout tucked into the bottom-right corner (size-1 gray text).
// Callers fillScreen(C_BLACK) before this, so no background fill is needed.
static void drawIpPanel() {
  gfx->fillRect(96, 224, 136, 9, C_CREAM);
  printRight(232, 224, 1, "IP " + WiFi.localIP().toString(), C_MUTE, C_CREAM);
}

static void drawMetricRow(int y, const char *label, int pct, int barX, int barW) {
  gfx->setTextSize(1);
  gfx->setTextColor(C_GRAY, C_BLACK);
  gfx->setCursor(12, y);
  gfx->print(label);
  gfx->setTextColor(C_WHITE, C_BLACK);
  printRight(226, y, 1, pctText(pct), C_WHITE, C_BLACK);
  drawProgressBar(barX, y + 13, barW, 12, pct);
}

static void drawMacClock() {
  unsigned long e = nowEpoch();
  String t = e ? hhmm(e + TZ_OFFSET) : String("--:--");
  gfx->fillRect(184, 2, 52, 10, C_WHITE);
  printRight(236, 2, 1, t, C_BLACK, C_WHITE);
}

static String uptimePlainText() {
  unsigned long totalHours = millis() / 3600000UL;
  unsigned long days = totalHours / 24UL;
  unsigned long hours = totalHours % 24UL;
  return String(days) + "d " + pad2(hours) + "h";
}

static void drawMacApple(int x, int y) {
  gfx->fillCircle(x + 4, y + 6, 3, C_BLACK);
  gfx->fillCircle(x + 8, y + 6, 3, C_BLACK);
  gfx->fillCircle(x + 6, y + 9, 4, C_BLACK);
  gfx->fillCircle(x + 10, y + 5, 2, C_WHITE);
  gfx->drawLine(x + 6, y + 1, x + 9, y, C_BLACK);
}

static void drawCompactMacIcon(int x, int y) {
  gfx->drawRect(x, y, 30, 34, C_BLACK);
  gfx->drawRect(x + 4, y + 5, 22, 16, C_BLACK);
  gfx->fillRect(x + 7, y + 8, 16, 10, C_BLACK);
  gfx->drawFastHLine(x + 7, y + 25, 15, C_BLACK);
  gfx->fillRect(x + 5, y + 32, 5, 2, C_BLACK);
  gfx->fillRect(x + 20, y + 32, 5, 2, C_BLACK);
}

static void drawMacRowChrome(int y, const char *label) {
  gfx->setTextSize(1);
  gfx->setTextColor(C_BLACK, C_WHITE);
  gfx->setCursor(18, y + 4);
  gfx->print(label);
  gfx->drawRect(76, y, 96, 14, C_BLACK);
}

static bool macMetricDanger(int pct, bool battery) {
  if (pct < 0) return false;
  return battery ? (pct <= 20) : (pct >= 85);
}

static void drawMacRowDynamic(int y, int pct, bool battery) {
  const int barX = 76;
  const int barY = y;
  const int barW = 96;
  const int barH = 14;
  gfx->fillRect(barX + 2, barY + 2, barW - 4, barH - 4, C_WHITE);
  if (pct > 0) {
    int p = pct > 100 ? 100 : pct;
    uint16_t fill = macMetricDanger(pct, battery) ? C_RED : C_BLACK;
    gfx->fillRect(barX + 2, barY + 2, (barW - 4) * p / 100, barH - 4, fill);
  }

  gfx->fillRect(178, y - 1, 50, 18, C_WHITE);
  printRight(226, y, 2, pctText(pct), C_BLACK, C_WHITE);
}

static void drawMacStaleMarker() {
  bool stale = (lastUpdateMs != 0) && (millis() - lastUpdateMs > 120000UL);
  gfx->fillRect(220, 20, 9, 9, C_WHITE);
  if (!stale) return;
  gfx->drawRect(220, 20, 9, 9, C_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(C_BLACK, C_WHITE);
  gfx->setCursor(223, 21);
  gfx->print("!");
}

static void drawMacChrome() {
  gfx->fillRect(0, 0, 240, 15, C_WHITE);
  drawMacApple(4, 2);
  gfx->setTextSize(1);
  gfx->setTextColor(C_BLACK, C_WHITE);
  gfx->setCursor(20, 3);
  gfx->print("Finder");
  gfx->drawFastHLine(0, 14, 240, C_BLACK);

  gfx->drawFastVLine(234, 18, 221, C_BLACK);
  gfx->drawFastHLine(8, 238, 227, C_BLACK);
  gfx->fillRect(6, 16, 228, 222, C_WHITE);
  gfx->drawRect(6, 16, 228, 222, C_BLACK);

  for (int y = 19; y <= 31; y += 2) gfx->drawFastHLine(8, y, 224, C_BLACK);
  gfx->fillRect(91, 18, 120, 14, C_WHITE);
  gfx->drawRect(13, 20, 9, 9, C_BLACK);
  printCentered(21, 1, String("About This Macintosh"), C_BLACK, C_WHITE);
  gfx->drawFastHLine(6, 34, 228, C_BLACK);

  drawCompactMacIcon(24, 48);
  gfx->setTextColor(C_BLACK, C_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(68, 52);
  gfx->print("System Software 7.1");
  gfx->setCursor(68, 66);
  gfx->print("Clawdmeter");
  gfx->drawFastHLine(16, 96, 208, C_BLACK);

  drawMacRowChrome(116, "CPU");
  drawMacRowChrome(140, "Memory");
  drawMacRowChrome(164, "Disk");
  drawMacRowChrome(188, "Battery");
}

static void drawMacDynamic() {
  drawMacClock();

  gfx->fillRect(68, 66, 132, 10, C_WHITE);
  gfx->setTextSize(1);
  gfx->setTextColor(C_BLACK, C_WHITE);
  gfx->setCursor(68, 66);
  gfx->print("Clawdmeter up " + uptimePlainText());

  drawMacStaleMarker();
  drawMacRowDynamic(116, macCpuPct, false);
  drawMacRowDynamic(140, macMemPct, false);
  drawMacRowDynamic(164, macDiskPct, false);
  drawMacRowDynamic(188, macBatteryPct, true);
}

// Typewriter frame for a desk label: types it in one char at a time, blinks a
// cursor, holds the full word, then repeats. Applies to every desk word, not
// just CODING. The frame is reset by drawDeskSign() restarting deskTypingStartMs.
static String deskDisplayText(const String &label) {
  unsigned long len = label.length();
  if (len == 0) return label;

  const unsigned long typeMs = 800UL;     // per-character reveal
  const unsigned long holdMs = 30000UL;   // hold the full word before repeating
  const unsigned long cycleMs = len * typeMs + holdMs;
  unsigned long elapsed = (millis() - deskTypingStartMs) % cycleMs;
  int chars = elapsed < len * typeMs ? (int)(elapsed / typeMs) + 1 : (int)len;
  bool cursorOn = ((millis() / 500UL) % 2UL) == 0;
  return label.substring(0, chars) + (cursorOn ? "|" : " ");
}

// The MacPaint window "canvas": the white drawing area the typed text lives in.
// (x..x+w, y..y+h) = 39..238 wide, 29..199 tall. drawDeskSign fills it white once.
#define DESK_CANVAS_X 39
#define DESK_CANVAS_Y 29
#define DESK_CANVAS_W 200
#define DESK_CANVAS_H 171

static void drawDeskStatusText(const String &label) {
  String shown = deskDisplayText(label);
  if (shown == deskLastShown) return;      // unchanged frame — skip the redraw
  deskLastShown = shown;

  // Size from the full label (+cursor) so it stays constant through the type-in,
  // and center on the canvas at the final width so letters land in place instead
  // of re-centering — and jittering — on every frame. Drop a size if the word
  // won't fit the canvas (12 chars only fit at size 2).
  int full = (int)label.length() + 1;
  uint8_t textSize = (full * 24 <= DESK_CANVAS_W - 8) ? 4
                   : (full * 18 <= DESK_CANVAS_W - 8) ? 3 : 2;
  int x = DESK_CANVAS_X + (DESK_CANVAS_W - textWidth(label + "|", textSize)) / 2;
  if (x < DESK_CANVAS_X + 1) x = DESK_CANVAS_X + 1;
  int y = DESK_CANVAS_Y + (DESK_CANVAS_H - 8 * textSize) / 2;

  // Pad to a constant-width field and print with an OPAQUE white background
  // instead of wiping the canvas first. Already-typed glyphs get overwritten
  // with the same pixels (no visible flash), trailing/erased cells are cleared
  // by the space glyphs' white background, so only the changed cell flips.
  String field = shown;
  while (field.length() < label.length() + 1) field += " ";
  gfx->setTextSize(textSize);
  gfx->setTextColor(C_BLACK, C_WHITE);     // black ink on the white canvas
  gfx->setCursor(x, y);
  gfx->print(field);
}

static void drawDeskAnimatedStatus() {
  // MacPaint is black ink on a white canvas — presets and custom text alike.
  // (Preset/custom color still drives the dashboard dot via deskColorName.)
  String label = deskText;
  label.toUpperCase();
  if (label.length() > 12) label = label.substring(0, 12);
  drawDeskStatusText(label);
}

// Static chrome for the cream "claude.ai" card. Drawn once on switch-in (gated by
// claudeChromeReady); the per-push dynamic pass repaints only the values in-place,
// so there is no fillScreen flash every 60 s (same discipline as the MAC screen).
static void drawClaudeChrome() {
  // Soft rounded card edge (2 px) on the cream field.
  gfx->drawRoundRect(2, 2, 236, 236, 12, C_MUTE);
  gfx->drawRoundRect(3, 3, 234, 234, 11, C_MUTE);

  // Header: Claude burst mark + wordmark, then a hairline divider rule.
  drawClaudeIcon(16, 16, C_CLAUDE);
  gfx->setTextSize(2);
  gfx->setTextColor(C_INK, C_CREAM);
  gfx->setCursor(32, 9);
  gfx->print("Claude");
  gfx->drawFastHLine(12, 33, 216, C_TAN);

  // Static block labels (the values themselves are painted by the dynamic pass).
  gfx->setTextSize(1);
  gfx->setTextColor(C_MUTE, C_CREAM);
  gfx->setCursor(20, 46);
  gfx->print("Session 5h");

  gfx->setTextSize(2);
  gfx->setTextColor(C_INK, C_CREAM);
  gfx->setCursor(20, 158);
  gfx->print("Weekly");
}

// Dynamic session block: binding accent, headline %, pill bar, reset + countdown.
static void drawClaudeHero() {
  // Binding-limit accent: a coral tick beside the active block's label.
  gfx->fillRect(12, 45, 3, 9, bindingLimit == 1 ? C_CLAUDE : C_CREAM);

  // Headline percentage in dark ink (coral is reserved for accents); red in danger.
  uint16_t bigC = (sessionPct >= 85) ? C_RED : C_INK;
  gfx->fillRect(8, 60, 224, 38, C_CREAM);              // clear band (width changes)
  printCentered(63, 5, pctText(sessionPct), bigC, C_CREAM);

  drawClaudeBar(16, 110, 208, 16, sessionPct);

  // Reset time (fixed width) + live countdown (variable width → cleared).
  gfx->setTextSize(1);
  gfx->setTextColor(C_MUTE, C_CREAM);
  gfx->setCursor(20, 136);
  if (sessReset == 0) gfx->print("reset --:--");
  else gfx->print("reset " + hhmm(sessReset + TZ_OFFSET));

  String cd = (sessReset && nowEpoch()) ? countdown((long)sessReset - (long)nowEpoch()) : "T--";
  gfx->fillRect(176, 136, 48, 8, C_CREAM);
  printRight(224, 136, 1, cd, C_INK, C_CREAM);
}

// Dynamic weekly block: binding accent, %, pill bar, reset day/time.
static void drawClaudeWeekly() {
  gfx->fillRect(12, 160, 3, 11, bindingLimit == 2 ? C_CLAUDE : C_CREAM);

  uint16_t wC = (weeklyPct >= 85) ? C_RED : C_INK;
  gfx->fillRect(150, 158, 74, 14, C_CREAM);           // clear region (width changes)
  printRight(224, 158, 2, pctText(weeklyPct), wC, C_CREAM);

  drawClaudeBar(16, 182, 208, 12, weeklyPct);

  gfx->fillRect(20, 202, 176, 8, C_CREAM);
  gfx->setTextSize(1);
  gfx->setTextColor(C_MUTE, C_CREAM);
  gfx->setCursor(20, 202);
  if (weekReset == 0) gfx->print("reset --");
  else gfx->print("reset " + dowName(weekReset + TZ_OFFSET) + " " + hhmm(weekReset + TZ_OFFSET));
}

// Redraw only the once-per-second fields (clock + session countdown) so the
// rest of the screen doesn't flicker on every tick.
void tickDynamic() {
  unsigned long e = nowEpoch();
  if (e == 0) return;

  if (lcdScreen == SCREEN_DESK) {
    drawDeskAnimatedStatus();   // no clock on the MacPaint canvas; typewriter only
    return;
  }

  if (lcdScreen == SCREEN_MAC) {
    drawMacDynamic();
    return;
  }

  // SCREEN_CLAUDE: cream header clock + session countdown, opaque cream-bg prints.
  gfx->fillRect(184, 12, 48, 8, C_CREAM);
  printRight(232, 12, 1, hhmmss(e + TZ_OFFSET), C_INK, C_CREAM);

  if (sessReset) {
    gfx->fillRect(176, 136, 48, 8, C_CREAM);
    printRight(224, 136, 1, countdown((long)sessReset - (long)e), C_INK, C_CREAM);
  }
}

void drawMacMeter() {
  if (!macChromeReady) {
    gfx->fillScreen(C_WHITE);
    drawMacChrome();
    macChromeReady = true;
  }
  drawMacDynamic();
}

// One swatch of the MacPaint pattern palette: a small black-on-white fill drawn
// pixel by pixel. Static (draw-once), so the per-pixel cost doesn't matter.
static void drawDeskPattern(int x, int y, int w, int h, int type) {
  switch (type & 7) {
    case 0: break;                                   // white / empty
    case 1: gfx->fillRect(x, y, w, h, C_BLACK); break;  // solid
    default:
      for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
          bool on = false;
          switch (type & 7) {
            case 2: on = (xx + yy) & 1; break;       // checker
            case 3: on = (yy & 1) == 0; break;       // horizontal lines
            case 4: on = (xx & 1) == 0; break;       // vertical lines
            case 5: on = (xx % 3 == 0) && (yy % 3 == 0); break;  // sparse dots
            case 6: on = (xx + yy) % 3 == 0; break;  // diagonal
            case 7: on = (xx & 1) == 0 && (yy & 1) == 0; break;  // grid
          }
          if (on) gfx->drawPixel(x + xx, y + yy, C_BLACK);
        }
  }
}

// A tiny simplified MacPaint tool glyph centered in its cell.
static void drawDeskToolGlyph(int cx, int cy, int i) {
  switch (i) {
    case 0:  gfx->drawRect(cx - 5, cy - 4, 10, 8, C_BLACK); break;          // marquee
    case 1:  gfx->drawCircle(cx - 1, cy - 1, 4, C_BLACK);
             gfx->drawLine(cx - 1, cy + 3, cx + 4, cy + 4, C_BLACK); break; // lasso
    case 2:  gfx->fillRect(cx - 4, cy - 3, 8, 6, C_BLACK); break;           // hand/select
    case 3:  gfx->setTextSize(1); gfx->setTextColor(C_BLACK, C_WHITE);
             gfx->setCursor(cx - 3, cy - 3); gfx->print('A'); break;        // text
    case 4:  gfx->fillTriangle(cx - 4, cy - 3, cx + 4, cy - 3, cx, cy + 4, C_BLACK); break; // bucket
    case 5:  for (int d = 0; d < 7; d++)
               gfx->drawPixel(cx - 3 + (d * 5 % 8), cy - 3 + (d * 3 % 7), C_BLACK); break;  // spray
    case 6:  gfx->drawLine(cx - 4, cy + 4, cx + 3, cy - 3, C_BLACK);
             gfx->drawLine(cx - 3, cy + 4, cx + 4, cy - 3, C_BLACK); break; // brush
    case 7:  gfx->drawLine(cx - 4, cy + 4, cx + 4, cy - 4, C_BLACK);
             gfx->fillRect(cx + 3, cy - 4, 2, 2, C_BLACK); break;           // pencil
    case 8:  gfx->drawLine(cx - 5, cy + 4, cx + 5, cy - 4, C_BLACK); break; // line
    case 9:  gfx->drawRect(cx - 5, cy - 3, 10, 7, C_BLACK); break;          // eraser
    case 10: gfx->drawRect(cx - 5, cy - 4, 10, 8, C_BLACK); break;          // rect
    case 11: gfx->drawRoundRect(cx - 5, cy - 4, 10, 8, 3, C_BLACK); break;  // round rect
    case 12: gfx->drawCircle(cx, cy, 4, C_BLACK); break;                    // oval
    case 13: gfx->drawLine(cx - 5, cy + 2, cx - 2, cy - 3, C_BLACK);
             gfx->drawLine(cx - 2, cy - 3, cx + 1, cy + 2, C_BLACK);
             gfx->drawLine(cx + 1, cy + 2, cx + 4, cy - 3, C_BLACK); break; // freeform
    case 14: gfx->fillRect(cx - 5, cy - 4, 10, 8, C_BLACK); break;          // filled rect
    default: gfx->fillCircle(cx, cy, 4, C_BLACK); break;                    // filled oval
  }
}

// The left MacPaint tool palette: a white 2x8 grid of tool glyphs.
static void drawDeskToolPalette() {
  const int px = 0, py = 15, pw = 36, ph = 186;
  const int cols = 2, rows = 8, cw = pw / cols, ch = ph / rows;
  gfx->fillRect(px, py, pw, ph, C_WHITE);
  gfx->drawRect(px, py, pw, ph, C_BLACK);
  for (int i = 1; i < cols; i++) gfx->drawFastVLine(px + i * cw, py, ph, C_BLACK);
  for (int j = 1; j < rows; j++) gfx->drawFastHLine(px, py + j * ch, pw, C_BLACK);
  for (int r = 0; r < rows; r++)
    for (int col = 0; col < cols; col++)
      drawDeskToolGlyph(px + col * cw + cw / 2, py + r * ch + ch / 2, r * cols + col);
}

// The bottom MacPaint palette: line-width box on the left, pattern swatches right.
static void drawDeskPatternStrip() {
  const int sy = 202, sh = 38;
  gfx->fillRect(0, sy, 240, sh, C_WHITE);
  gfx->drawRect(0, sy, 240, sh, C_BLACK);

  // line-width selector box (four increasing-thickness bars)
  gfx->drawRect(2, sy + 3, 28, sh - 6, C_BLACK);
  for (int i = 0; i < 4; i++) gfx->fillRect(5, sy + 6 + i * 7, 22, i + 1, C_BLACK);

  // pattern swatches: 12 x 2 grid
  const int gx = 34, gy = sy + 3, pcols = 12, prows = 2;
  const int pcw = (240 - gx - 3) / pcols, pch = (sh - 6) / prows;
  for (int r = 0; r < prows; r++)
    for (int col = 0; col < pcols; col++) {
      int x = gx + col * pcw, y = gy + r * pch;
      gfx->drawRect(x, y, pcw, pch, C_BLACK);
      drawDeskPattern(x + 1, y + 1, pcw - 1, pch - 1, r * pcols + col);
    }
}

void drawDeskSign() {
  gfx->fillScreen(C_GRAY);        // classic grey desktop behind the window
  deskTypingStartMs = millis();
  deskAnimLastMs = 0;
  deskLastShown = "";             // force the first typing frame to paint

  // Menu bar: apple + the MacPaint menu titles.
  gfx->fillRect(0, 0, 240, 13, C_WHITE);
  gfx->fillCircle(7, 6, 3, C_BLACK);
  gfx->fillRect(8, 1, 2, 3, C_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(C_BLACK, C_WHITE);
  gfx->setCursor(14, 3);
  gfx->print("File Edit Goodies Font FontSize Style");
  gfx->drawFastHLine(0, 13, 240, C_BLACK);

  drawDeskToolPalette();

  // The "untitled" document window: outline + striped title bar + close box.
  const int wx = 38, wy = 15, ww = 202, wh = 186;
  gfx->drawRect(wx, wy, ww, wh, C_BLACK);
  gfx->fillRect(wx + 1, wy + 1, ww - 2, 12, C_WHITE);
  for (int s = wy + 3; s <= wy + 11; s += 2) gfx->drawFastHLine(wx + 2, s, ww - 4, C_BLACK);
  gfx->fillRect(wx + 4, wy + 3, 8, 8, C_WHITE);
  gfx->drawRect(wx + 4, wy + 3, 8, 8, C_BLACK);
  const int tw = 8 * 6;                       // "untitled" at size 1
  const int tx = wx + (ww - tw) / 2;
  gfx->fillRect(tx - 3, wy + 2, tw + 6, 10, C_WHITE);   // clear stripes behind title
  gfx->setTextColor(C_BLACK, C_WHITE);
  gfx->setCursor(tx, wy + 3);
  gfx->print("untitled");
  gfx->drawFastHLine(wx, wy + 13, ww, C_BLACK);

  // The white canvas the typed text is drawn into.
  gfx->fillRect(DESK_CANVAS_X, DESK_CANVAS_Y, DESK_CANVAS_W, DESK_CANVAS_H, C_WHITE);
  drawDeskAnimatedStatus();

  drawDeskPatternStrip();
}

// ---- Companion face: the Claude "pixel creature", animated frame by frame ------
// A 20x20 pixel-grid mascot, ported from the PixelEngine reference (its idle
// look-around preset). Each grid cell is one 12px square on the 240x240 panel
// (240 / 20 = 12). Cell values: 0 empty, 1 body (mood color), 2 eye (drawn as a
// dark hole punched in the body). A frame is the base creature optionally shifted
// by (dr,dc) and patched with a few cells; the preset plays the frames on a
// per-frame hold timer. Eyes lead, the head follows (glance left, right, up).
// Driven by the millis() poll in loop() (NOT a timer ISR), so it costs zero IRAM
// and never fights WiFi/TCP; only the cells that change between frames repaint.
#define F_N    20          // grid is 20x20
// Cell palette. 0..2 are the idle creature (empty / mood body / eye-hole). 3..9
// are the extra colors the "working" desk scene needs (headphones, laptop, desk).
// faceCellColor() maps each to a panel color; value 1 (body) tracks the mood.
static const uint8_t CELL_EMPTY = 0, CELL_BODY = 1, CELL_EYE = 2,
                     CELL_HP_LIGHT = 3, CELL_HP_SHADOW = 4, CELL_SCREEN = 5,
                     CELL_LBASE = 6, CELL_LOGO = 7, CELL_DESKTOP = 8,
                     CELL_DESKLEG = 9,
                     // The "monk" meditation scene: an orange Claude sunburst face
                     // (CELL_SPARK) over a static white stone robe (CELL_ROBE), with
                     // grey fold/edge shadows (CELL_ROBE_SHADE) that give it shape.
                     // CELL_ROBE_EDGE is the ink silhouette outline auto-generated
                     // around the white robe so it separates from the cream paper
                     // (the white robe is invisible on cream without it).
                     CELL_SPARK = 10, CELL_ROBE = 11, CELL_ROBE_SHADE = 12,
                     CELL_ROBE_EDGE = 13;
// RGB565 for the desk-scene cells (converted from the reference #hex palette).
#define C_HP_LIGHT  0xD6FC   // headphone light  #d4dde2
#define C_HP_SHADOW 0x8C93   // headphone shadow #8a9199
#define C_SCREEN    0x6B8F   // laptop screen    #6e7278
#define C_LBASE     0x39E8   // laptop base      #3a3c40
#define C_LOGO      0xBDF8   // laptop logo/cursor #b8bcc0
#define C_DESKTOP   0x2966   // desk top         #2a2c30
#define C_DESKLEG   0x18E4   // desk leg         #1c1e21

// On-screen placement. The creature is drawn in a CENTERED zone (not the whole
// panel) so a header strip, a divider rule and a footer data bar fit around it.
// Only the window rows FACE_R0..FACE_R1 / cols FACE_C0..FACE_C1 are ever painted
// — that is every cell the look-around animation can reach (head-up shifts to
// row 3, eye/head glances reach cols 2 and 18) — which keeps creature pixels out
// of the header/footer chrome on a force repaint. Origin places content col 10 /
// row 10 at panel centre (120) within the zone; 10px cells (vs the old 12) free
// the top/bottom bands for chrome.
#define FACE_CELL 10
#define FACE_OX   15       // x = FACE_OX + col*FACE_CELL  (content col 3..17 -> x45..195)
#define FACE_OY   4        // y = FACE_OY + row*FACE_CELL  (content row 4..16 -> y44..174)
#define FACE_R0   3
#define FACE_R1   17
#define FACE_C0   2
#define FACE_C1   18

// Base creature (the reference "idle" grid). PROGMEM -> lives in flash, not RAM.
static const uint8_t creatureBase[F_N][F_N] PROGMEM = {
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
  {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
  {0,0,0,0,0,1,1,2,1,1,1,1,1,2,1,1,0,0,0,0},
  {0,0,0,1,1,1,1,2,1,1,1,1,1,2,1,1,1,1,0,0},
  {0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
  {0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
  {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1,0,1,0,0},
  {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
  {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
  {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
  {0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0},
  {0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0},
  {0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

// Eye-move and head-follow patches (head-follow shifts the eyes one further in the
// same direction). Mirrors the reference's patch()/shift() frame builders exactly.
static const FacePatch P_EYES_L[] = {
  {6,7,CELL_BODY},{7,7,CELL_BODY},{6,13,CELL_BODY},{7,13,CELL_BODY},
  {6,6,CELL_EYE}, {7,6,CELL_EYE}, {6,12,CELL_EYE}, {7,12,CELL_EYE},
};
static const FacePatch P_HEAD_L[] = {
  {6,6,CELL_BODY},{7,6,CELL_BODY},{6,12,CELL_BODY},{7,12,CELL_BODY},
  {6,5,CELL_EYE}, {7,5,CELL_EYE}, {6,11,CELL_EYE}, {7,11,CELL_EYE},
};
static const FacePatch P_EYES_R[] = {
  {6,7,CELL_BODY},{7,7,CELL_BODY},{6,13,CELL_BODY},{7,13,CELL_BODY},
  {6,8,CELL_EYE}, {7,8,CELL_EYE}, {6,14,CELL_EYE}, {7,14,CELL_EYE},
};
static const FacePatch P_HEAD_R[] = {
  {6,8,CELL_BODY},{7,8,CELL_BODY},{6,14,CELL_BODY},{7,14,CELL_BODY},
  {6,9,CELL_EYE}, {7,9,CELL_EYE}, {6,15,CELL_EYE}, {7,15,CELL_EYE},
};
static const FacePatch P_EYES_UP[] = {
  {6,7,CELL_BODY},{7,7,CELL_BODY},{6,13,CELL_BODY},{7,13,CELL_BODY},
  {5,7,CELL_EYE}, {5,13,CELL_EYE},
};
static const FacePatch P_HEAD_UP[] = {
  {5,7,CELL_BODY},{6,7,CELL_BODY},{5,13,CELL_BODY},{6,13,CELL_BODY},
  {4,7,CELL_EYE}, {4,13,CELL_EYE},
};
static const FacePatch P_CURIOUS[] = { {5,6,CELL_BODY},{5,14,CELL_BODY} };

// The "idle look around" preset (values copied verbatim from the reference).
static const FaceFrame FACE_FRAMES[] = {
  {800,  0,  0, nullptr, 0},
  {200,  0,  0, FACE_PATCH(P_EYES_L)},   // glance left: eyes first...
  {500,  0, -1, FACE_PATCH(P_HEAD_L)},   // ...then head follows
  {300,  0,  0, FACE_PATCH(P_EYES_L)},
  {200,  0,  0, nullptr, 0},
  {400,  0,  0, FACE_PATCH(P_CURIOUS)},  // brief curious rest
  {400,  0,  0, nullptr, 0},
  {200,  0,  0, FACE_PATCH(P_EYES_R)},   // glance right
  {500,  0,  1, FACE_PATCH(P_HEAD_R)},
  {300,  0,  0, FACE_PATCH(P_EYES_R)},
  {200,  0,  0, nullptr, 0},
  {500,  0,  0, nullptr, 0},
  {200,  0,  0, FACE_PATCH(P_EYES_UP)},  // look up
  {400, -1,  0, FACE_PATCH(P_HEAD_UP)},
  {300,  0,  0, FACE_PATCH(P_EYES_UP)},
  {200,  0,  0, nullptr, 0},
  {700,  0,  0, nullptr, 0},             // settle
};
static const uint8_t FACE_NFRAMES = sizeof(FACE_FRAMES) / sizeof(FACE_FRAMES[0]);

// ---- "Working" scene: the same mascot, headphones on, coding at a desk --------
// A second base grid (the user's "work_coding" pixel art, compacted to the face
// animation window rows 3..16 / cols 2..17). Cells: body/eye as before, plus the
// headphone (HP_*), laptop (SCREEN/LBASE/LOGO) and desk (DESKTOP/DESKLEG) colors.
// Its frames are pure cell patches with NO base shift (dr=dc=0) so the desk stays
// put while only the hands / eyes / on-screen cursor move.
#define DB CELL_BODY
#define DE CELL_EYE
#define DH CELL_HP_LIGHT
#define DS CELL_HP_SHADOW
#define DC CELL_SCREEN
#define DL CELL_LBASE
#define DG CELL_LOGO
#define DT CELL_DESKTOP
#define DK CELL_DESKLEG
#define __ CELL_EMPTY
static const uint8_t deskBase[F_N][F_N] PROGMEM = {
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, // 0
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, // 1
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, // 2
  {__,__,__,__,__,__,__,DH,DH,DH,DH,DH,DH,__,__,__,__,__,__,__}, // 3 headband
  {__,__,__,__,__,DH,DS,DB,DB,DB,DB,DB,DB,DS,DH,__,__,__,__,__}, // 4 cups + head
  {__,__,__,__,__,DH,DS,DB,DE,DB,DB,DE,DB,DS,DH,__,__,__,__,__}, // 5 cups + eyes
  {__,__,__,__,__,DH,DS,DB,DB,DB,DB,DB,DB,DS,DH,__,__,__,__,__}, // 6 cups + head
  {__,__,__,__,__,__,__,DB,DB,DB,DB,DB,DB,__,__,__,__,__,__,__}, // 7 chin
  {__,__,__,__,__,DB,DB,DB,DB,DB,DB,DB,DB,DB,DB,__,__,__,__,__}, // 8 shoulders
  {__,__,__,__,DB,DB,DC,DC,DC,DC,DC,DC,DC,DC,DB,DB,__,__,__,__}, // 9 arms + screen
  {__,__,__,__,DB,DB,DC,DC,DC,DG,DG,DC,DC,DC,DB,DB,__,__,__,__}, //10 arms + logo
  {__,__,__,__,DB,DB,DC,DC,DC,DC,DC,DC,DC,DC,DB,DB,__,__,__,__}, //11 arms + screen
  {__,__,__,__,__,DL,DL,DL,DL,DL,DL,DL,DL,DL,DL,__,__,__,__,__}, //12 laptop base
  {__,__,DT,DT,DT,DT,DT,DT,DT,DT,DT,DT,DT,DT,DT,DT,DT,DT,__,__}, //13 desk top
  {__,__,__,DK,DK,__,__,__,__,__,__,__,__,__,__,DK,DK,__,__,__}, //14 desk legs
  {__,__,__,DK,DK,__,__,__,__,__,__,__,__,__,__,DK,DK,__,__,__}, //15 desk legs
  {__,__,__,DK,DK,__,__,__,__,__,__,__,__,__,__,DK,DK,__,__,__}, //16 desk legs
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, //17
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, //18
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, //19
};
#undef DB
#undef DE
#undef DH
#undef DS
#undef DC
#undef DL
#undef DG
#undef DT
#undef DK
#undef __

// Working-scene patches (applied with no base shift). Hands drop onto the laptop
// base to "type"; eyes blink/look up; a cursor blinks on the screen while thinking.
static const FacePatch W_TYPE_L[]   = { {12,6,CELL_BODY} };
static const FacePatch W_TYPE_R[]   = { {12,13,CELL_BODY} };
static const FacePatch W_TYPE_BOTH[]= { {12,6,CELL_BODY},{12,13,CELL_BODY} };
static const FacePatch W_BLINK[]    = { {5,8,CELL_BODY},{5,11,CELL_BODY} };
static const FacePatch W_THINK[]    = { {5,8,CELL_BODY},{5,11,CELL_BODY},
                                        {4,8,CELL_EYE}, {4,11,CELL_EYE} };
static const FacePatch W_THINK_CUR[]= { {5,8,CELL_BODY},{5,11,CELL_BODY},
                                        {4,8,CELL_EYE}, {4,11,CELL_EYE},
                                        {11,12,CELL_LOGO} };
static const FaceFrame WORK_FRAMES[] = {
  {220, 0, 0, FACE_PATCH(W_TYPE_L)},
  {220, 0, 0, FACE_PATCH(W_TYPE_R)},
  {180, 0, 0, FACE_PATCH(W_TYPE_BOTH)},
  {220, 0, 0, FACE_PATCH(W_TYPE_L)},
  {220, 0, 0, FACE_PATCH(W_TYPE_R)},
  {110, 0, 0, FACE_PATCH(W_BLINK)},
  {110, 0, 0, nullptr, 0},
  {550, 0, 0, FACE_PATCH(W_THINK)},      // pause to think...
  {350, 0, 0, FACE_PATCH(W_THINK_CUR)},  // ...cursor blinks on the screen
  {300, 0, 0, FACE_PATCH(W_THINK)},
  {350, 0, 0, FACE_PATCH(W_THINK_CUR)},
  {220, 0, 0, FACE_PATCH(W_TYPE_L)},     // back to typing
  {220, 0, 0, FACE_PATCH(W_TYPE_R)},
  {180, 0, 0, FACE_PATCH(W_TYPE_BOTH)},
};
static const uint8_t WORK_NFRAMES = sizeof(WORK_FRAMES) / sizeof(WORK_FRAMES[0]);

// ---- "Sleep" scene: closed eyes, slow breathing nod, and drifting Z particles --
// Same creature base as idle. Shifted frames close the eyes at the shifted rows,
// because faceBuildGrid applies the sparse patch after the base shift.
static const FacePatch S_SLEEP[] = {
  {6,7,CELL_BODY},{7,7,CELL_BODY},{6,13,CELL_BODY},{7,13,CELL_BODY},
};
static const FacePatch S_NOD_DOWN[] = {
  {7,7,CELL_BODY},{8,7,CELL_BODY},{7,13,CELL_BODY},{8,13,CELL_BODY},
};
static const FacePatch S_NOD_UP[] = {
  {5,7,CELL_BODY},{6,7,CELL_BODY},{5,13,CELL_BODY},{6,13,CELL_BODY},
};
static const FacePatch S_Z1[] = {
  {6,7,CELL_BODY},{7,7,CELL_BODY},{6,13,CELL_BODY},{7,13,CELL_BODY},
  {3,15,CELL_BODY},{3,16,CELL_BODY},{4,15,CELL_BODY},{4,16,CELL_BODY},
};
static const FacePatch S_Z2[] = {
  {6,7,CELL_BODY},{7,7,CELL_BODY},{6,13,CELL_BODY},{7,13,CELL_BODY},
  {2,16,CELL_BODY},{2,17,CELL_BODY},{3,16,CELL_BODY},{3,17,CELL_BODY},
  {4,15,CELL_BODY},
};
static const FacePatch S_Z3[] = {
  {6,7,CELL_BODY},{7,7,CELL_BODY},{6,13,CELL_BODY},{7,13,CELL_BODY},
  {1,17,CELL_BODY},{1,18,CELL_BODY},{2,17,CELL_BODY},{2,18,CELL_BODY},
  {3,16,CELL_BODY},
};
static const FacePatch S_Z4[] = {
  {6,7,CELL_BODY},{7,7,CELL_BODY},{6,13,CELL_BODY},{7,13,CELL_BODY},
  {0,18,CELL_BODY},{1,18,CELL_BODY},{2,17,CELL_BODY},
};
static const FacePatch S_Z5[] = {
  {6,7,CELL_BODY},{7,7,CELL_BODY},{6,13,CELL_BODY},{7,13,CELL_BODY},
  {0,18,CELL_BODY},{0,19,CELL_BODY},
};
static const FacePatch S_NOD_DOWN_Z[] = {
  {7,7,CELL_BODY},{8,7,CELL_BODY},{7,13,CELL_BODY},{8,13,CELL_BODY},
  {4,15,CELL_BODY},{4,16,CELL_BODY},
};
static const FacePatch S_NOD_UP_Z2[] = {
  {5,7,CELL_BODY},{6,7,CELL_BODY},{5,13,CELL_BODY},{6,13,CELL_BODY},
  {2,16,CELL_BODY},{2,17,CELL_BODY},{3,16,CELL_BODY},
};
static const FaceFrame SLEEP_FRAMES[] = {
  {600,  0, 0, FACE_PATCH(S_SLEEP)},
  {400,  1, 0, FACE_PATCH(S_NOD_DOWN)},
  {300,  0, 0, FACE_PATCH(S_SLEEP)},
  {300,  0, 0, FACE_PATCH(S_Z1)},
  {300,  1, 0, FACE_PATCH(S_NOD_DOWN_Z)},
  {300,  0, 0, FACE_PATCH(S_Z1)},
  {300,  0, 0, FACE_PATCH(S_Z2)},
  {300, -1, 0, FACE_PATCH(S_NOD_UP_Z2)},
  {300,  0, 0, FACE_PATCH(S_Z3)},
  {300,  1, 0, FACE_PATCH(S_NOD_DOWN)},
  {300,  0, 0, FACE_PATCH(S_Z4)},
  {300,  0, 0, FACE_PATCH(S_SLEEP)},
  {300,  0, 0, FACE_PATCH(S_Z5)},
  {400, -1, 0, FACE_PATCH(S_NOD_UP)},
  {700,  0, 0, FACE_PATCH(S_SLEEP)},
  {400,  1, 0, FACE_PATCH(S_NOD_DOWN)},
  {500,  0, 0, FACE_PATCH(S_SLEEP)},
  {300,  0, 0, FACE_PATCH(S_Z1)},
  {300,  0, 0, FACE_PATCH(S_Z2)},
  {300, -1, 0, FACE_PATCH(S_NOD_UP)},
  {300,  0, 0, FACE_PATCH(S_Z3)},
  {300,  0, 0, FACE_PATCH(S_Z4)},
  {300,  0, 0, FACE_PATCH(S_Z5)},
  {400,  0, 0, FACE_PATCH(S_SLEEP)},
};
static const uint8_t SLEEP_NFRAMES = sizeof(SLEEP_FRAMES) / sizeof(SLEEP_FRAMES[0]);

// ---- "Monk" scene: the Claude block-mascot seated in meditation -----------------
// A white stone monk (CELL_ROBE) sitting cross-legged with a raised open palm and
// grey robe-fold shadows (CELL_ROBE_SHADE) for shape. The HEAD is a SMALL white
// circular dome (cols 8-12, rows 4-8 — a 5x5 circle proportioned to the seated
// body, not the old head-filling dome) carrying an 8-point Claude burst (CELL_SPARK).
// The BODY is SLIM — tapered to match the
// 5-wide head rather than the old wide base: shoulders 8 (cols 6-13), torso 7
// (cols 7-13), crossed-leg knees 11 (cols 5-15), cushion 9 (cols 6-14), all
// centered on col 10 like the head. Only the raised left palm (cols 5-6, row 9)
// breaks that symmetry — that asymmetry IS the meditation gesture. The burst is
// sized to fill that small head exactly: a vertical spoke (col 10), a horizontal arm
// (row 6, cols 8-12) and four diagonals converging on the hub. The white dome shows
// through only as four wedge cells between the spokes and a thin rim halo — the burst
// is the same size as the head. Unlike the other scenes the BODY never shifts
// (meditation is still): the only motion is the burst, whose minor spokes shimmer in
// over the white wedges by toggling tip cells. Those tip cells sit on dome (white)
// cells, so a retracted spoke reverts to white — a clean pulse with no black holes.
// Patch coords are absolute (no dr), so the spokes always line up with the static hub
// — same reason the desk scene uses dr=0.
#define SP CELL_SPARK
#define RB CELL_ROBE
#define GS CELL_ROBE_SHADE
#define __ CELL_EMPTY
static const uint8_t monkBase[F_N][F_N] PROGMEM = {
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, // 0
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, // 1
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, // 2
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, // 3
  {__,__,__,__,__,__,__,__,__,RB,SP,RB,__,__,__,__,__,__,__,__}, // 4 dome top + N spoke
  {__,__,__,__,__,__,__,__,SP,RB,SP,RB,SP,__,__,__,__,__,__,__}, // 5 NW / N / NE spokes
  {__,__,__,__,__,__,__,__,SP,SP,SP,SP,SP,__,__,__,__,__,__,__}, // 6 horizontal arm + hub
  {__,__,__,__,__,__,__,__,SP,RB,SP,RB,SP,__,__,__,__,__,__,__}, // 7 SW / S / SE spokes
  {__,__,__,__,__,__,__,__,__,RB,SP,RB,__,__,__,__,__,__,__,__}, // 8 dome bottom + S spoke
  {__,__,__,__,__,RB,RB,__,__,RB,RB,RB,__,__,__,__,__,__,__,__}, // 9 raised palm + neck
  {__,__,__,__,__,__,RB,RB,RB,RB,RB,RB,RB,RB,__,__,__,__,__,__}, //10 forearm + shoulders
  {__,__,__,__,__,__,__,RB,RB,RB,GS,RB,RB,RB,__,__,__,__,__,__}, //11 torso + center fold
  {__,__,__,__,__,__,__,RB,RB,RB,GS,RB,RB,RB,__,__,__,__,__,__}, //12 torso + center fold
  {__,__,__,__,__,__,RB,RB,RB,RB,RB,RB,RB,RB,RB,__,__,__,__,__}, //13 lap
  {__,__,__,__,__,RB,RB,RB,RB,GS,GS,GS,RB,RB,RB,RB,__,__,__,__}, //14 crossed legs + seam
  {__,__,__,__,__,RB,RB,RB,RB,RB,GS,RB,RB,RB,RB,RB,__,__,__,__}, //15 crossed legs + seam
  {__,__,__,__,__,__,RB,RB,RB,RB,RB,RB,RB,RB,RB,__,__,__,__,__}, //16 cushion
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, //17
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, //18
  {__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__}, //19
};
#undef SP
#undef RB
#undef GS
#undef __

// Minor burst-spoke tips (absolute coords, no base shift). The base shows the eight
// main spokes; the preset shimmers the four white wedge cells between them — set A
// then set B (a slow rotation), with a brief full burst in between. Every tip lands
// on a dome (white) cell, so when a spoke retracts the cell reverts to white, never
// black.
static const FacePatch M_RAY_A[] = {
  {5,9,CELL_SPARK},{7,11,CELL_SPARK},
};
static const FacePatch M_RAY_B[] = {
  {5,11,CELL_SPARK},{7,9,CELL_SPARK},
};
static const FacePatch M_RAY_BURST[] = {
  {5,9,CELL_SPARK},{5,11,CELL_SPARK},{7,9,CELL_SPARK},{7,11,CELL_SPARK},
};
static const FaceFrame MONK_FRAMES[] = {
  {520, 0, 0, FACE_PATCH(M_RAY_A)},
  {200, 0, 0, FACE_PATCH(M_RAY_BURST)},
  {520, 0, 0, FACE_PATCH(M_RAY_B)},
  {200, 0, 0, FACE_PATCH(M_RAY_BURST)},
};
static const uint8_t MONK_NFRAMES = sizeof(MONK_FRAMES) / sizeof(MONK_FRAMES[0]);

static uint8_t faceGrid[F_N][F_N];   // the frame currently shown
static uint8_t facePrev[F_N][F_N];   // last grid drawn (for cell diffing)
static bool faceInit = false;
static uint8_t faceFrameIdx = 0;
static unsigned long faceFrameStart = 0;
static uint16_t faceBody = C_CLAY;   // current body color (tracks mood)
static uint8_t faceMoodId = 255;
static unsigned long faceClockSec = 0;

// Pick the active scene (base grid + frame list) by state.
static const FaceFrame *faceFrameList() {
  if (faceMode == FACE_MODE_WORKING) return WORK_FRAMES;
  if (faceMode == FACE_MODE_SLEEP) return SLEEP_FRAMES;
  if (faceMode == FACE_MODE_MONK) return MONK_FRAMES;
  return FACE_FRAMES;
}

static uint8_t faceFrameCount() {
  if (faceMode == FACE_MODE_WORKING) return WORK_NFRAMES;
  if (faceMode == FACE_MODE_SLEEP) return SLEEP_NFRAMES;
  if (faceMode == FACE_MODE_MONK) return MONK_NFRAMES;
  return FACE_NFRAMES;
}

static FaceExpr faceComputeExpr() {
  FaceExpr e;
  unsigned long ee = nowEpoch();
  int hour = ee ? (int)(((ee + TZ_OFFSET) / 3600UL) % 24UL) : 12;
  bool alarmed = (sessionPct >= 80) || (weeklyPct >= 90) ||
                 (unifiedStatus.length() && unifiedStatus != "allowed");
  bool sleepy = (hour >= 23 || hour < 7);
  // Body + accent are ALWAYS the Claude palette (clay creature, orange spark) —
  // never percent-tinted green/yellow/red. Only the mood *label* tracks state.
  if (alarmed) {
    e.eyeH = 32; e.mouth = 2; e.id = 2; e.mood = "ALERT";
  } else if (sleepy) {
    e.eyeH = 14; e.mouth = 3; e.id = 3; e.mood = "SLEEPY";
  } else if (sessionPct >= 40) {
    e.eyeH = 24; e.mouth = 1; e.id = 1; e.mood = "FOCUS";
  } else {
    e.eyeH = 26; e.mouth = 0; e.id = 0; e.mood = "HAPPY";
  }
  e.body = C_CLAY; e.spark = C_CLAUDE;
  // Explicit scene states override the mood label. Distinct ids force a status
  // repaint when switching scenes even if the body palette stays unchanged.
  if (faceMode == FACE_MODE_WORKING) { e.mood = "CODING"; e.id = 10; }
  else if (faceMode == FACE_MODE_SLEEP) { e.mood = "SLEEP"; e.id = 11; }
  else if (faceMode == FACE_MODE_MONK) { e.mood = "ZEN"; e.id = 12; }
  return e;
}

// Map a grid cell to a panel color. The face screen sits on the warm ivory Claude
// paper (C_CREAM), so EMPTY paints cream (the background) and the eye is warm ink
// (reads as a hole punched in the clay body, the same way it read as black on the
// old black field).
static uint16_t faceCellColor(uint8_t v, uint16_t body) {
  switch (v) {
    case CELL_BODY:      return body;        // mood color (clay)
    case CELL_EYE:       return C_INK;       // eye-hole (warm near-black on clay)
    case CELL_HP_LIGHT:  return C_HP_LIGHT;
    case CELL_HP_SHADOW: return C_HP_SHADOW;
    case CELL_SCREEN:    return C_SCREEN;
    case CELL_LBASE:     return C_LBASE;
    case CELL_LOGO:      return C_LOGO;
    case CELL_DESKTOP:   return C_DESKTOP;
    case CELL_DESKLEG:   return C_DESKLEG;
    case CELL_SPARK:     return C_CLAUDE;    // monk sunburst face (orange)
    case CELL_ROBE:      return C_WHITE;     // monk stone robe (static, not mood-tinted)
    case CELL_ROBE_SHADE: return C_GRAY;     // robe fold/edge shadow (gives shape)
    case CELL_ROBE_EDGE: return C_INK;       // ink silhouette outline on cream
    default:             return C_CREAM;     // empty == the ivory paper background
  }
}

// Build a frame grid: clear, copy the base shifted by (dr,dc), then apply the
// sparse cell patch. Cells shifted off-grid are dropped (cleared to empty), which
// is what makes the head-follow shift leave a clean trailing edge.
static void faceBuildGrid(uint8_t out[F_N][F_N], const FaceFrame &f,
                          const uint8_t base[F_N][F_N]) {
  for (int r = 0; r < F_N; r++)
    for (int c = 0; c < F_N; c++) out[r][c] = CELL_EMPTY;
  for (int r = 0; r < F_N; r++) {
    int nr = r + f.dr;
    if (nr < 0 || nr >= F_N) continue;
    for (int c = 0; c < F_N; c++) {
      int nc = c + f.dc;
      if (nc < 0 || nc >= F_N) continue;
      out[nr][nc] = pgm_read_byte(&base[r][c]);
    }
  }
  for (uint8_t i = 0; i < f.nops; i++)
    out[f.ops[i].r][f.ops[i].c] = f.ops[i].v;

  // Monk scene only: auto-outline the white stone robe with an ink silhouette so it
  // separates from the cream paper (pure-white robe is invisible on cream). Any empty
  // cell touching a robe cell (8-neighbour) becomes CELL_ROBE_EDGE. Safe in-place:
  // edge cells are never CELL_ROBE/_SHADE, so they can't seed further outline, and the
  // orange burst (CELL_SPARK) isn't robe, so its spoke tips against cream stay clean.
  if (faceMode == FACE_MODE_MONK) {
    for (int r = 0; r < F_N; r++) {
      for (int c = 0; c < F_N; c++) {
        if (out[r][c] != CELL_EMPTY) continue;
        bool near = false;
        for (int dr2 = -1; dr2 <= 1 && !near; dr2++) {
          for (int dc2 = -1; dc2 <= 1; dc2++) {
            int rr = r + dr2, cc = c + dc2;
            if (rr < 0 || rr >= F_N || cc < 0 || cc >= F_N) continue;
            uint8_t nv = out[rr][cc];
            if (nv == CELL_ROBE || nv == CELL_ROBE_SHADE) { near = true; break; }
          }
        }
        if (near) out[r][c] = CELL_ROBE_EDGE;
      }
    }
  }
}

// The base grid for the active scene.
static const uint8_t (*faceActiveBase())[F_N] {
  if (faceMode == FACE_MODE_WORKING) return deskBase;
  if (faceMode == FACE_MODE_MONK) return monkBase;
  return creatureBase;
}

// Repaint the grid. force=true redraws every cell (used on a body-color change);
// otherwise only cells that differ from the last drawn grid are touched, so a
// frame step paints just the handful of moved eye/edge cells.
static void faceRender(uint16_t body, bool force) {
  // Confined to the animation window: this both centres the creature and keeps
  // its (cream) empty cells from painting over the header/footer chrome.
  for (int r = FACE_R0; r <= FACE_R1; r++) {
    for (int c = FACE_C0; c <= FACE_C1; c++) {
      uint8_t v = faceGrid[r][c];
      if (force || v != facePrev[r][c]) {
        gfx->fillRect(FACE_OX + c * FACE_CELL, FACE_OY + r * FACE_CELL,
                      FACE_CELL, FACE_CELL, faceCellColor(v, body));
        facePrev[r][c] = v;
      }
    }
  }
}

// Static frame, drawn once per faceBegin: the Claude-brand chrome on the ivory
// paper — a soft rounded card edge, the burst mark + "Claude" wordmark header (same
// as the hero screen, so the companion reads as one family), a hairline divider, and
// the small-caps footer sub-labels. The per-second status redraw never touches any of
// this, so the sub-labels can't be ghosted by the clock tick.
static void faceDrawChrome() {
  gfx->drawRoundRect(2, 2, 236, 236, 12, C_MUTE);    // soft rounded card edge
  gfx->drawRoundRect(3, 3, 234, 234, 11, C_MUTE);
  drawClaudeIcon(16, 16, C_CLAUDE);                  // burst mark
  gfx->setTextSize(2);
  gfx->setTextColor(C_INK, C_CREAM);
  gfx->setCursor(32, 9); gfx->print("Claude");       // wordmark
  gfx->drawFastHLine(12, 33, 216, C_TAN);            // header rule
  gfx->drawFastHLine(12, 190, 216, C_TAN);           // creature | data divider
  gfx->setTextSize(1);
  gfx->setTextColor(C_MUTE, C_CREAM);
  gfx->setCursor(14, 194); gfx->print("MOOD");       // footer sub-labels
  printRight(226, 194, 1, "TIME", C_MUTE, C_CREAM);
}

// Dynamic footer readout on the ivory paper. Mood value (left, spark colour), the
// live S/W usage % (centre, muted — supplements the dedicated Claude screen), and the
// clock (right, ink). Each is cleared in its own cream rectangle so a variable-length
// mood (SLEEPY=6 vs HAPPY=5) or usage width leaves no ghost and the static sub-labels
// survive.
static void faceDrawStatus(const FaceExpr &e) {
  int sc = sessionPct, wc = weeklyPct;
  String usage = "S" + pctText(sc) + " W" + pctText(wc);
  gfx->fillRect(84, 193, 92, 9, C_CREAM);            // clear old usage label (centred band)
  printCentered(194, 1, usage, C_MUTE, C_CREAM);

  unsigned long ee = nowEpoch();
  String clk = ee ? hhmmss(ee + TZ_OFFSET) : String("--:--:--");
  gfx->fillRect(12, 205, 96, 16, C_CREAM);           // clear old mood value
  gfx->setTextSize(2);
  gfx->setTextColor(e.spark, C_CREAM);
  gfx->setCursor(14, 206); gfx->print(e.mood);
  gfx->fillRect(130, 205, 104, 16, C_CREAM);         // clear old clock
  printRight(232, 206, 2, clk, C_INK, C_CREAM);
}

static void faceBegin() {
  faceInit = true;
  faceFrameIdx = 0;
  faceFrameStart = millis();
  FaceExpr e = faceComputeExpr();
  faceBody = e.body; faceMoodId = e.id;
  gfx->fillScreen(C_CREAM);   // ivory Claude paper (only fillScreen on this screen)
  faceDrawChrome();
  faceBuildGrid(faceGrid, faceFrameList()[0], faceActiveBase());
  memset(facePrev, 0xFF, sizeof(facePrev));   // sentinel -> first render draws all
  faceRender(faceBody, true);
  faceClockSec = 0;
  faceDrawStatus(e);
}

// One animation step. Called every loop() iteration; advances the preset on its
// own per-frame hold timer (the reference frame.hold values), redrawing only the
// cells that change. No timer ISR, so it never starves WiFi/TCP.
static void faceTick() {
  if (!faceInit) { faceBegin(); return; }
  unsigned long now = millis();
  unsigned long sec = nowEpoch();
  FaceExpr e = faceComputeExpr();

  // Mood -> body color. Repaint the *current* frame in the new color (without
  // resetting the animation), so a usage change recolors the creature in place.
  if (e.body != faceBody || e.id != faceMoodId) {
    faceBody = e.body; faceMoodId = e.id;
    faceRender(faceBody, true);
    faceDrawStatus(e); faceClockSec = sec;
  }

  // Advance to the next preset frame once the current one's hold has elapsed.
  const FaceFrame *frames = faceFrameList();
  if (now - faceFrameStart >= frames[faceFrameIdx].hold) {
    faceFrameIdx = (faceFrameIdx + 1) % faceFrameCount();
    faceFrameStart = now;
    faceBuildGrid(faceGrid, frames[faceFrameIdx], faceActiveBase());
    faceRender(faceBody, false);
  }

  // Tick the clock once a second.
  if (sec != faceClockSec) { faceClockSec = sec; faceDrawStatus(e); }
}

void drawMeter() {
  if (lcdScreen == SCREEN_MAC) { drawMacMeter(); return; }
  if (lcdScreen == SCREEN_DESK) { drawDeskSign(); return; }
  if (lcdScreen == SCREEN_FACE) { faceBegin(); return; }

  // Two-phase repaint: draw the cream card chrome once on switch-in, then only
  // repaint values on every /usage push (no fillScreen flash). The no-data case
  // (Claude down) renders the same card with "--" placeholders, not a black screen.
  if (!claudeChromeReady) {
    gfx->fillScreen(C_CREAM);
    drawClaudeChrome();
    claudeChromeReady = true;
  }

  // Header status: API state word + pulsing dot, then the Thailand-time clock.
  if (unifiedStatus.length()) {
    uint16_t c = statusColor();
    drawClaudeStatusDot(true);
    String label = unifiedStatus;
    label.toUpperCase();
    if (label.length() > 8) label = label.substring(0, 8);
    gfx->fillRect(122, 12, 56, 8, C_CREAM);
    gfx->setTextSize(1);
    gfx->setTextColor(c, C_CREAM);
    gfx->setCursor(124, 12);
    gfx->print(label);
  }
  unsigned long e = nowEpoch();
  gfx->fillRect(184, 12, 48, 8, C_CREAM);
  printRight(232, 12, 1, e ? hhmmss(e + TZ_OFFSET) : String("--:--:--"),
             C_INK, C_CREAM);

  drawClaudeHero();
  drawClaudeWeekly();
  drawIpPanel();
}

// Toggle the companion between idle (look-around), working (desk-coding), and sleep.
// /face?state=idle|working|sleep|monk|toggle . Switches the LCD to face mode if needed and
// restarts the animation so the new scene draws immediately. Defined after
// drawMeter()/faceBegin() so it can call them without a forward prototype.
void handleFace() {
  if (server.hasArg("state")) {
    String st = server.arg("state");
    st.toLowerCase();
    if (st == "working") faceMode = FACE_MODE_WORKING;
    else if (st == "sleep") faceMode = FACE_MODE_SLEEP;
    else if (st == "monk") faceMode = FACE_MODE_MONK;
    else if (st == "idle") faceMode = FACE_MODE_IDLE;
    else if (st == "toggle") faceMode = (faceMode + 1) % FACE_MODE_COUNT;
    lcdScreen = SCREEN_FACE;
    lastTickEpoch = 0;
    faceInit = false;        // force a fresh faceBegin() for the new scene
    drawMeter();
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", faceModeName());
}

void handleUpdateDone() {
  server.sendHeader("Connection", "close");
  if (!otaUpdateOk || Update.hasError() || otaError.length()) {
    String msg = otaError.length() ? otaError : String("unknown update error");
    server.send(500, "text/plain", "Update failed: " + msg);
    otaInProgress = false;
    otaUpdateOk = false;
    WiFi.setSleepMode(WIFI_MODEM_SLEEP);
    drawMeter();
    return;
  }

  server.send(200, "text/html",
              "<!doctype html><meta charset='utf-8'>"
              "<body style='font-family:system-ui'>Update OK. Rebooting...</body>");
  delay(500);
  ESP.restart();
}

void handleFirmwareUpload() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaInProgress = true;
    otaUpdateOk = false;
    otaError = "";
    Serial.printf("OTA update: %s\n", upload.filename.c_str());
    WiFiUDP::stopAll();
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    backlightStopForFlash();   // load-bearing: PWM ISR + flash write = reset mid-OTA
    drawOtaStatus("uploading firmware...");

    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketchSpace, U_FLASH)) {
      setOtaError();
    }
  } else if (upload.status == UPLOAD_FILE_WRITE && !otaError.length()) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      setOtaError();
    }
  } else if (upload.status == UPLOAD_FILE_END && !otaError.length()) {
    if (Update.end(true)) {
      Serial.printf("OTA success: %u bytes\n", upload.totalSize);
      otaUpdateOk = true;
      drawOtaStatus("success, rebooting...");
    } else {
      setOtaError();
      drawOtaStatus("failed");
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    otaError = "upload aborted";
    otaInProgress = false;
    otaUpdateOk = false;
    WiFi.setSleepMode(WIFI_MODEM_SLEEP);
    setBacklight(lcdBrightness);   // OTA failed, no reboot: restore the live PWM level
    drawOtaStatus("aborted");
  }

  yield();
}

void setup() {
  Serial.begin(115200);

  // Capture why we just reset BEFORE anything else can clobber it. A repeated
  // "Exception"/"Watchdog" here = a crash loop; "Power on"/"Brown out" = power.
  // Exposed in /usage.json as "rst" so it's readable over the network (no USB).
  bootReason = ESP.getResetReason();
  bootInfo = ESP.getResetInfo();
  Serial.println();
  Serial.print("Reset reason: "); Serial.println(bootReason);
  Serial.print("Reset info: ");   Serial.println(bootInfo);

  // --- Display init (mirrors the GeekMagic open firmware) ---
  pinMode(LCD_BL, OUTPUT);
  EEPROM.begin(4);
  applyBrightness(loadBrightness(), false);  // PWM-dimmed; full-on ran the panel hot
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

  // Let the radio idle between AP beacons instead of full active RX. Combined with
  // the delay() in loop() (which yields to the SDK), average WiFi current drops a
  // lot -> the ESP8266 runs cooler. CPU stays on, so the web server stays responsive.
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);

  // NTP in UTC (we apply the UTC+7 offset at display time). This gives the wait
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
  server.on("/brightness", HTTP_GET, handleBrightness);
  server.on("/brightness", HTTP_POST, handleBrightness);
  server.on("/mode", HTTP_GET, handleMode);
  server.on("/mode", HTTP_POST, handleMode);
  server.on("/face", HTTP_GET, handleFace);
  server.on("/face", HTTP_POST, handleFace);
  server.on("/desk", HTTP_GET, handleDesk);
  server.on("/desk", HTTP_POST, handleDesk);
  server.on("/restart", HTTP_GET, handleRestart);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/factory-reset", HTTP_GET, handleFactoryReset);
  server.on("/factory-reset", HTTP_POST, handleFactoryReset);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleFirmwareUpload);
  Serial.println("OTA update: http://clawdmeter.local/update");
  server.begin();
}

void loop() {
  if (!otaInProgress) MDNS.update();
  server.handleClient();
  if (otaInProgress) {
    // Keep feeding the SDK/WiFi stack between upload chunks. Returning here
    // without a yield can make OTA unstable on slower or lossy WiFi links.
    delay(2);
    return;
  }
  if (meterRedrawPending) {
    meterRedrawPending = false;
    // The face scene animates continuously and faceTick() already absorbs new
    // usage/mood values, so a daemon push must NOT call faceBegin() — that would
    // fillScreen + restart the animation from frame 0 (a once-a-minute flash).
    if (lcdScreen != SCREEN_FACE) drawMeter();
  }

  if (lcdScreen == SCREEN_FACE) {
    faceTick();    // self-paced animation; owns its own clock + redraws
    delay(2);      // keep the WiFi modem-sleep yield (see note below)
    return;
  }

  if (lcdScreen == SCREEN_DESK && millis() - deskAnimLastMs >= 120UL) {
    deskAnimLastMs = millis();
    drawDeskAnimatedStatus();
  }

  if (lcdScreen == SCREEN_CLAUDE && unifiedStatus.length()) {
    drawClaudeStatusDot(false);
  }

  unsigned long e = nowEpoch();
  if (e != 0) {
    if (lcdScreen == SCREEN_MAC) {
      // MAC clock is HH:MM, so refresh it once a minute. Daemon /usage pushes
      // still trigger a full redraw through meterRedrawPending above.
      unsigned long minute = e / 60UL;
      if (minute != lastTickEpoch) { lastTickEpoch = minute; tickDynamic(); }
    } else if (e != lastTickEpoch) {
      // With data: tick the clock + countdown once a second, no full redraw.
      lastTickEpoch = e;
      tickDynamic();
    }
  }

  // Yield to the SDK so WIFI_MODEM_SLEEP can actually engage between beacons.
  // 2 ms is invisible to a 1 s clock tick and a page polled once per second.
  delay(2);
}
