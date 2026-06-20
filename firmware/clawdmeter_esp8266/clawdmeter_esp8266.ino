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
uint8_t lcdScreen = SCREEN_CLAUDE;
String deskStatus = "coding";   // coding / busy / break
String deskText = "CODING";
String deskColorName = "green";
bool otaInProgress = false;     // true while /update is writing firmware
bool otaUpdateOk = false;
bool meterRedrawPending = false;
String otaError = "";
String bootReason = "";         // why the chip last reset (captured once at boot)
String bootInfo = "";           // detailed reset info (exception cause/stack on crash)

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
  :root{color-scheme:dark;--bg:#0d1117;--panel:#161b22;--panel2:#10151d;--line:#30363d;--text:#f0f6fc;--muted:#8b949e;--green:#3fb950;--yellow:#d29922;--red:#f85149;--blue:#58a6ff}
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
  <div class="control"><span class="muted">Desk</span><div class="actions"><button class="btn deskBtn" data-status="coding">Coding</button><button class="btn deskBtn" data-status="busy">Busy</button><button class="btn deskBtn" data-status="break">Break</button></div><span class="v" id="deskValue">--</span></div>
  <div class="control"><span class="muted">Custom</span><div class="deskCustom"><input id="deskText" type="text" maxlength="12" value="CODING"><select id="deskColor"><option value="green">Green</option><option value="red">Red</option><option value="amber">Amber</option><option value="blue">Blue</option><option value="white">White</option></select><button class="btn" id="deskApply">Apply</button></div><span></span></div>
  <div class="control"><span class="muted">Brightness</span><input id="brightness" type="range" min="0" max="120" value="90"><span class="v" id="brightnessValue">90</span></div>
</section>
</main>
<script>
function color(p){return p>=90?'#f85149':p>=60?'#d29922':'#3fb950'}
var TZ=25200; // Asia/Bangkok UTC+7
function pad2(n){return (n<10?'0':'')+n}
function lt(e){return new Date((e+TZ)*1000)} // local (ICT) Date via UTC getters
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
  view=view=='claude'?'mac':view=='mac'?'desk':'claude';
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
    var now=d.now||0;
    document.getElementById('clock').textContent=now?utcClock(now):'--:--:--';
    document.getElementById('age').textContent=ageText(d.age);
    var live=d.age>=0&&d.age<=120;
    document.body.className=live?'':'stale';
    if(view=='desk'){
      document.getElementById('title').textContent='Desk Status';
      document.getElementById('subtitle').textContent='Physical status sign';
      document.getElementById('switchMode').textContent='Claude';
      document.getElementById('switchMode').href='?view=claude';
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
      dot.style.background=d.deskColor=='red'?'var(--red)':d.deskColor=='amber'?'var(--yellow)':d.deskColor=='blue'?'var(--blue)':d.deskColor=='white'?'var(--text)':'var(--green)';
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
        d.sr ? 'Reset '+utcHHMM(d.sr)+' ICT' : 'Reset --';
      document.getElementById('sc').textContent = d.sr&&now ? countdown(d.sr-now) : '--';
      document.getElementById('wt').textContent =
        d.wr ? 'Reset '+DOW[lt(d.wr).getUTCDay()]+' '+utcHHMM(d.wr)+' ICT' : 'Reset --';
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
             ",\"screen\":\"" + String(lcdScreen == SCREEN_DESK ? "desk" : lcdScreen == SCREEN_MAC ? "mac" : "claude") + "\"" +
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
    else if (screen == "claude") nextScreen = SCREEN_CLAUDE;
    if (nextScreen != lcdScreen) {
      lcdScreen = nextScreen;
      lastTickEpoch = 0;
      drawMeter();
    }
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", lcdScreen == SCREEN_DESK ? "desk" : lcdScreen == SCREEN_MAC ? "mac" : "claude");
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
         color == "blue" || color == "white";
}

void handleDesk() {
  bool changed = false;
  if (server.hasArg("status")) {
    String status = server.arg("status");
    status.toLowerCase();
    if (status == "coding" || status == "busy" || status == "break") {
      deskStatus = status;
      if (status == "busy") {
        deskText = "BUSY";
        deskColorName = "red";
      } else if (status == "break") {
        deskText = "BREAK";
        deskColorName = "amber";
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

static uint16_t deskColor() {
  if (deskColorName == "red") return C_RED;
  if (deskColorName == "amber") return C_AMBER;
  if (deskColorName == "blue") return C_BLUE;
  if (deskColorName == "white") return C_WHITE;
  return C_GREEN;
}

static void drawOtaStatus(const String &line) {
  gfx->fillScreen(C_BLACK);
  gfx->setTextColor(C_WHITE, C_BLACK);
  gfx->setTextSize(2);
  gfx->setCursor(16, 76);
  gfx->print("OTA update");
  gfx->setTextColor(C_GRAY, C_BLACK);
  gfx->setTextSize(1);
  gfx->setCursor(16, 112);
  gfx->print(line);
  gfx->setCursor(16, 132);
  gfx->print(WiFi.localIP());
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

static void drawMacBlock(int y, const char *label, int pct) {
  gfx->setTextSize(2);
  gfx->setTextColor(C_WHITE, C_BLACK);
  gfx->setCursor(12, y + 4);
  gfx->print(label);
  printRight(228, y + 4, 2, (pct < 0) ? "--" : String(pct) + "%", C_WHITE, C_BLACK);

  const int bx = 12, by = y + 28, bw = 216, bh = 14;
  gfx->fillRect(bx, by, bw, bh, C_BLACK);
  gfx->drawRect(bx, by, bw, bh, C_LINE);
  if (pct > 0) {
    int p = pct > 100 ? 100 : pct;
    gfx->fillRect(bx + 2, by + 2, (bw - 4) * p / 100, bh - 4, barColor(pct));
  }
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

  if (lcdScreen == SCREEN_DESK) {
    gfx->fillRect(132, 178, 96, 10, C_BLACK);
    printRight(228, 178, 1, hhmmss(e + TZ_OFFSET) + " " TZ_LABEL, C_WHITE, C_BLACK);
    return;
  }

  gfx->fillRect(150, 8, 86, 10, C_BLACK);
  printRight(236, 9, 1, hhmmss(e + TZ_OFFSET) + " " TZ_LABEL, C_WHITE, C_BLACK);

  if (lcdScreen == SCREEN_CLAUDE && sessReset) {
    gfx->fillRect(150, 82, 86, 10, C_BLACK);
    printRight(236, 82, 1, countdown((long)sessReset - (long)e), C_WHITE, C_BLACK);
  }
}

void drawMacMeter() {
  gfx->fillScreen(C_BLACK);

  gfx->setTextSize(1);
  gfx->setTextColor(C_WHITE, C_BLACK);
  gfx->setCursor(4, 9);
  gfx->print("MAC MONITOR");
  unsigned long e = nowEpoch();
  printRight(236, 9, 1, e ? hhmmss(e + TZ_OFFSET) + " " TZ_LABEL
                          : String("--:--:-- " TZ_LABEL), C_WHITE, C_BLACK);
  gfx->drawFastHLine(0, 27, 240, C_LINE);

  drawMacBlock(34, "CPU", macCpuPct);
  drawMacBlock(78, "MEM", macMemPct);
  drawMacBlock(122, "DISK", macDiskPct);
  drawMacBlock(166, "BATT", macBatteryPct);
  drawIpPanel();
}

void drawDeskSign() {
  gfx->fillScreen(C_BLACK);

  String label = deskText;
  label.toUpperCase();
  uint16_t c = deskColor();

  gfx->fillRect(0, 0, 240, 34, c);
  gfx->setTextSize(2);
  gfx->setTextColor(C_BLACK, c);
  gfx->setCursor(14, 10);
  gfx->print("DESK STATUS");

  uint8_t textSize = (label.length() <= 8) ? 4 : 3;
  gfx->setTextSize(textSize);
  gfx->setTextColor(c, C_BLACK);
  int textW = label.length() * 6 * textSize;
  gfx->setCursor((240 - textW) / 2, 88);
  gfx->print(label);

  gfx->setTextSize(1);
  gfx->setTextColor(C_GRAY, C_BLACK);
  String hint = "set from dashboard";
  gfx->setCursor((240 - (int)hint.length() * 6) / 2, 146);
  gfx->print(hint);

  unsigned long e = nowEpoch();
  printRight(228, 178, 1, e ? hhmmss(e + TZ_OFFSET) + " " TZ_LABEL
                            : String("--:--:-- " TZ_LABEL), C_WHITE, C_BLACK);
  drawIpPanel();
}

void drawMeter() {
  if (lcdScreen == SCREEN_MAC) { drawMacMeter(); return; }
  if (lcdScreen == SCREEN_DESK) { drawDeskSign(); return; }

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
  server.on("/brightness", HTTP_GET, handleBrightness);
  server.on("/brightness", HTTP_POST, handleBrightness);
  server.on("/mode", HTTP_GET, handleMode);
  server.on("/mode", HTTP_POST, handleMode);
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
    drawMeter();
  }

  unsigned long e = nowEpoch();
  if (e != 0) {
    if (lcdScreen == SCREEN_CLAUDE && sessionPct < 0 && weeklyPct < 0) {
      // Wait screen: refresh the big clock/date once a minute.
      unsigned long minute = e / 60UL;
      if (minute != lastTickEpoch) { lastTickEpoch = minute; drawWaitingTime(); }
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
