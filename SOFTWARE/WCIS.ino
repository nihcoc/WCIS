/*
 * ╔══════════════════════════════════════════════════╗
 * ║       ESP32 WATER VALVE SCHEDULER                ║
 * ║  Servo Pin: 17 | Enable Pin: 22                  ║
 * ║  Dual daily schedule via Wi-Fi web interface     ║
 * ╚══════════════════════════════════════════════════╝
 *
 * Libraries required (install via Arduino Library Manager):
 *   - ESP32Servo   by Kevin Harrington
 *   - ArduinoJson  by Benoit Blanchon
 *
 * Board: ESP32 Dev Module
 */
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>
#include <time.h>
// ─── USER CONFIG ────────────────────────────────────────────────────────────
const char* WIFI_SSID      = "change";
const char* WIFI_PASSWORD  = "chnage";
const char* NTP_SERVER     = "pool.ntp.org";
const long  GMT_OFFSET_S   = 14400;   // UTC+4 Dubai — adjust for your timezone
const int   DAYLIGHT_S     = 0;
const int SERVO_OPEN_DEG   = -40;      // Angle when valve is OPEN
const int SERVO_CLOSE_DEG  = 90;       // Angle when valve is CLOSED
const int VALVE_DURATION_S = 1800;    // Seconds valve stays open (30 min)
// How long (ms) to wait after writing servo position before assuming travel is done.
const unsigned long SERVO_TRAVEL_MS = 10000;
const unsigned long SERVO_ENABLE_SETTLE_MS = 200;
const unsigned long SERVO_HOLD_CLOSED_MS   = 300;
const unsigned long SERVO_DISABLE_GAP_MS   = 20;
const int SERVO_PIN        = 17;
const int SERVO_ENABLE_PIN = 22;
// ────────────────────────────────────────────────────────────────────────────
// ─── VALVE STATE MACHINE ────────────────────────────────────────────────────
enum ValveState {
  VS_IDLE,
  VS_ENABLING,
  VS_MOVING_OPEN,
  VS_OPEN,
  VS_MOVING_CLOSE,
  VS_POWERING_DOWN,
  VS_DISABLE_WAIT
};
ValveState    valveState      = VS_IDLE;
unsigned long stateEnteredAt  = 0;
unsigned long valveOpenAt     = 0;
bool          pendingClose    = false;
Servo       waterValve;
WebServer   server(80);
Preferences prefs;
// ─── SCHEDULE ───────────────────────────────────────────────────────────────
struct Schedule {
  bool    enabled;
  uint8_t hour;
  uint8_t minute;
};
Schedule sched[2];
int lastMinuteChecked = -1;
// ─── NVS HELPERS ────────────────────────────────────────────────────────────
void saveSchedule() {
  prefs.begin("valve", false);
  for (int i = 0; i < 2; i++) {
    String p = "s" + String(i);
    prefs.putBool((p + "en").c_str(), sched[i].enabled);
    prefs.putUChar((p + "h").c_str(), sched[i].hour);
    prefs.putUChar((p + "m").c_str(), sched[i].minute);
  }
  prefs.end();
}
void loadSchedule() {
  prefs.begin("valve", true);
  for (int i = 0; i < 2; i++) {
    String p = "s" + String(i);
    sched[i].enabled = prefs.getBool((p + "en").c_str(), false);
    sched[i].hour    = prefs.getUChar((p + "h").c_str(), 6 + i * 12);
    sched[i].minute  = prefs.getUChar((p + "m").c_str(), 0);
  }
  prefs.end();
}
// ─── TIME HELPERS ───────────────────────────────────────────────────────────
bool tryGetLocalTimeFast(struct tm* out) {
  time_t now = time(nullptr);
  if (now < 24 * 60 * 60) {
    return false;
  }
  localtime_r(&now, out);
  return true;
}
// ─── VALVE HELPERS ──────────────────────────────────────────────────────────
void beginCloseMotion(unsigned long now) {
  pendingClose = false;
  valveState = VS_MOVING_CLOSE;
  stateEnteredAt = now;
  if (!waterValve.attached()) {
    waterValve.attach(SERVO_PIN);
  }
  digitalWrite(SERVO_ENABLE_PIN, HIGH);
  waterValve.write(SERVO_CLOSE_DEG);
}
bool requestOpen() {
  if (valveState != VS_IDLE) {
    return false;
  }
  Serial.println("[VALVE] Open requested");
  pendingClose = false;
  valveState = VS_ENABLING;
  stateEnteredAt = millis();
  digitalWrite(SERVO_ENABLE_PIN, HIGH);
  return true;
}
bool requestClose() {
  if (valveState == VS_IDLE) {
    return false;
  }
  if (valveState == VS_OPEN) {
    Serial.println("[VALVE] Close requested");
    beginCloseMotion(millis());
    return true;
  }
  pendingClose = true;
  return true;
}
// ─── NON-BLOCKING STATE MACHINE ─────────────────────────────────────────────
void updateValveStateMachine() {
  unsigned long now = millis();
  switch (valveState) {
    case VS_IDLE:
      break;
    case VS_ENABLING:
      if (now - stateEnteredAt >= SERVO_ENABLE_SETTLE_MS) {
        waterValve.attach(SERVO_PIN);
        waterValve.write(SERVO_OPEN_DEG);
        valveState = VS_MOVING_OPEN;
        stateEnteredAt = now;
        Serial.println("[VALVE] Moving to OPEN");
      }
      break;
    case VS_MOVING_OPEN:
      if (now - stateEnteredAt >= SERVO_TRAVEL_MS) {
        valveState = VS_OPEN;
        valveOpenAt = now;
        Serial.println("[VALVE] OPEN");
        if (pendingClose) {
          Serial.println("[VALVE] Pending close detected");
          beginCloseMotion(now);
        }
      }
      break;
    case VS_OPEN:
      if (pendingClose ||
          (now - valveOpenAt >= (unsigned long)VALVE_DURATION_S * 1000UL)) {
        Serial.println("[VALVE] Starting close sequence");
        beginCloseMotion(now);
      }
      break;
    case VS_MOVING_CLOSE:
      if (now - stateEnteredAt >= SERVO_TRAVEL_MS) {
        valveState = VS_POWERING_DOWN;
        stateEnteredAt = now;
        Serial.println("[VALVE] Reached CLOSED, holding...");
      }
      break;
    case VS_POWERING_DOWN:
      if (now - stateEnteredAt >= SERVO_HOLD_CLOSED_MS) {
        waterValve.detach();
        valveState = VS_DISABLE_WAIT;
        stateEnteredAt = now;
        Serial.println("[VALVE] PWM detached");
      }
      break;
    case VS_DISABLE_WAIT:
      if (now - stateEnteredAt >= SERVO_DISABLE_GAP_MS) {
        digitalWrite(SERVO_ENABLE_PIN, LOW);
        valveState = VS_IDLE;
        Serial.println("[VALVE] IDLE (power off)");
      }
      break;
  }
}
bool isValveOpen() {
  return valveState == VS_ENABLING ||
         valveState == VS_MOVING_OPEN ||
         valveState == VS_OPEN;
}
bool isScheduleValueValid(int hour, int minute) {
  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}
