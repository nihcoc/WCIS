
#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include "time.h"

// ─── USER CONFIG ─────────────────────────────────────────────────────────────
const char* SSID       = "change";
const char* PASSWORD   = "change";
const int   SERVO_PIN  = 17;
const int   POWER_PIN  = 22;    // controls servo power rail
const int   POS_OPEN   = -40;
const int   POS_CLOSED =  90;
const long  UTC_OFFSET = 4 * 3600;   // UTC+4  (Abu Dhabi / Dubai)

// How long to keep power on after sending the servo signal.
// 700 ms is enough for a typical servo to travel 130 degrees.
// Increase to 1000 if your servo is slow or under load.
#define SERVO_POWER_ON_MS  10000

#define WIFI_CHECK_INTERVAL    5000
#define WIFI_RECONNECT_TIMEOUT 10000
// ─────────────────────────────────────────────────────────────────────────────

Servo       myServo;
WebServer   server(80);
Preferences prefs;
String      currentState  = "unknown";
bool        ntpSynced     = false;

unsigned long lastWifiCheckMs   = 0;
unsigned long reconnectStartMs  = 0;
bool          reconnecting      = false;

// ─── SCHEDULE STORAGE ────────────────────────────────────────────────────────
#define MAX_SCHEDULES 10

struct Schedule {
  uint8_t startHour, startMin;
  uint8_t endHour,   endMin;
  uint8_t days;
  bool    enabled;
};

Schedule schedules[MAX_SCHEDULES];
int      scheduleCount      = 0;
bool     lastScheduleActive = false;
unsigned long lastCheckMs   = 0;

void saveSchedules() {
  prefs.begin("sched", false);
  prefs.putInt("cnt", scheduleCount);
  prefs.putBytes("data", schedules, sizeof(schedules));
  prefs.end();
}

void loadSchedules() {
  prefs.begin("sched", true);
  scheduleCount = constrain(prefs.getInt("cnt", 0), 0, MAX_SCHEDULES);
  if (scheduleCount > 0)
    prefs.getBytes("data", schedules, sizeof(schedules));
  prefs.end();
}

// ─── SERVO + POWER GATING ────────────────────────────────────────────────────
int degreesToMicros(int deg) {
  return (int)(544.0 + (deg * (2400.0 - 544.0) / 180.0));
}

void moveServo(int pos, String state) {
  Serial.printf("Servo -> %s  (%d deg)  [power ON]\n", state.c_str(), pos);

  // 1. Power the servo rail
  digitalWrite(POWER_PIN, HIGH);

  // 2. Brief settling time – lets the servo power up before receiving signal
  millis(10000);

  // 3. Send target position
  int us = constrain(degreesToMicros(pos), 400, 2500);
  myServo.writeMicroseconds(us);

  // 4. Wait for physical movement to complete
  millis(SERVO_POWER_ON_MS);

  // 5. Cut power – servo holds position mechanically (or via internal lock)
  digitalWrite(POWER_PIN, LOW);

  currentState = state;
  Serial.printf("Servo reached %s  [power OFF]\n", state.c_str());
}

// ─── WIFI AUTO-RECONNECT ─────────────────────────────────────────────────────
void startReconnect() {
  Serial.println("[WiFi] Connection lost. Reconnecting...");
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(SSID, PASSWORD);
  reconnecting     = true;
  reconnectStartMs = millis();
}