// ─── SCHEDULE CHECK ─────────────────────────────────────────────────────────
void checkSchedule() {
  struct tm t;
  if (!tryGetLocalTimeFast(&t)) {
    return;
  }
  int currentMin = t.tm_hour * 60 + t.tm_min;
  if (currentMin == lastMinuteChecked) {
    return;
  }
  lastMinuteChecked = currentMin;
  for (int i = 0; i < 2; i++) {
    if (!sched[i].enabled) {
      continue;
    }
    if (currentMin == sched[i].hour * 60 + sched[i].minute) {
      Serial.printf("[SCHED] Schedule %d fired at %02d:%02d\n", i, t.tm_hour, t.tm_min);
      requestOpen();
    }
  }
}
// ─── API HANDLERS ───────────────────────────────────────────────────────────
void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
}
void handleOptions() {
  addCorsHeaders();
  server.send(204);
}
void handleGetStatus() {
  struct tm t;
  bool hasTime = tryGetLocalTimeFast(&t);
  char timeBuf[20] = "Syncing...";
  if (hasTime) {
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  }
  long remaining = 0;
  if (valveState == VS_OPEN) {
    long elapsed = (long)((millis() - valveOpenAt) / 1000UL);
    remaining = max(0L, (long)VALVE_DURATION_S - elapsed);
  }
  StaticJsonDocument<512> doc;
  doc["valve"] = isValveOpen();
  doc["state"] = (int)valveState;
  doc["remaining"] = remaining;
  doc["time"] = timeBuf;
  doc["ip"] = WiFi.localIP().toString();
  JsonArray arr = doc.createNestedArray("schedules");
  for (int i = 0; i < 2; i++) {
    JsonObject o = arr.createNestedObject();
    o["enabled"] = sched[i].enabled;
    o["hour"] = sched[i].hour;
    o["minute"] = sched[i].minute;
  }
  String out;
  serializeJson(doc, out);
  addCorsHeaders();
  server.send(200, "application/json", out);
}
void handleSetSchedule() {
  if (!server.hasArg("plain")) {
    addCorsHeaders();
    server.send(400, "text/plain", "No body");
    return;
  }
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    addCorsHeaders();
    server.send(400, "text/plain", "Bad JSON");
    return;
  }
  int idx    = doc["index"] | -1;
  int hour   = doc["hour"] | -1;
  int minute = doc["minute"] | -1;
  bool en    = doc["enabled"] | false;
  if (idx < 0 || idx > 1) {
    addCorsHeaders();
    server.send(400, "text/plain", "index must be 0 or 1");
    return;
  }
  if (!isScheduleValueValid(hour, minute)) {
    addCorsHeaders();
    server.send(400, "text/plain", "hour must be 0-23 and minute 0-59");
    return;
  }
  sched[idx].enabled = en;
  sched[idx].hour = (uint8_t)hour;
  sched[idx].minute = (uint8_t)minute;
  saveSchedule();
  addCorsHeaders();
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.printf("[API] Schedule %d: %02d:%02d en=%d\n",
    idx, sched[idx].hour, sched[idx].minute, sched[idx].enabled);
}
void handleManualOpen() {
  bool accepted = requestOpen();
  addCorsHeaders();
  server.send(200, "application/json",
    accepted ? "{\"ok\":true,\"action\":\"opening\"}" :
               "{\"ok\":true,\"action\":\"ignored\"}");
}
void handleManualClose() {
  bool accepted = requestClose();
  addCorsHeaders();
  server.send(200, "application/json",
    accepted ? "{\"ok\":true,\"action\":\"closing_or_queued\"}" :
               "{\"ok\":true,\"action\":\"ignored\"}");
}
// ─── EMBEDDED WEB PAGE ──────────────────────────────────────────────────────
void handleRoot() {
  const char* html = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1.0"/>
<title>AquaTimer</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Bebas+Neue&display=swap');
:root{
  --bg:#0d0f0e;--surface:#141816;--card:#1a1e1b;--border:#2a3028;
  --accent:#4ade80;--accent2:#86efac;--danger:#f87171;
  --muted:#4a5c4e;--text:#d1fae5;--text-dim:#6b8c72;
  --glow:0 0 24px #4ade8044;
}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:'Share Tech Mono',monospace;
  min-height:100vh;padding:24px 16px 48px;overflow-x:hidden;position:relative}
body::before{content:'';position:fixed;inset:0;
  background:repeating-linear-gradient(0deg,transparent,transparent 2px,rgba(0,0,0,.06) 2px,rgba(0,0,0,.06) 4px);
  pointer-events:none;z-index:999}
body::after{content:'';position:fixed;inset:0;
  background-image:linear-gradient(var(--border) 1px,transparent 1px),linear-gradient(90deg,var(--border) 1px,transparent 1px);
  background-size:40px 40px;opacity:.3;pointer-events:none;z-index:0}
.wrap{max-width:680px;margin:0 auto;position:relative;z-index:1}
header{text-align:center;margin-bottom:36px;padding-top:12px}
.logo-row{display:flex;align-items:center;justify-content:center;gap:16px;margin-bottom:6px}
.pipe{width:60px;height:3px;border-radius:2px;background:linear-gradient(90deg,transparent,var(--accent))}
.pipe.r{background:linear-gradient(90deg,var(--accent),transparent)}
h1{font-family:'Bebas Neue',cursive;font-size:clamp(2.4rem,8vw,4rem);letter-spacing:.12em;
  color:var(--accent);text-shadow:var(--glow);line-height:1}
.sub{color:var(--text-dim);font-size:.72rem;letter-spacing:.25em;text-transform:uppercase}
#clock{font-family:'Bebas Neue',cursive;font-size:clamp(3rem,12vw,5.5rem);color:var(--accent2);
  letter-spacing:.06em;text-align:center;text-shadow:var(--glow);margin:18px 0 8px}
.card{background:var(--card);border:1px solid var(--border);border-radius:4px;padding:22px;
  margin-bottom:14px;position:relative;overflow:hidden}