void handleWifi() {
  if (millis() - lastWifiCheckMs < WIFI_CHECK_INTERVAL) return;
  lastWifiCheckMs = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (reconnecting) {
      reconnecting = false;
      ntpSynced    = false;
      Serial.printf("[WiFi] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
      configTime(UTC_OFFSET, 0, "pool.ntp.org", "time.nist.gov");
      struct tm t;
      if (getLocalTime(&t)) {
        ntpSynced = true;
        Serial.printf("[NTP]  Re-synced: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
      }
    }
    return;
  }

  if (!reconnecting) { startReconnect(); return; }
  if (millis() - reconnectStartMs > WIFI_RECONNECT_TIMEOUT) {
    Serial.println("[WiFi] Timeout. Retrying...");
    startReconnect();
  }
}

// ─── SCHEDULE CHECKER ────────────────────────────────────────────────────────
void checkSchedules() {
  if (millis() - lastCheckMs < 10000) return;
  lastCheckMs = millis();
  if (scheduleCount == 0) return;

  struct tm t;
  if (!getLocalTime(&t)) { Serial.println("[Sched] NTP not ready"); return; }

  int     nowMin = t.tm_hour * 60 + t.tm_min;
  uint8_t dayBit = (uint8_t)(1 << t.tm_wday);

  bool active = false;
  for (int i = 0; i < scheduleCount && !active; i++) {
    if (!schedules[i].enabled)         continue;
    if (!(schedules[i].days & dayBit)) continue;
    int s = schedules[i].startHour * 60 + schedules[i].startMin;
    int e = schedules[i].endHour   * 60 + schedules[i].endMin;
    if (nowMin >= s && nowMin < e) active = true;
  }

  if ( active && !lastScheduleActive) moveServo(POS_OPEN,   "open");
  if (!active &&  lastScheduleActive) moveServo(POS_CLOSED, "closed");
  lastScheduleActive = active;
  
}

// ─── HTML PAGE ────────────────────────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM =

  "<!DOCTYPE html><html lang='en'><head>"
  "<meta charset='UTF-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
  "<title>WCIS Control Center</title>"
  "<link href='https://fonts.googleapis.com/css2?family=Rajdhani:wght@300;500;700&family=Share+Tech+Mono&display=swap' rel='stylesheet'>"
  "<style>"
  "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}"
  ":root{--bg:#0a0c10;--panel:#10141c;--border:#1e2538;--accent:#00e5ff;--open-c:#00e5ff;--closed-c:#ff4b6e;--text:#c8d6e8;--dim:#4a5568;}"
  "body{min-height:100vh;background:var(--bg);display:flex;flex-direction:column;align-items:center;justify-content:flex-start;padding:28px 0 48px;font-family:'Rajdhani',sans-serif;color:var(--text);overflow-y:auto;}"
  "body::before{content:'';position:fixed;inset:0;pointer-events:none;background-image:linear-gradient(rgba(0,229,255,0.04) 1px,transparent 1px),linear-gradient(90deg,rgba(0,229,255,0.04) 1px,transparent 1px);background-size:40px 40px;animation:gridShift 20s linear infinite;}"
  "@keyframes gridShift{to{background-position:40px 40px}}"
  ".wifi-bar{width:min(460px,94vw);display:flex;align-items:center;gap:8px;font-family:'Share Tech Mono',monospace;font-size:10px;letter-spacing:2px;color:var(--dim);margin-bottom:10px;padding:0 2px;}"
  ".wifi-dot{width:7px;height:7px;border-radius:50%;background:var(--dim);transition:background 0.4s,box-shadow 0.4s;flex-shrink:0;}"
  ".wifi-bar.online .wifi-dot{background:#39ff7e;box-shadow:0 0 7px #39ff7e;}"
  ".wifi-bar.online .wifi-label{color:#39ff7e;}"
  ".wifi-bar.offline .wifi-dot{background:var(--closed-c);box-shadow:0 0 7px var(--closed-c);animation:blink 1s step-start infinite;}"
  ".wifi-bar.offline .wifi-label{color:var(--closed-c);}"
  ".wifi-bar.reconnecting .wifi-dot{background:#ffd24b;box-shadow:0 0 7px #ffd24b;animation:blink 0.5s step-start infinite;}"
  ".wifi-bar.reconnecting .wifi-label{color:#ffd24b;}"
  "@keyframes blink{50%{opacity:0}}"
  ".card{position:relative;background:var(--panel);border:1px solid var(--border);border-radius:4px;padding:44px 48px 40px;width:min(460px,94vw);box-shadow:0 0 60px rgba(0,0,0,0.6),inset 0 1px 0 rgba(255,255,255,0.04);}"
  ".card::before,.card::after,.card .cbr,.card .cbl{content:'';position:absolute;width:12px;height:12px;border-color:var(--accent);border-style:solid;opacity:0.6;}"
  ".card::before{top:8px;left:8px;border-width:2px 0 0 2px}"
  ".card::after{top:8px;right:8px;border-width:2px 2px 0 0}"
  ".card .cbr{bottom:8px;right:8px;border-width:0 2px 2px 0}"
  ".card .cbl{bottom:8px;left:8px;border-width:0 0 2px 2px}"
  ".clock-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:20px}"
  ".chip{font-family:'Share Tech Mono',monospace;font-size:9px;letter-spacing:2px;color:var(--dim);text-transform:uppercase;}"
  ".clock{font-family:'Share Tech Mono',monospace;font-size:1.2rem;color:var(--accent);letter-spacing:3px;text-shadow:0 0 10px rgba(0,229,255,0.35);}"
  "h1{font-size:2rem;font-weight:700;letter-spacing:6px;text-transform:uppercase;color:#fff;margin-bottom:32px;text-align:center;text-shadow:0 0 18px rgba(0,229,255,0.35);}"

  // Servo visualiser
  ".servo-vis{margin:0 auto 32px;width:120px;height:120px;position:relative}"
  ".servo-body{width:100%;height:100%;border-radius:50%;border:2px solid var(--border);background:radial-gradient(circle at 35% 35%,#1a2030,#0a0c10);position:relative;box-shadow:0 0 30px rgba(0,229,255,0.08);}"
  ".servo-arc{position:absolute;inset:8px;border-radius:50%;border:2px dashed rgba(0,229,255,0.15)}"
  ".servo-arm{position:absolute;top:50%;left:50%;width:46px;height:4px;background:linear-gradient(90deg,var(--accent),rgba(0,229,255,0.3));transform-origin:6px 50%;transform:translateY(-50%) rotate(0deg);transition:transform 0.6s cubic-bezier(0.34,1.56,0.64,1);border-radius:2px;margin-left:-6px;box-shadow:0 0 8px rgba(0,229,255,0.5);}"
  ".servo-hub{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);width:14px;height:14px;background:var(--accent);border-radius:50%;box-shadow:0 0 10px var(--accent);}"

  // Power indicator dot next to status
  ".status-wrap{margin-bottom:28px;display:flex;align-items:center;justify-content:center;gap:10px}"
  ".status-dot{width:8px;height:8px;border-radius:50%;background:var(--dim);transition:background 0.4s,box-shadow 0.4s;flex-shrink:0}"
  ".status-text{font-family:'Share Tech Mono',monospace;font-size:13px;letter-spacing:2px;color:var(--dim);text-transform:uppercase;transition:color 0.4s}"
  "body.is-open .status-dot{background:var(--open-c);box-shadow:0 0 8px var(--open-c)}"
  "body.is-open .status-text{color:var(--open-c)}"
  "body.is-closed .status-dot{background:var(--closed-c);box-shadow:0 0 8px var(--closed-c)}"
  "body.is-closed .status-text{color:var(--closed-c)}"

  // Power pill badge
  ".power-pill{"
    "display:inline-flex;align-items:center;gap:5px;"
    "font-family:'Share Tech Mono',monospace;font-size:9px;letter-spacing:2px;"
    "border-radius:10px;padding:2px 8px;margin-left:8px;"
    "border:1px solid var(--border);color:var(--dim);"
    "transition:all 0.3s;"
  "}"
  ".power-pill .pdot{width:5px;height:5px;border-radius:50%;background:var(--dim);transition:all 0.3s;}"
  ".power-pill.on{border-color:#ffd24b;color:#ffd24b;}"
  ".power-pill.on .pdot{background:#ffd24b;box-shadow:0 0 5px #ffd24b;}"

  ".btn-row{display:grid;grid-template-columns:1fr 1fr;gap:14px}"
  ".btn{padding:18px 10px;border:1.5px solid currentColor;border-radius:3px;background:transparent;cursor:pointer;font-family:'Rajdhani',sans-serif;font-size:1.1rem;font-weight:700;letter-spacing:4px;text-transform:uppercase;transition:background 0.2s,box-shadow 0.2s,transform 0.1s;position:relative;overflow:hidden;}"
  ".btn::after{content:'';position:absolute;inset:0;background:currentColor;opacity:0;transition:opacity 0.2s}"
  ".btn:hover::after{opacity:0.08}"
  ".btn:active{transform:scale(0.97)}"
  ".btn:active::after{opacity:0.16}"
  ".btn-open{color:var(--open-c)}"
  ".btn-closed{color:var(--closed-c)}"
  ".btn-open.active{background:rgba(0,229,255,0.12);box-shadow:0 0 20px rgba(0,229,255,0.25)}"
  ".btn-closed.active{background:rgba(255,75,110,0.12);box-shadow:0 0 20px rgba(255,75,110,0.25)}"
  ".btn:disabled{opacity:0.4;cursor:not-allowed;transform:none!important;}"
  ".btn-icon{display:block;font-size:1.5rem;margin-bottom:4px}"
  ".readout{margin-top:24px;font-family:'Share Tech Mono',monospace;font-size:11px;color:var(--dim);letter-spacing:2px;text-align:center;}"
  ".readout span{color:var(--accent)}"
  ".divider{border:none;border-top:1px solid var(--border);margin:28px 0}"
  ".sched-title-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px}"
  ".sched-badge{font-family:'Share Tech Mono',monospace;font-size:10px;background:rgba(0,229,255,0.08);color:var(--accent);border:1px solid rgba(0,229,255,0.25);border-radius:10px;padding:2px 9px;}"
  ".sched-list{display:flex;flex-direction:column;gap:8px;margin-bottom:14px}"
  ".sched-item{display:flex;align-items:center;gap:10px;background:rgba(0,229,255,0.03);border:1px solid var(--border);border-radius:3px;padding:10px 12px;}"
  ".sched-time{font-family:'Share Tech Mono',monospace;font-size:12px;color:var(--text);white-space:nowrap;min-width:118px;}"
  ".sched-days{display:flex;gap:4px;flex:1;flex-wrap:wrap}"
  ".dp{font-family:'Share Tech Mono',monospace;font-size:9px;width:22px;height:22px;display:flex;align-items:center;justify-content:center;border-radius:50%;border:1px solid var(--border);color:var(--dim);}"
  ".dp.on{border-color:var(--accent);color:var(--accent);background:rgba(0,229,255,0.1)}"
  ".del-btn{background:transparent;border:1px solid rgba(255,75,110,0.35);color:rgba(255,75,110,0.6);border-radius:3px;width:26px;height:26px;cursor:pointer;font-size:13px;flex-shrink:0;display:flex;align-items:center;justify-content:center;transition:all 0.2s;line-height:1;}"
  ".del-btn:hover{background:rgba(255,75,110,0.1);border-color:var(--closed-c);color:var(--closed-c)}"
  ".empty-sched{font-family:'Share Tech Mono',monospace;font-size:11px;color:var(--dim);letter-spacing:2px;text-align:center;padding:18px 0;}"
  ".sched-form{background:rgba(0,0,0,0.25);border:1px solid var(--border);border-radius:3px;padding:18px 16px;}"
  ".form-lbl{font-family:'Share Tech Mono',monospace;font-size:9px;letter-spacing:2px;color:var(--dim);display:block;margin-bottom:6px;text-transform:uppercase;}"
  ".time-row{display:flex;align-items:flex-end;gap:10px;margin-bottom:14px}"
  ".time-field{flex:1}"
  ".time-sep{font-family:'Share Tech Mono',monospace;color:var(--dim);font-size:16px;padding-bottom:9px}"
  "input[type='time']{width:100%;background:var(--bg);border:1px solid var(--border);color:var(--text);font-family:'Share Tech Mono',monospace;font-size:14px;padding:8px 10px;border-radius:3px;outline:none;transition:border-color 0.2s;color-scheme:dark;}"
  "input[type='time']:focus{border-color:var(--accent)}"
  ".days-row{display:flex;gap:6px;margin-bottom:16px;justify-content:space-between}"
  ".day-cb{display:flex;flex-direction:column;align-items:center;gap:4px;cursor:pointer;flex:1}"
  ".day-cb input{display:none}"
  ".day-cb span{font-family:'Share Tech Mono',monospace;font-size:9px;width:100%;max-width:36px;height:30px;display:flex;align-items:center;justify-content:center;border-radius:3px;border:1px solid var(--border);color:var(--dim);transition:all 0.2s;cursor:pointer;user-select:none;}"
  ".day-cb input:checked+span{border-color:var(--accent);color:var(--accent);background:rgba(0,229,255,0.1);box-shadow:0 0 8px rgba(0,229,255,0.15);}"
  ".btn-add{width:100%;padding:13px;border:1.5px solid var(--accent);border-radius:3px;background:transparent;cursor:pointer;font-family:'Rajdhani',sans-serif;font-size:1rem;font-weight:700;letter-spacing:4px;text-transform:uppercase;color:var(--accent);transition:background 0.2s,box-shadow 0.2s,transform 0.1s;}"
  ".btn-add:hover{background:rgba(0,229,255,0.08);box-shadow:0 0 20px rgba(0,229,255,0.2)}"
  ".btn-add:active{transform:scale(0.98)}"
  ".toast{position:fixed;bottom:28px;left:50%;transform:translateX(-50%) translateY(80px);background:rgba(16,20,28,0.96);border:1px solid var(--border);border-radius:3px;padding:10px 22px;font-family:'Share Tech Mono',monospace;font-size:12px;letter-spacing:2px;color:var(--text);transition:transform 0.35s cubic-bezier(0.34,1.56,0.64,1),opacity 0.35s;opacity:0;pointer-events:none;white-space:nowrap;z-index:99;}"
  ".toast.show{transform:translateX(-50%) translateY(0);opacity:1}"
  "</style></head><body>"

  "<div class='wifi-bar' id='wifiBar'>"
    "<div class='wifi-dot'></div>"
    "<span class='wifi-label' id='wifiLabel'>CHECKING...</span>"
  "</div>"

  "<div class='card'>"
    "<div class='cbr'></div><div class='cbl'></div>"
    "<div class='clock-row'>"
      "<span class='chip'>ESP32 &middot; SERVO</span>"
      "<span class='clock' id='clock'>--:--:--</span>"
    "</div>"
    "<h1>Gate Control</h1>"
    "<div class='servo-vis'>"
      "<div class='servo-body'>"
        "<div class='servo-arc'></div>"
        "<div class='servo-arm' id='arm'></div>"
        "<div class='servo-hub'></div>"
      "</div>"
    "</div>"
    "<div class='status-wrap'>"
      "<div class='status-dot'></div>"
      "<div class='status-text' id='statusText'>--</div>"
      "<span class='power-pill' id='powerPill'><span class='pdot'></span>PWR</span>"
    "</div>"
    "<div class='btn-row'>"
      "<button class='btn btn-open' id='btnOpen' onclick='sendCmd(\"open\")'>"
        "<span class='btn-icon'>&#x2302;</span>OPEN"
      "</button>"
      "<button class='btn btn-closed' id='btnClosed' onclick='sendCmd(\"closed\")'>"
        "<span class='btn-icon'>&#x229E;</span>CLOSE"
      "</button>"
    "</div>"
    "<div class='readout'>POS_OPEN <span>-40&deg;</span> &nbsp;|&nbsp; POS_CLOSED <span>90&deg;</span></div>"
    "<hr class='divider'>"
    "<div class='sched-title-row'>"
      "<span class='chip'>WEEKLY SCHEDULES</span>"
      "<span class='sched-badge' id='schedBadge'>0 active</span>"
    "</div>"
    "<div class='sched-list' id='schedList'>"
      "<div class='empty-sched'>No schedules yet</div>"
    "</div>"
    "<div class='sched-form'>"
      "<div class='time-row'>"
        "<div class='time-field'>"
          "<label class='form-lbl' for='tStart'>Open at</label>"
          "<input type='time' id='tStart' value='08:00'>"
        "</div>"
        "<div class='time-sep'>&rarr;</div>"
        "<div class='time-field'>"
          "<label class='form-lbl' for='tEnd'>Close at</label>"
          "<input type='time' id='tEnd' value='09:00'>"
        "</div>"
      "</div>"
      "<div class='days-row'>"
        "<label class='day-cb'><input type='checkbox' value='2'  checked><span>Mo</span></label>"
        "<label class='day-cb'><input type='checkbox' value='4'  checked><span>Tu</span></label>"
        "<label class='day-cb'><input type='checkbox' value='8'  checked><span>We</span></label>"
        "<label class='day-cb'><input type='checkbox' value='16' checked><span>Th</span></label>"
        "<label class='day-cb'><input type='checkbox' value='32' checked><span>Fr</span></label>"
        "<label class='day-cb'><input type='checkbox' value='64'><span>Sa</span></label>"
        "<label class='day-cb'><input type='checkbox' value='1' ><span>Su</span></label>"
      "</div>"
      "<button class='btn-add' onclick='addSchedule()'>+ ADD SCHEDULE</button>"
    "</div>"
  "</div>"

  "<div class='toast' id='toast'></div>"

  "<script>"
  "var DISP_LABELS=['Mo','Tu','We','Th','Fr','Sa','Su'];"
  "var DISP_BITS=[2,4,8,16,32,64,1];"
  "var toastTimer;"
  "var busy=false;"  // blocks double-clicks during servo move

  "function armAngle(deg){return ((deg+40)/110)*100-50;}"

  "function setState(state){"
    "document.body.className='is-'+state;"
    "document.getElementById('statusText').textContent=state.toUpperCase();"
    "document.getElementById('btnOpen').classList.toggle('active',state==='open');"
    "document.getElementById('btnClosed').classList.toggle('active',state==='closed');"
    "var angle=state==='open'?armAngle(-40):armAngle(90);"
    "document.getElementById('arm').style.transform='translateY(-50%) rotate('+angle+'deg)';"
  "}"

  // Show power-on pill while servo is moving, then hide
  "function showPowerBusy(ms){"
    "var pill=document.getElementById('powerPill');"
    "var btns=document.querySelectorAll('.btn');"
    "pill.classList.add('on');"
    "busy=true;"
    "for(var i=0;i<btns.length;i++) btns[i].disabled=true;"
    "setTimeout(function(){"
      "pill.classList.remove('on');"
      "busy=false;"
      "for(var i=0;i<btns.length;i++) btns[i].disabled=false;"
    "},ms);"
  "}"

  "function showToast(msg){"
    "var t=document.getElementById('toast');"
    "t.textContent=msg;"
    "t.classList.add('show');"
    "clearTimeout(toastTimer);"
    "toastTimer=setTimeout(function(){t.classList.remove('show');},2400);"
  "}"

  "function sendCmd(state){"
    "if(busy){showToast('SERVO MOVING...');return;}"
    "showPowerBusy(900);"   // match SERVO_POWER_ON_MS + 80ms settle + margin
    "fetch('/'+state)"
      ".then(function(r){"
        "if(r.ok){setState(state);showToast('SERVO -> '+state.toUpperCase()+' OK');}"
        "else{showToast('ERROR '+r.status);}"
      "})"
      ".catch(function(){showToast('CONNECTION LOST');});"
  "}"

  "function updateWifi(){"
    "fetch('/wifistatus')"
      ".then(function(r){return r.text();})"
      ".then(function(s){"
        "var bar=document.getElementById('wifiBar');"
        "var lbl=document.getElementById('wifiLabel');"
        "bar.className='wifi-bar '+s;"
        "if(s==='online')lbl.textContent='WIFI CONNECTED';"
        "else if(s==='reconnecting')lbl.textContent='RECONNECTING...';"
        "else lbl.textContent='WIFI OFFLINE';"
      "})"
      ".catch(function(){});"
  "}"
  "setInterval(updateWifi,4000);updateWifi();"

  "function updateClock(){"
    "fetch('/time')"
      ".then(function(r){return r.text();})"
      ".then(function(t){document.getElementById('clock').textContent=t;})"
      ".catch(function(){});"
  "}"
  "setInterval(updateClock,1000);updateClock();"

  "function loadSchedules(){"
    "fetch('/schedules')"
      ".then(function(r){return r.json();})"
      ".then(function(arr){renderSchedules(arr);})"
      ".catch(function(){});"
  "}"

  "function renderSchedules(arr){"
    "var el=document.getElementById('schedList');"
    "document.getElementById('schedBadge').textContent=arr.length+' active';"
    "if(arr.length===0){el.innerHTML='<div class=\"empty-sched\">No schedules yet</div>';return;}"
    "var html='';"
    "for(var i=0;i<arr.length;i++){"
      "var s=arr[i];"
      "var dayHtml='';"
      "for(var d=0;d<7;d++){"
        "var on=(s.days&DISP_BITS[d])?' on':'';"
        "dayHtml+='<span class=\"dp'+on+'\">'+DISP_LABELS[d]+'</span>';"
      "}"
      "html+='<div class=\"sched-item\">'"
            "+'<div class=\"sched-time\">'+s.start+' &rarr; '+s.end+'</div>'"
            "+'<div class=\"sched-days\">'+dayHtml+'</div>'"
            "+'<button class=\"del-btn\" onclick=\"deleteSchedule('+s.id+')\">&#x2715;</button>'"
            "+'</div>';"
    "}"
    "el.innerHTML=html;"
  "}"

  "function addSchedule(){"
    "var start=document.getElementById('tStart').value;"
    "var end=document.getElementById('tEnd').value;"
    "if(!start||!end){showToast('SET BOTH TIMES');return;}"
    "if(start>=end){showToast('START MUST BE BEFORE END');return;}"
    "var cbs=document.querySelectorAll('.day-cb input:checked');"
    "var days=0;"
    "for(var i=0;i<cbs.length;i++) days+=parseInt(cbs[i].value);"
    "if(days===0){showToast('SELECT AT LEAST 1 DAY');return;}"
    "fetch('/schedule/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'start='+start+'&end='+end+'&days='+days})"
      ".then(function(r){"
        "if(r.ok){loadSchedules();showToast('SCHEDULE SAVED');}"
        "else{r.text().then(function(t){showToast('ERR: '+t);});}"
      "})"
      ".catch(function(){showToast('CONNECTION LOST');});"
  "}"

  "function deleteSchedule(id){"
    "fetch('/schedule/delete?id='+id)"
      ".then(function(r){"
        "if(r.ok){loadSchedules();showToast('SCHEDULE REMOVED');}"
        "else{showToast('DELETE FAILED');}"
      "})"
      ".catch(function(){showToast('CONNECTION LOST');});"
  "}"

  "fetch('/status').then(function(r){return r.text();}).then(function(s){if(s==='open'||s==='closed'){setState(s);}}).catch(function(){});"
  "loadSchedules();"
  "</script></body></html>";

// ─── HTTP HANDLERS ────────────────────────────────────────────────────────────
void handleRoot()   { server.send(200, "text/html", INDEX_HTML); }
void handleOpen()   { moveServo(POS_OPEN,   "open");   server.send(200, "text/plain", "OK"); }
void handleClosed() { moveServo(POS_CLOSED, "closed"); server.send(200, "text/plain", "OK"); }
void handleStatus() { server.send(200, "text/plain", currentState); }

void handleWifiStatus() {
  if (WiFi.status() == WL_CONNECTED)
    server.send(200, "text/plain", "online");
  else if (reconnecting)
    server.send(200, "text/plain", "reconnecting");
  else
    server.send(200, "text/plain", "offline");
}

void handleTime() {
  struct tm t;
  if (!getLocalTime(&t)) { server.send(200, "text/plain", "--:--:--"); return; }
  char buf[10];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  server.send(200, "text/plain", buf);
}

void handleGetSchedules() {
  String json = "[";
  for (int i = 0; i < scheduleCount; i++) {
    if (i > 0) json += ",";
    char buf[80];
    snprintf(buf, sizeof(buf),
      "{\"id\":%d,\"start\":\"%02d:%02d\",\"end\":\"%02d:%02d\",\"days\":%d}",
      i, schedules[i].startHour, schedules[i].startMin,
         schedules[i].endHour,   schedules[i].endMin,
         schedules[i].days);
    json += buf;
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleAddSchedule() {
  if (scheduleCount >= MAX_SCHEDULES) { server.send(400, "text/plain", "Max 10 schedules"); return; }
  String startStr = server.arg("start");
  String endStr   = server.arg("end");
  String daysStr  = server.arg("days");
  if (startStr.length() < 5 || endStr.length() < 5 || daysStr.length() == 0) {
    server.send(400, "text/plain", "Bad params"); return;
  }
  Schedule s;
  s.startHour = startStr.substring(0,2).toInt();
  s.startMin  = startStr.substring(3,5).toInt();
  s.endHour   = endStr.substring(0,2).toInt();
  s.endMin    = endStr.substring(3,5).toInt();
  s.days      = (uint8_t)daysStr.toInt();
  s.enabled   = true;
  if (s.startHour>23||s.startMin>59||s.endHour>23||s.endMin>59||s.days==0) {
    server.send(400, "text/plain", "Invalid values"); return;
  }
  schedules[scheduleCount++] = s;
  saveSchedules();
  server.send(200, "text/plain", "OK");
}

void handleDeleteSchedule() {
  int id = server.arg("id").toInt();
  if (id < 0 || id >= scheduleCount) { server.send(400, "text/plain", "Bad id"); return; }
  for (int i = id; i < scheduleCount-1; i++) schedules[i] = schedules[i+1];
  scheduleCount--;
  saveSchedules();
  server.send(200, "text/plain", "OK");
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }

// ─── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  // Power pin – start LOW (servo unpowered)
  pinMode(POWER_PIN, OUTPUT);
  digitalWrite(POWER_PIN, LOW);

  Serial.begin(115200);
  delay(500);

  ESP32PWM::allocateTimer(0);
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 400, 2500);

  // Move to closed on boot (power gates automatically inside moveServo)
  moveServo(POS_CLOSED, "closed");

  loadSchedules();
  Serial.printf("Loaded %d schedule(s) from flash\n", scheduleCount);

  Serial.printf("\nConnecting to %s", SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    // OTA setup
    ArduinoOTA.setHostname("change");
    ArduinoOTA.setPassword("change");   // change this!
    ArduinoOTA.onStart([]()   { Serial.println("[OTA] Starting..."); });
    ArduinoOTA.onEnd([]()     { Serial.println("\n[OTA] Done! Rebooting."); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Error %u\n", e); });
    ArduinoOTA.begin();
    Serial.println("[OTA] Ready. Hostname: esp32-gate");

    configTime(UTC_OFFSET, 0, "pool.ntp.org", "time.nist.gov");
    struct tm t;
    int tries = 0;
    while (!getLocalTime(&t) && tries++ < 20) delay(500);
    if (getLocalTime(&t)) {
      ntpSynced = true;
      Serial.printf("[NTP] Synced: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
    }
  } else {
    Serial.println("\n[WiFi] Initial connect failed – retrying in loop...");
    reconnecting     = true;
    reconnectStartMs = millis();
  }

  server.on("/",                handleRoot);
  server.on("/open",            handleOpen);
  server.on("/closed",          handleClosed);
  server.on("/status",          handleStatus);
  server.on("/wifistatus",      handleWifiStatus);
  server.on("/time",            handleTime);
  server.on("/schedules",       handleGetSchedules);
  server.on("/schedule/add",    HTTP_POST, handleAddSchedule);
  server.on("/schedule/delete", handleDeleteSchedule);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started.");
}

// ─── LOOP ─────────────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();
  handleWifi();
  server.handleClient();
  checkSchedules();
}