.card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;
  background:linear-gradient(90deg,transparent,var(--accent),transparent);opacity:.5}
.ctitle{font-size:.62rem;letter-spacing:.3em;text-transform:uppercase;color:var(--text-dim);
  margin-bottom:16px;display:flex;align-items:center;gap:10px}
.ctitle::after{content:'';flex:1;height:1px;background:var(--border)}
.vstatus{display:flex;align-items:center;gap:18px}
.orb{width:52px;height:52px;border-radius:50%;border:2px solid var(--muted);
  display:flex;align-items:center;justify-content:center;font-size:1.4rem;transition:all .4s;flex-shrink:0}
.orb.open{border-color:var(--accent);background:#4ade8020;box-shadow:0 0 20px #4ade8066;animation:pulse 2s infinite}
.orb.moving{border-color:#facc15;background:#facc1520;box-shadow:0 0 14px #facc1555;animation:pulse-y 1s infinite}
@keyframes pulse{0%,100%{box-shadow:0 0 16px #4ade8055}50%{box-shadow:0 0 36px #4ade8099}}
@keyframes pulse-y{0%,100%{opacity:1}50%{opacity:.5}}
.vlabel{font-family:'Bebas Neue',cursive;font-size:1.7rem;letter-spacing:.1em;transition:color .3s}
.vlabel.open{color:var(--accent)}.vlabel.closed{color:var(--muted)}.vlabel.moving{color:#facc15}
.vsub{font-size:.68rem;color:var(--text-dim);letter-spacing:.08em;margin-top:4px}
#countdown{font-size:.72rem;color:var(--accent2);letter-spacing:.1em;margin-top:6px;min-height:1.2em}
.brow{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:18px}
button{font-family:'Share Tech Mono',monospace;font-size:.78rem;letter-spacing:.15em;text-transform:uppercase;
  padding:13px 10px;border-radius:3px;border:1px solid;cursor:pointer;transition:all .2s;background:transparent}
.bo{border-color:var(--accent);color:var(--accent)}
.bo:hover,.bo:active{background:var(--accent);color:var(--bg);box-shadow:var(--glow)}
.bc{border-color:var(--danger);color:var(--danger)}
.bc:hover,.bc:active{background:var(--danger);color:var(--bg);box-shadow:0 0 20px #f8717155}
.slot{padding:16px;border:1px solid var(--border);border-radius:3px;margin-bottom:12px;
  background:var(--surface);transition:border-color .3s}
.slot.active{border-color:#2a3a2e}
.sh{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px}
.snum{font-family:'Bebas Neue',cursive;font-size:1.15rem;color:var(--text-dim);letter-spacing:.1em}
.toggle{position:relative;width:44px;height:24px}
.toggle input{opacity:0;width:0;height:0}
.slider{position:absolute;inset:0;background:var(--border);border-radius:24px;cursor:pointer;transition:.3s}
.slider::before{content:'';position:absolute;width:18px;height:18px;left:3px;top:3px;
  background:var(--muted);border-radius:50%;transition:.3s}
input:checked+.slider{background:#1f3a26}
input:checked+.slider::before{transform:translateX(20px);background:var(--accent);box-shadow:0 0 8px var(--accent)}
.trow{display:flex;align-items:center;gap:10px}
.tlbl{font-size:.62rem;color:var(--text-dim);letter-spacing:.2em;text-transform:uppercase;min-width:52px}
.tinp{display:flex;align-items:center;gap:6px}
.tinp select{background:var(--bg);color:var(--accent);border:1px solid var(--border);border-radius:3px;
  padding:8px 10px;font-family:'Share Tech Mono',monospace;font-size:1rem;cursor:pointer;
  appearance:none;-webkit-appearance:none;width:64px;text-align:center}
.tinp select:focus{outline:none;border-color:var(--accent)}
.tsep{color:var(--accent);font-size:1.2rem;font-family:'Bebas Neue',cursive}
.sbar{display:flex;align-items:center;justify-content:space-between;padding:10px 14px;
  background:var(--surface);border:1px solid var(--border);border-radius:3px;
  font-size:.62rem;color:var(--text-dim);letter-spacing:.1em;margin-bottom:14px}
.dot{width:7px;height:7px;border-radius:50%;background:var(--accent);
  box-shadow:0 0 6px var(--accent);animation:blink 2s infinite;display:inline-block;margin-right:6px}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.3}}
#toast{position:fixed;bottom:28px;left:50%;transform:translateX(-50%);background:var(--card);
  border:1px solid var(--accent);color:var(--accent);padding:10px 24px;border-radius:3px;
  font-size:.72rem;letter-spacing:.15em;opacity:0;pointer-events:none;transition:opacity .3s;
  z-index:1000;white-space:nowrap}
#toast.show{opacity:1}
@media(max-width:400px){.brow{grid-template-columns:1fr}}
</style>
</head>
<body>
<div class="wrap">
<header>
  <div class="logo-row">
    <div class="pipe"></div>
    <h1>AQUATIMER</h1>
    <div class="pipe r"></div>
  </div>
  <p class="sub">ESP32 &middot; Valve Scheduler &middot; GPIO 17</p>
</header>
<div class="sbar">
  <span><span class="dot"></span>ONLINE</span>
  <span id="ip-disp">—</span>
  <span>EN: GPIO22</span>
</div>
<div id="clock">--:--:--</div>
<div class="card">
  <div class="ctitle">Valve Status</div>
  <div class="vstatus">
    <div class="orb" id="orb">💧</div>
    <div>
      <div class="vlabel closed" id="vlbl">CLOSED</div>
      <div class="vsub" id="vsub">Idle</div>
      <div id="countdown"></div>
    </div>
  </div>
  <div class="brow">
    <button class="bo" onclick="manualOpen()">&#9654; Open Valve</button>
    <button class="bc" onclick="manualClose()">&#9632; Close Valve</button>
  </div>
</div>
<div class="card">
  <div class="ctitle">Daily Schedules</div>
  <div class="slot" id="slot-0">
    <div class="sh">
      <span class="snum">SCHEDULE 01</span>
      <label class="toggle"><input type="checkbox" id="en0" onchange="saveSched(0)"/><span class="slider"></span></label>
    </div>
    <div class="trow">
      <span class="tlbl">TIME</span>
      <div class="tinp">
        <select id="h0" onchange="saveSched(0)"></select>
        <span class="tsep">:</span>
        <select id="m0" onchange="saveSched(0)"></select>
      </div>
    </div>
  </div>
  <div class="slot" id="slot-1">
    <div class="sh">
      <span class="snum">SCHEDULE 02</span>
      <label class="toggle"><input type="checkbox" id="en1" onchange="saveSched(1)"/><span class="slider"></span></label>
    </div>
    <div class="trow">
      <span class="tlbl">TIME</span>
      <div class="tinp">
        <select id="h1" onchange="saveSched(1)"></select>
        <span class="tsep">:</span>
        <select id="m1" onchange="saveSched(1)"></select>
      </div>
    </div>
  </div>
</div>
</div>
<div id="toast"></div>
<script>
const VS_IDLE=0,VS_ENABLING=1,VS_MOVING_OPEN=2,VS_OPEN=3,VS_MOVING_CLOSE=4,VS_POWERING_DOWN=5,VS_DISABLE_WAIT=6;
['h0','h1'].forEach(id=>{
  const el=document.getElementById(id);
  for(let i=0;i<24;i++){const o=document.createElement('option');o.value=i;o.textContent=String(i).padStart(2,'0');el.appendChild(o);}
});
['m0','m1'].forEach(id=>{
  const el=document.getElementById(id);
  for(let i=0;i<60;i++){const o=document.createElement('option');o.value=i;o.textContent=String(i).padStart(2,'0');el.appendChild(o);}
});
function toast(msg){
  const t=document.getElementById('toast');
  t.textContent=msg;t.classList.add('show');
  setTimeout(()=>t.classList.remove('show'),2200);
}
function fmtTime(sec){
  const m=Math.floor(sec/60),s=sec%60;
  return String(m).padStart(2,'0')+':'+String(s).padStart(2,'0')+' remaining';
}
function updateValve(valve, state, remaining){
  const orb=document.getElementById('orb');
  const lbl=document.getElementById('vlbl');
  const sub=document.getElementById('vsub');
  const cd =document.getElementById('countdown');
  if(state===VS_ENABLING||state===VS_MOVING_OPEN){
    orb.className='orb moving';
    lbl.className='vlabel moving';lbl.textContent='OPENING';
    sub.textContent='Servo moving \xb7 Pin 22 active';
    cd.textContent='';
  } else if(state===VS_OPEN){
    orb.className='orb open';
    lbl.className='vlabel open';lbl.textContent='OPEN';
    sub.textContent='Valve open \xb7 Pin 22 active';
    cd.textContent=remaining>0?fmtTime(remaining):'';
  } else if(state===VS_MOVING_CLOSE||state===VS_POWERING_DOWN||state===VS_DISABLE_WAIT){
    orb.className='orb moving';
    lbl.className='vlabel moving';lbl.textContent='CLOSING';
    sub.textContent='Servo moving \xb7 Pin 22 active';
    cd.textContent='';
  } else {
    orb.className='orb';
    lbl.className='vlabel closed';lbl.textContent='CLOSED';
    sub.textContent='Idle \xb7 Pin 22 off';
    cd.textContent='';
  }
}
async function fetchStatus(){
  try{
    const d=await(await fetch('/api/status')).json();
    document.getElementById('clock').textContent=d.time;
    document.getElementById('ip-disp').textContent=d.ip;
    updateValve(d.valve, d.state, d.remaining);
    d.schedules.forEach((s,i)=>{
      document.getElementById('en'+i).checked=s.enabled;
      document.getElementById('h'+i).value=s.hour;
      document.getElementById('m'+i).value=s.minute;
      document.getElementById('slot-'+i).classList.toggle('active',s.enabled);
    });
  }catch(e){}
}
async function saveSched(idx){
  const p={index:idx,enabled:document.getElementById('en'+idx).checked,
    hour:parseInt(document.getElementById('h'+idx).value,10),
    minute:parseInt(document.getElementById('m'+idx).value,10)};
  document.getElementById('slot-'+idx).classList.toggle('active',p.enabled);
  try{
    const r=await fetch('/api/schedule',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});
    if(!r.ok){throw new Error('save failed');}
    toast('SCH 0'+(idx+1)+' \u2192 '+String(p.hour).padStart(2,'0')+':'+String(p.minute).padStart(2,'0')+(p.enabled?' ON':' OFF'));
  }catch(e){toast('ERROR');}
}
async function manualOpen(){
  try{
    const r=await fetch('/api/open',{method:'POST'});
    if(!r.ok){throw new Error('open failed');}
    toast('OPEN REQUESTED');
  }catch(e){toast('ERROR');}
}
async function manualClose(){
  try{
    const r=await fetch('/api/close',{method:'POST'});
    if(!r.ok){throw new Error('close failed');}
    toast('CLOSE REQUESTED');
  }catch(e){toast('ERROR');}
}
fetchStatus();
setInterval(fetchStatus,1000);
</script>
</body>
</html>)rawhtml";
  server.send(200, "text/html", html);
}
// ─── SETUP ──────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] Water Valve Controller");
  pinMode(SERVO_ENABLE_PIN, OUTPUT);
  digitalWrite(SERVO_ENABLE_PIN, LOW);
  loadSchedule();
  Serial.printf("[SCHED] S0: %02d:%02d (%s)  S1: %02d:%02d (%s)\n",
    sched[0].hour, sched[0].minute, sched[0].enabled ? "ON" : "OFF",
    sched[1].hour, sched[1].minute, sched[1].enabled ? "ON" : "OFF");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Connecting");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 30) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] AP fallback");
    WiFi.softAP("ValveController", "12345678");
    Serial.println("[AP] 192.168.4.1  pass: 12345678");
    Serial.println("[TIME] NTP unavailable in AP-only mode; schedules need a valid clock");
  }
  configTime(GMT_OFFSET_S, DAYLIGHT_S, NTP_SERVER);
  server.on("/",             HTTP_GET,     handleRoot);
  server.on("/api/status",   HTTP_GET,     handleGetStatus);
  server.on("/api/status",   HTTP_OPTIONS, handleOptions);
  server.on("/api/schedule", HTTP_POST,    handleSetSchedule);
  server.on("/api/schedule", HTTP_OPTIONS, handleOptions);
  server.on("/api/open",     HTTP_POST,    handleManualOpen);
  server.on("/api/open",     HTTP_OPTIONS, handleOptions);
  server.on("/api/close",    HTTP_POST,    handleManualClose);
  server.on("/api/close",    HTTP_OPTIONS, handleOptions);
  server.onNotFound([]() {
    addCorsHeaders();
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
  Serial.println("[HTTP] Server ready");
}
// ─── LOOP ───────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();
  updateValveStateMachine();
  static unsigned long lastChk = 0;
  if (millis() - lastChk >= 1000) {
    lastChk = millis();
    checkSchedule();
  }
}
